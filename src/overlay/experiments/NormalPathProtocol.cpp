// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "scp/LocalNode.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{
using namespace experiment;

void
OverlayManagerImpl::initializeNormalPath()
{
    if (normalPathEnabled())
    {
        auto const& cfg = mApp.getConfig();
        // Forget-IT / IT-HS run independent instances with native view changes.
        // TetraBFT and Simple-IT have native multi-shot leader schedules.
        if ((FORGET_IT_MODE || ITHS_MODE) && cfg.SEND_CUSTOM_MESSAGE)
            mNormalLeader = cfg.NODE_SEED.getPublicKey();
        mNormalMembers.insert(mApp.getConfig().NODE_SEED.getPublicKey());
        LocalNode::forAllNodes(cfg.QUORUM_SET, [&](NodeID const& id) {
            mNormalMembers.insert(id);
            return true;
        });
        if (FORGET_IT_MODE && (mNormalMembers.size() - 1) % 3 != 0)
            throw std::invalid_argument("Forget-IT Algorithm 1 requires n=3f+1 "
                                        "(e.g. 4, 7, 16, 49 replicas)");
        if (mNormalMembers.size() < 4)
            throw std::invalid_argument(
                "Normal-path protocols need at least four fixed members");
        mNormalLeaders.assign(mNormalMembers.begin(), mNormalMembers.end());
        mNormalQuorum = mNormalMembers.size() - (mNormalMembers.size() - 1) / 3;
        if (ITHS_MODE)
            CLOG_INFO(
                Overlay,
                "[IT-HS-BLOG CONFIG] delta_ms={} clocks=synchronized_wall "
                "lock_evidence=accept initial_lock=view_zero_input",
                ITHS_BLOG_DELTA_MS);
        CLOG_INFO(
            Overlay,
            "[{} CONFIG] members={} quorum={} simple_optimistic={} "
            "native_view_change={} payload={} "
            "simple_commit=round_only rotation=true native_pipeline={}",
            normalPathName(), mNormalMembers.size(), mNormalQuorum,
            SIMPLE_IT_MODE && SIMPLE_IT_OPTIMISTIC, FORGET_IT_MODE || ITHS_MODE,
            (FORGET_IT_MODE || ITHS_MODE) ? "full_values" : "digest_votes",
            !(FORGET_IT_MODE || ITHS_MODE));
    }
}

bool
OverlayManagerImpl::normalPathEnabled() const
{
    return TETRABFT_MODE || SIMPLE_IT_MODE || FORGET_IT_MODE || ITHS_MODE;
}

char const*
OverlayManagerImpl::normalPathName() const
{
    return ITHS_MODE        ? "IT-HS-BLOG"
           : TETRABFT_MODE  ? "TETRA"
           : SIMPLE_IT_MODE ? "SIMPLE-IT"
                            : "FORGET-IT";
}

CustomMessageType
OverlayManagerImpl::normalProposalType() const
{
    return TETRABFT_MODE    ? CUSTOM_TETRA_PROPOSE
           : SIMPLE_IT_MODE ? CUSTOM_SIMPLE_PROPOSE
                            : CUSTOM_FORGET_PREPARE;
}

uint64_t
OverlayManagerImpl::normalMetadata(uint64_t seq) const
{
    // Simple-IT parent round; Forget-IT fresh instance's view 1; Tetra view 0.
    return SIMPLE_IT_MODE ? seq - 1 : FORGET_IT_MODE ? 1 : 0;
}

void
OverlayManagerImpl::normalSend(CustomMessageType type, uint64_t seq,
                               Hash const& digest, std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    auto& cm = msg->customMessage();
    cm.msgType = type;
    cm.view = seq; // block slot / round (independent instance for Forget-IT)
    cm.vp = type == CUSTOM_SIMPLE_COMMIT ? 0 : normalMetadata(seq);
    // Forget-IT's first-view guard is SAFE, explicitly encoded on vote-2.
    cm.dependencyVp = type == CUSTOM_FORGET_VOTE2 ? 1 : 0;
    cm.blockHash = digest;
    cm.data = data;
    if (TETRABFT_MODE && type == normalProposalType())
        cm.bp = mNormalSlots.at(seq).parent;
    // Direct authenticated channels: identical votes from distinct senders
    // must not be collapsed by the overlay's content-based flood cache.
    for (auto const& peer : getAuthenticatedPeers())
        if (mNormalMembers.count(peer.first))
            peer.second->sendMessage(msg);
}

NodeID const&
OverlayManagerImpl::normalLeader(uint64_t seq) const
{
    releaseAssert(seq > 0 && !mNormalLeaders.empty());
    return mNormalLeaders[(seq - 1) % mNormalLeaders.size()];
}

Hash
OverlayManagerImpl::normalDigest(uint64_t seq, Hash const& parent,
                                 std::string const& data) const
{
    if (FORGET_IT_MODE)
        return sha256(data);
    // Bind the TetraBFT block to its parent hash. Simple-IT instead refers to
    // its parent by round, as in Figure 2; it is not a hash-chain wrapper.
    return sha256((TETRABFT_MODE ? std::string("tetra:") : "simple:") +
                  std::to_string(seq) + ":" +
                  (TETRABFT_MODE ? binToHex(parent) : std::to_string(seq - 1)) +
                  ":" + data);
}

bool
OverlayManagerImpl::normalTakeWork(uint64_t seq, std::string& data)
{
    auto const& cfg = mApp.getConfig();
    auto it = mNormalWork.find(seq);
    if (it == mNormalWork.end() && cfg.SEND_CUSTOM_MESSAGE &&
        g_clientListenerActive)
    {
        TransactionBatch batch;
        std::vector<ClientAck> acks;
        if (tryBuildServerBatch(batch, acks))
        {
            auto payload = batch.serialize();
            g_blockClientAcks[BlockKey{seq, sha256(payload)}] = std::move(acks);
            it = mNormalWork.emplace(seq, NormalWork{std::move(payload)}).first;
        }
        else if (TETRABFT_MODE && mNormalLastReal > mNormalExecuted &&
                 seq > mNormalLastReal && seq - mNormalLastReal <= 3)
        {
            // Empty descendants finalize the last real batch even with only
            // one outstanding client request. They contain no synthetic txns.
            it = mNormalWork.emplace(seq, NormalWork{}).first;
        }
    }
    if (it == mNormalWork.end())
        return false;
    auto const& leader = normalLeader(seq);
    if (leader == cfg.NODE_SEED.getPublicKey())
    {
        data = it->second.data;
        return true;
    }
    if (cfg.SEND_CUSTOM_MESSAGE)
    {
        auto now = std::chrono::steady_clock::now();
        if (it->second.forwarded == std::chrono::steady_clock::time_point{} ||
            now - it->second.forwarded >= std::chrono::seconds(1))
        {
            auto peers = getAuthenticatedPeers();
            auto peer = peers.find(leader);
            if (peer != peers.end())
            {
                auto msg = std::make_shared<StellarMessage>();
                msg->type(CUSTOM_MESSAGE);
                auto& cm = msg->customMessage();
                cm.msgType = CUSTOM_EXPERIMENT_WORK;
                cm.view = seq;
                cm.data = it->second.data;
                cm.blockHash = sha256(cm.data);
                peer->second->sendMessage(msg);
                it->second.forwarded = now;
            }
        }
    }
    return false;
}

void
OverlayManagerImpl::normalReceiveWork(CustomMessage const& cm,
                                      NodeID const& sender)
{
    auto seq = cm.view;
    if (!mNormalMembers.count(sender) || seq <= mNormalExecuted ||
        seq - mNormalExecuted > NORMAL_PATH_WINDOW ||
        !(normalLeader(seq) == mApp.getConfig().NODE_SEED.getPublicKey()) ||
        mNormalWork.count(seq) || sha256(cm.data) != cm.blockHash)
        return;
    try
    {
        auto batch = TransactionBatch::deserialize(cm.data);
        if ((batch.transactions.size() != SERVER_BATCH_SIZE &&
             !(TETRABFT_MODE && cm.data.empty())) ||
            batch.serialize() != cm.data)
            return;
    }
    catch (std::exception const&)
    {
        return;
    }
    mNormalWork.emplace(seq, NormalWork{cm.data});
    normalProp();
}

void
OverlayManagerImpl::normalProp()
{
    uint64_t seq =
        (FORGET_IT_MODE || ITHS_MODE) ? mNormalNextSeq : mNormalReady + 1;
    auto existing = mNormalSlots.find(seq);
    if (existing != mNormalSlots.end() && existing->second.proposed)
        return;
    std::string data;
    if (FORGET_IT_MODE || ITHS_MODE)
    {
        if (!mApp.getConfig().SEND_CUSTOM_MESSAGE || seq != mNormalExecuted + 1)
            return;
        TransactionBatch batch;
        std::vector<ClientAck> acks;
        if (!g_clientListenerActive || !tryBuildServerBatch(batch, acks))
            return;
        data = batch.serialize();
        g_blockClientAcks[BlockKey{seq, sha256(data)}] = std::move(acks);
        ++mNormalNextSeq;
        if (ITHS_MODE)
            startITHSBlog(seq, data,
                          0); // ingress creates and distributes a common epoch
        else
            startForgetIT(seq, data);
        normalAdvance(seq);
        return;
    }
    else if (!normalTakeWork(seq, data))
        return;
    Hash parent{};
    if (TETRABFT_MODE && seq > 1)
        parent = mNormalSlots.at(seq - 1).digest;
    auto digest = normalDigest(seq, parent, data);
    auto& slot = mNormalSlots[seq];
    slot.digest = digest;
    slot.parent = parent;
    if (!data.empty())
        mNormalLastReal = std::max(mNormalLastReal, seq);
    if (SIMPLE_IT_MODE)
        slot.simple.propose(digest);
    slot.proposed = true;
    slot.data = data;
    normalSend(normalProposalType(), seq, digest, data);
    normalAdvance(seq); // leader casts vote-1 as well
}

void
OverlayManagerImpl::handleNormalMessage(StellarMessage const& msg,
                                        NodeID const& sender)
{
    if (FORGET_IT_MODE || ITHS_MODE)
        return; // Native one-shot packets have a separate decoder and view
                // state.
    auto const& cm = msg.customMessage();
    auto seq = cm.view;
    // Bounded lookahead permits a faster replica to start the next instance
    // before this replica has received the last vote of the previous one.
    // Simple-IT continues the RBC's auxiliary messages briefly after execution.
    bool retainedSimple =
        !FORGET_IT_MODE && seq <= mNormalExecuted && mNormalSlots.count(seq);
    if (!mNormalMembers.count(sender) || seq == 0 ||
        cm.vp != (cm.msgType == CUSTOM_SIMPLE_COMMIT ? 0 : normalMetadata(seq)) ||
        (!retainedSimple && seq <= mNormalExecuted) ||
        (seq > mNormalExecuted && seq - mNormalExecuted > NORMAL_PATH_WINDOW))
        return;
    if (cm.msgType == normalProposalType())
    {
        bool wrongLeader =
            FORGET_IT_MODE ? ((mNormalLeader && !(sender == *mNormalLeader)) ||
                              (!mNormalLeader && seq != 1))
                           : !(sender == normalLeader(seq));
        if (wrongLeader || normalDigest(seq, cm.bp, cm.data) != cm.blockHash ||
            (!TETRABFT_MODE && cm.bp != Hash{}))
            return;
        try
        {
            auto batch = TransactionBatch::deserialize(cm.data);
            if ((batch.transactions.size() != SERVER_BATCH_SIZE &&
                 !(TETRABFT_MODE && cm.data.empty())) ||
                batch.serialize() != cm.data)
                return;
        }
        catch (std::exception const&)
        {
            return;
        }
        auto& slot = mNormalSlots[seq];
        bool accepted = TETRABFT_MODE
                            ? (!slot.proposed || slot.digest == cm.blockHash)
                            : slot.simple.propose(cm.blockHash);
        if (!accepted)
            return;
        // Bind only after membership, metadata, digest and payload validation.
        if (FORGET_IT_MODE && !mNormalLeader)
            mNormalLeader = sender;
        slot.digest = cm.blockHash;
        slot.proposed = true;
        slot.data = cm.data;
        slot.parent = cm.bp;
        if (!cm.data.empty())
            mNormalLastReal = std::max(mNormalLastReal, seq);
    }
    else
    {
        bool roundCommit = SIMPLE_IT_MODE && cm.msgType == CUSTOM_SIMPLE_COMMIT;
        // Votes refer to the validated proposal by digest; do not retransmit
        // its batch. Simple-IT COMMIT refers only to the round.
        if (roundCommit)
        {
            if (!cm.data.empty() || cm.blockHash != Hash{})
                return;
        }
        else if (!cm.data.empty())
            return;
        if (TETRABFT_MODE)
        {
            if (cm.msgType != CUSTOM_TETRA_VOTE1)
                return; // one vote per block in the native multi-shot path
            mNormalSlots[seq].tetraVotes.emplace(sender, cm.blockHash);
        }
        else if (SIMPLE_IT_MODE)
        {
            if (!SIMPLE_IT_OPTIMISTIC && (cm.msgType == CUSTOM_SIMPLE_VOTE ||
                                          cm.msgType == CUSTOM_SIMPLE_ACK))
                return;
            if (roundCommit)
                mNormalSlots[seq].simple.commit(sender);
            else
                mNormalSlots[seq].simple.vote(cm.msgType - CUSTOM_SIMPLE_ECHO + 1,
                                              sender, cm.blockHash);
        }
    }
    normalAdvance(seq);
}

void
OverlayManagerImpl::normalAdvance(uint64_t seq)
{
    // Reconsider the affected slot, then only children unblocked by it.
    // Do not rescan the retained history on every vote in the fast path.
    auto current = mNormalSlots.find(seq);
    while (current != mNormalSlots.end() && current->second.proposed)
    {
        auto round = current->first;
        auto oldReady = mNormalReady;
        auto oldNotarized = mNormalNotarized;
        bool decided = ITHS_MODE        ? advanceITHSBlog(round)
                       : TETRABFT_MODE  ? advanceTetraBFT(round)
                       : FORGET_IT_MODE ? advanceForgetIT(round)
                                        : advanceSimpleIT(round);
        if (decided)
        {
            if (FORGET_IT_MODE || ITHS_MODE)
                current->second.decided = true;
            else
            {
                // Simple-IT delivers Log(round); TetraBFT finalizes the prefix
                // ending three ancestors behind a newly notarized block.
                uint64_t last = TETRABFT_MODE ? round - 3 : round;
                for (auto it = mNormalSlots.upper_bound(mNormalExecuted);
                     it != mNormalSlots.end() && it->first <= last; ++it)
                    it->second.decided = true;
            }
        }
        if (FORGET_IT_MODE || ITHS_MODE ||
            (TETRABFT_MODE && oldNotarized == mNormalNotarized) ||
            (SIMPLE_IT_MODE && oldReady == mNormalReady))
            break;
        ++current;
        if (current != mNormalSlots.end() && current->first != round + 1)
            break;
    }

    // Execution is ordered even if network delivery reorders decisions.
    while (true)
    {
        auto it = mNormalSlots.find(mNormalExecuted + 1);
        if (it == mNormalSlots.end() || !it->second.decided)
            break;
        auto const& ready = it->second;
        auto batch = TransactionBatch::deserialize(ready.data);
        for (auto const& txn : batch.transactions)
        {
            std::istringstream input(txn.payload);
            std::string op, key, value;
            input >> op >> key;
            if (op == "READ")
            {
                auto found = g_kvStore.find(key);
                (void)found;
            }
            else if (op == "UPDATE" || op == "RMW")
            {
                input >> value;
                g_kvStore[key] = value;
            }
        }
        ++mNormalExecuted;
        CLOG_INFO(Overlay, "[{} COMMITTED] seq={} digest={} txns={}",
                  normalPathName(), mNormalExecuted, hexAbbrev(ready.digest),
                  batch.transactions.size());
        if (mApp.getConfig().SEND_CUSTOM_MESSAGE)
            ackClientBatchesForBlock(mNormalExecuted, sha256(ready.data));
        mNormalWork.erase(mNormalExecuted);
    }
    {
        // Bounded benchmark retention lets late messages finish without re-execution.
        while (!mNormalSlots.empty() && mNormalExecuted > NORMAL_PATH_WINDOW &&
               mNormalSlots.begin()->first <
                   mNormalExecuted - NORMAL_PATH_WINDOW)
            mNormalSlots.erase(mNormalSlots.begin());
    }
    normalProp();
}

} // namespace stellar
