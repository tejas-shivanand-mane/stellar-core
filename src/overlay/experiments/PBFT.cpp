// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/SecretKey.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "scp/LocalNode.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{
using namespace experiment;

namespace
{
enum Kind : uint32_t
{
    ClientWork,
    Proposal,
    Prepare,
    Commit,
    ViewChange,
    NewView,
    Fetch,
    Decision,
    Status
};
using Clock = std::chrono::steady_clock;
using Votes = std::map<uint32_t, PbftVote>;

Hash
valueDigest(uint64_t originView, std::string const& value)
{
    return sha256(xdr::xdr_to_opaque(originView, xdr::xstring<90000>(value)));
}

bool
validBatch(std::string const& value)
{
    if (value.empty() || value.size() > 90000)
        return false;
    try
    {
        auto b = TransactionBatch::deserialize(value);
        return b.transactions.size() == SERVER_BATCH_SIZE &&
               b.serialize() == value;
    }
    catch (std::exception const&)
    {
        return false;
    }
}
}

// A replica votes only on its next unexecuted sequence number. Its previous
// decision is an implicit low watermark. This makes a view-change report
// contain at most one prepared certificate. Peers behind the watermark can
// request the ordered decisions, each accompanied by a commit certificate.
struct OverlayManagerImpl::PbftRuntime
{
    OverlayManagerImpl& owner;
    std::vector<NodeID> members;
    uint32_t self;
    size_t quorum;
    Hash domain;
    uint64_t seq = 1, base = 1, view = 1;
    bool changing = false;
    bool synthetic = false;
    Clock::time_point deadline{}, retry{}, lastFetch{}, lastRepeat{},
        lastStatus{};
    uint64_t highestSeen = 1;
    std::string pending; // ingress keeps its client requests until execution
    std::vector<ClientAck> acks;
    std::string candidate;
    std::optional<PbftPacket> proposal, prepared, required, ownChange;
    Votes prepares, commits;
    std::map<uint32_t, std::pair<Hash, PbftVote>> earlyPrepares, earlyCommits;
    std::map<uint32_t, uint64_t> requested;
    std::map<uint32_t, PbftPacket> changes;
    std::map<uint64_t, PbftPacket> history;
    std::map<uint32_t, std::vector<PbftPacket>> nextPackets;
    bool sentNewView = false;

    explicit PbftRuntime(OverlayManagerImpl& o) : owner(o)
    {
        auto const& cfg = owner.mApp.getConfig();
        std::set<NodeID> ids{cfg.NODE_SEED.getPublicKey()};
        LocalNode::forAllNodes(cfg.QUORUM_SET, [&](NodeID const& id) {
            ids.insert(id);
            return true;
        });
        members.assign(ids.begin(), ids.end());
        if (members.size() < 4 || members.size() > 256)
            throw std::invalid_argument("PBFT requires 4..256 fixed replicas");
        self = std::lower_bound(members.begin(), members.end(),
                                cfg.NODE_SEED.getPublicKey()) -
               members.begin();
        quorum = members.size() - (members.size() - 1) / 3;
        xdr::xvector<NodeID, 256> roster(members.begin(), members.end());
        domain = sha256(xdr::xdr_to_opaque(xdr::xstring<>("stellar-pbft-v1"),
                                           owner.mApp.getNetworkID(), roster,
                                           bool(PBFT_ROTATING)));
        CLOG_INFO(Overlay,
                  "[PBFT CONFIG] protocol={} quorum={} timeout_ms={} "
                  "rotation_batches={} primary={} signatures=ed25519",
                  PROTOCOL, quorum, PBFT_TIMEOUT_MS, PBFT_ROTATING ? 1 : 0,
                  KeyUtils::toShortString(members[0]));
    }

    uint32_t
    leader(uint64_t v) const
    {
        return (v - 1) % members.size();
    }

    // On the normal path the primary's signed proposal and Q-1 distinct
    // backup prepares form a transferable prepared certificate.
    auto
    statement(uint32_t kind, uint64_t v, Hash const& digest,
              uint64_t preparedView = 0) const
    {
        return xdr::xdr_to_opaque(domain, kind, seq, v, digest, preparedView);
    }

    PbftVote
    sign(uint32_t k, uint64_t v, Hash const& d, uint64_t pv = 0)
    {
        PbftVote a;
        a.signer = self;
        a.signature =
            owner.mApp.getConfig().NODE_SEED.sign(statement(k, v, d, pv));
        return a;
    }

    bool
    verify(PbftVote const& a, uint32_t k, uint64_t v, Hash const& d,
           uint64_t pv = 0) const
    {
        return a.signer < members.size() &&
               PubKeyUtils::verifySig(members[a.signer], a.signature,
                                      statement(k, v, d, pv));
    }

    void
    send(PbftPacket const& p, std::optional<uint32_t> recipient = {},
         std::optional<uint64_t> sequence = {})
    {
        auto bytes = xdr::xdr_to_opaque(p);
        auto msg = std::make_shared<StellarMessage>();
        msg->type(CUSTOM_MESSAGE);
        auto& cm = msg->customMessage();
        cm.msgType = CUSTOM_PBFT_NATIVE;
        cm.view = sequence.value_or(seq);
        cm.data.assign(bytes.begin(), bytes.end());
        transmit(msg, recipient);
    }

    void
    transmit(std::shared_ptr<StellarMessage> const& msg,
             std::optional<uint32_t> recipient)
    {
        auto peers = owner.getAuthenticatedPeers();
        if (recipient)
        {
            auto it = peers.find(members[*recipient]);
            if (it != peers.end())
                it->second->sendMessage(msg);
        }
        else
            for (auto const& [id, peer] : peers)
                if (std::binary_search(members.begin(), members.end(), id))
                    peer->sendMessage(msg);
    }

    PbftPacket
    packet(Kind k) const
    {
        PbftPacket p{};
        p.kind = k;
        p.view = view;
        return p;
    }

    void
    arm()
    {
        if (deadline == Clock::time_point{})
        {
            auto planned = PBFT_ROTATING && seq > 1 ? 1u : 0u;
            auto shifts = std::min<uint64_t>(view - base - planned, 10);
            deadline = Clock::now() +
                       std::chrono::milliseconds(PBFT_TIMEOUT_MS << shifts);
        }
    }

    bool
    validValue(PbftPacket const& p, uint64_t atView) const
    {
        return p.valueView >= base && p.valueView <= atView &&
               validBatch(p.value) &&
               valueDigest(p.valueView, p.value) == p.digest;
    }

    bool
    certificate(PbftPacket const& p, uint64_t atView, bool decision) const
    {
        if (p.proof.size() != quorum || !validValue(p, atView))
            return false;
        std::set<uint32_t> seen;
        for (auto const& a : p.proof)
        {
            uint32_t k =
                decision ? Commit
                         : (a.signer == leader(atView) ? Proposal : Prepare);
            if (!seen.insert(a.signer).second ||
                !verify(a, k, atView, p.digest))
                return false;
        }
        return decision || seen.count(leader(atView));
    }

    void
    input()
    {
        if (!owner.mApp.getConfig().SEND_CUSTOM_MESSAGE ||
            (!g_clientListenerActive && !synthetic))
            return;
        if (pending.empty())
        {
            TransactionBatch b;
            if (g_clientListenerActive)
            {
                if (!tryBuildServerBatch(b, acks))
                    return;
            }
            else
            {
                b = makeSyntheticYCSBBatch(
                    owner.mApp, KeyUtils::toShortString(members[self]),
                    txn_count, SERVER_BATCH_SIZE);
            }
            txn_count += b.transactions.size();
            pending = b.serialize();
            if (!validBatch(pending))
                throw std::invalid_argument(
                    "PBFT client batch exceeds the wire limit or is invalid");
        }
        auto now = Clock::now();
        if (retry > now)
            return;
        bool retransmit = retry != Clock::time_point{};
        retry = now + std::chrono::milliseconds(
                          std::max<uint64_t>(100, PBFT_TIMEOUT_MS / 2));
        candidate = pending;
        arm();
        if (leader(view) != self || retransmit)
        {
            auto w = packet(ClientWork);
            w.value = pending;
            // Wake backups if a primary stops before sending its proposal.
            if (retransmit)
                send(w);
            else
                send(w, leader(view));
        }
        propose();
    }

    void
    propose()
    {
        if (changing || self != leader(view) || proposal ||
            (!required && candidate.empty()))
            return;
        auto p = packet(Proposal);
        p.valueView = required ? required->valueView : view;
        p.value = required ? required->value : candidate;
        p.digest = valueDigest(p.valueView, p.value);
        p.proof.push_back(sign(Proposal, view, p.digest));
        send(p);
        normal(self, p);
    }

    void
    advance()
    {
        if (changing || !proposal)
            return;
        if (prepares.size() >= quorum && (!prepared || prepared->view != view))
        {
            prepared = proposal;
            prepared->proof.clear();
            // Always include the primary, even if its index is last.
            prepared->proof.push_back(proposal->proof.front());
            for (auto const& [id, a] : prepares)
                if (id != leader(view) && prepared->proof.size() < quorum)
                    prepared->proof.push_back(a);
            auto c = packet(Commit);
            c.digest = proposal->digest;
            c.proof.push_back(sign(Commit, view, c.digest));
            commits.emplace(self, c.proof.front());
            send(c);
        }
        if (prepared && prepared->view == view && commits.size() >= quorum)
        {
            auto d = *proposal;
            d.kind = Decision;
            d.proof.clear();
            for (auto const& [id, a] : commits)
                if (d.proof.size() < quorum)
                    d.proof.push_back(a);
            execute(d);
        }
    }

    void
    normal(uint32_t sender, PbftPacket const& p)
    {
        if (p.view != view || p.proof.size() != 1 || !p.reports.empty() ||
            p.proof.front().signer != sender)
            return;
        if (p.kind == Proposal)
        {
            if (sender != leader(view) || proposal || !validValue(p, view) ||
                (!changing && !allowedProposal(p)) ||
                !verify(p.proof.front(), Proposal, view, p.digest))
                return;
            proposal = p;
            if (changing)
                return; // NEW-VIEW may be delivered after PRE-PREPARE
            acceptProposal();
        }
        else
        {
            if ((p.kind == Prepare && sender == leader(view)) ||
                !p.value.empty() || p.valueView != 0 ||
                !verify(p.proof.front(), p.kind, view, p.digest))
                return;
            if (changing || !proposal)
            {
                auto& early = p.kind == Prepare ? earlyPrepares : earlyCommits;
                early.emplace(sender,
                              std::make_pair(p.digest, p.proof.front()));
                return;
            }
            if (proposal->digest != p.digest)
                return;
            (p.kind == Prepare ? prepares : commits)
                .emplace(sender, p.proof.front());
            advance();
        }
    }

    bool
    allowedProposal(PbftPacket const& p) const
    {
        return required ? p.digest == required->digest : p.valueView == view;
    }

    void
    acceptProposal()
    {
        auto const& p = *proposal;
        arm();
        prepares.emplace(leader(view), p.proof.front());
        for (auto const& [id, vote] : earlyPrepares)
            if (vote.first == p.digest)
                prepares.emplace(id, vote.second);
        for (auto const& [id, vote] : earlyCommits)
            if (vote.first == p.digest)
                commits.emplace(id, vote.second);
        earlyPrepares.clear();
        earlyCommits.clear();
        if (leader(view) != self)
        {
            auto m = packet(Prepare);
            m.digest = p.digest;
            m.proof.push_back(sign(Prepare, view, m.digest));
            prepares.emplace(self, m.proof.front());
            send(m);
        }
        advance();
    }

    void
    clearRound()
    {
        proposal.reset();
        required.reset();
        ownChange.reset();
        prepares.clear();
        commits.clear();
        earlyPrepares.clear();
        earlyCommits.clear();
        changes.clear();
        sentNewView = false;
        deadline = {};
    }

    void
    change(uint64_t target)
    {
        if (target <= view)
            return;
        clearRound(); // keep the highest prepared certificate across views
        view = target;
        changing = true;
        arm();
        auto p = packet(ViewChange);
        PbftReport r{};
        if (prepared)
        {
            r.preparedView = prepared->view;
            r.digest = p.digest = prepared->digest;
            p.valueView = prepared->valueView;
            p.value = prepared->value;
            p.proof = prepared->proof;
        }
        r.author = sign(ViewChange, view, r.digest, r.preparedView);
        p.reports.push_back(r);
        CLOG_INFO(Overlay, "[PBFT VIEW-CHANGE] seq={} view={} prepared_view={}",
                  seq, view, r.preparedView);
        ownChange = p;
        send(p);
        recordChange(self, p);
    }

    bool
    validReport(PbftReport const& r, uint64_t target) const
    {
        return (r.preparedView
                    ? (r.preparedView >= base && r.preparedView < target)
                    : r.digest == Hash{}) &&
               verify(r.author, ViewChange, target, r.digest, r.preparedView);
    }

    // Validate the individual reports at the new primary. NEW-VIEW carries
    // all Q signed summaries and only the highest prepared certificate. Every
    // backup recomputes that choice and verifies the same transferable proof.
    void
    recordChange(uint32_t sender, PbftPacket const& p)
    {
        if (p.view <= base || p.view < view || p.reports.size() != 1)
            return;
        auto const& r = p.reports.front();
        if (r.author.signer != sender || r.digest != p.digest ||
            !validReport(r, p.view))
            return;
        if (r.preparedView
                ? !certificate(p, r.preparedView, false)
                : (!p.proof.empty() || !p.value.empty() || p.valueView))
            return;
        auto& wanted = requested[sender];
        wanted = std::max(wanted, p.view);
        // Refuse a view jump based on a single replica's request.
        if (p.view > view)
        {
            std::vector<uint64_t> ahead;
            for (auto const& [id, v] : requested)
                if (v > view)
                    ahead.push_back(v);
            auto faults = members.size() - quorum;
            if (ahead.size() > faults)
            {
                // At least f+1 distinct members requested this view or later.
                std::sort(ahead.begin(), ahead.end(), std::greater<uint64_t>());
                change(ahead[faults]);
            }
        }
        if (!changing || view != p.view || leader(view) != self || sentNewView)
            return;
        changes.emplace(sender, p);
        if (changes.size() < quorum)
            return;
        auto nv = packet(NewView);
        uint64_t high = 0;
        for (auto const& [id, vc] : changes)
        {
            auto const& report = vc.reports.front();
            nv.reports.push_back(report);
            if (report.preparedView > high)
            {
                high = report.preparedView;
                nv.digest = vc.digest;
                nv.valueView = vc.valueView;
                nv.value = vc.value;
                nv.proof = vc.proof;
            }
        }
        changes.clear();
        sentNewView = true;
        send(nv);
        install(self, nv);
    }

    void
    install(uint32_t sender, PbftPacket const& p)
    {
        if (p.view <= base || p.view < view || (p.view == view && !changing) ||
            leader(p.view) != sender || p.reports.size() != quorum)
            return;
        std::set<uint32_t> seen;
        uint64_t high = 0;
        Hash selected{};
        for (auto const& r : p.reports)
        {
            if (!seen.insert(r.author.signer).second || !validReport(r, p.view))
                return;
            if (r.preparedView > high)
            {
                selected = r.digest;
                high = r.preparedView;
            }
            else if (r.preparedView == high && r.digest != selected)
                return;
        }
        if (p.digest != selected ||
            (high ? !certificate(p, high, false)
                  : (!p.proof.empty() || !p.value.empty() || p.valueView)))
            return;
        if (view != p.view)
            clearRound();
        view = p.view;
        changing = false;
        ownChange.reset();
        if (high)
            required = p;
        CLOG_INFO(Overlay,
                  "[PBFT NEW-VIEW] seq={} view={} primary={} reports={} "
                  "prepared_view={}",
                  seq, view, KeyUtils::toShortString(members[sender]),
                  seen.size(), high);
        deadline = {};
        if (required || !candidate.empty() || !pending.empty())
            arm();
        if (proposal && !allowedProposal(*proposal))
            proposal.reset();
        if (proposal)
            acceptProposal();
        else
            propose();
    }

    void
    execute(PbftPacket const& d)
    {
        auto b = TransactionBatch::deserialize(d.value);
        for (auto const& txn : b.transactions)
        {
            std::istringstream input(txn.payload);
            std::string op, key, value;
            input >> op >> key;
            if (op == "READ")
                (void)g_kvStore.find(key);
            else if ((op == "UPDATE" || op == "RMW") && (input >> value))
                g_kvStore[key] = value;
        }
        if (!pending.empty() && pending == d.value)
        {
            if (!acks.empty())
            {
                auto h = sha256(pending);
                g_blockClientAcks[BlockKey{seq, h}] = std::move(acks);
                ackClientBatchesForBlock(seq, h);
            }
            pending.clear();
            acks.clear();
        }
        CLOG_INFO(Overlay,
                  "[PBFT COMMITTED-LOCAL] view={} seq={} digest={} txns={}",
                  d.view, seq, hexAbbrev(d.digest), b.transactions.size());
        history.emplace(seq, d);
        if (history.size() > PBFT_HISTORY)
            history.erase(history.begin());
        // Bind the next sequence's initial primary to the decided value.
        // Replicas can learn the same value in different commit views.
        auto lastView = std::max(view, d.view);
        base = d.valueView;
        ++seq;
        view = base;
        clearRound();
        prepared.reset();
        requested.clear();
        candidate.clear();
        changing = false;
        retry = {};
        auto early = std::move(nextPackets);
        nextPackets.clear();
        auto replaySeq = seq;
        // A carried-over value may have originated under a failed primary.
        // Keep the installed view (rotate from it in the scheduled policy).
        // Reconcile different commit views through the same certified handoff.
        auto target = lastView + (PBFT_ROTATING ? 1 : 0);
        if (target > base)
            change(target);
        for (auto const& [id, packets] : early)
            for (auto const& p : packets)
                if (seq == replaySeq)
                    receive(id, replaySeq, p);
        input();
    }

    void
    fetch()
    {
        auto now = Clock::now();
        if (now - lastFetch >= std::chrono::milliseconds(250))
        {
            lastFetch = now;
            send(packet(Fetch));
        }
    }

    void
    receive(uint32_t sender, uint64_t s, PbftPacket const& p)
    {
        if (!s || !p.view || p.kind > Status)
            return;
        if (p.kind == Status)
        {
            if (s > seq)
            {
                highestSeen = std::max(highestSeen, s);
                fetch();
            }
            return;
        }
        if (p.kind == Fetch)
        {
            auto i = history.find(s);
            if (i != history.end())
                send(i->second, sender, s);
            return;
        }
        if (s < seq)
            return;
        if (s > seq)
        {
            highestSeen = std::max(highestSeen, s);
            // Peers can execute a batch a few messages apart. Replay bounded
            // next-slot traffic immediately after execution, in arrival order
            // per sender. Request a certified decision for longer gaps.
            if (s == seq + 1 && nextPackets[sender].size() < 8)
                nextPackets[sender].push_back(p);
            if (s > seq + 1)
                fetch();
            return;
        }
        switch (p.kind)
        {
        case ClientWork:
            if (candidate.empty() && validBatch(p.value))
                candidate = p.value;
            if (!candidate.empty())
            {
                arm();
                propose();
            }
            break;
        case Proposal:
        case Prepare:
        case Commit:
            normal(sender, p);
            break;
        case ViewChange:
            recordChange(sender, p);
            break;
        case NewView:
            install(sender, p);
            break;
        case Decision:
            if (p.reports.empty() && p.view >= base &&
                certificate(p, p.view, true))
                execute(p);
            break;
        }
    }

    void
    tick()
    {
        auto now = Clock::now();
        // A reconnecting replica can miss the last decision of an idle run.
        // Advertise just the next sequence number. Payloads are sent on demand.
        if (now - lastStatus >= std::chrono::seconds(1))
        {
            lastStatus = now;
            send(packet(Status));
        }
        if (highestSeen > seq)
            fetch();
        if (deadline != Clock::time_point{} && now >= deadline)
        {
            fetch();
            change(view + 1);
        }
        else if (changing && now - lastRepeat >= std::chrono::milliseconds(250))
        {
            // An early request may have arrived before its target entered the
            // view. Repeat our own signed report so a live primary can finish.
            lastRepeat = now;
            if (ownChange)
                send(*ownChange);
        }
        input();
    }
};

void
OverlayManagerImpl::initializePBFT()
{
    mPbft = std::make_shared<PbftRuntime>(*this);
}

void
OverlayManagerImpl::pbftProp()
{
    if (mPbft)
    {
        mPbft->synthetic = !g_clientListenerActive;
        mPbft->input();
    }
}

void
OverlayManagerImpl::tickPBFT()
{
    if (!mPbft || mShuttingDown)
        return;
    if (!mPbftTimer)
    {
        mPbftTimer = std::make_unique<VirtualTimer>(mApp);
        armPbftTimer();
    }
    mPbft->tick();
}

void
OverlayManagerImpl::armPbftTimer()
{
    mPbftTimer->expires_from_now(
        std::chrono::milliseconds(std::min<uint64_t>(100, PBFT_TIMEOUT_MS)));
    mPbftTimer->async_wait(
        [this]() {
            if (!mShuttingDown)
            {
                mPbft->tick();
                armPbftTimer();
            }
        },
        VirtualTimer::onFailureNoop);
}

void
OverlayManagerImpl::handlePBFTMessage(CustomMessage const& cm,
                                      NodeID const& sender)
{
    if (!mPbft)
        return;
    auto i =
        std::lower_bound(mPbft->members.begin(), mPbft->members.end(), sender);
    if (i == mPbft->members.end() || !(*i == sender))
        return;
    PbftPacket p;
    try
    {
        std::vector<char> bytes(cm.data.begin(), cm.data.end());
        xdr::xdr_from_opaque(bytes, p);
    }
    catch (xdr::xdr_runtime_error const&)
    {
        return;
    }
    mPbft->receive(i - mPbft->members.begin(), cm.view, p);
}
} // namespace stellar
