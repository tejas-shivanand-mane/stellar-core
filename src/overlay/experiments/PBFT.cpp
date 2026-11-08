// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{
using namespace experiment;

struct PbftSlotKey
{
    uint64_t view; // PBFT view, fixed to 0 without view change
    uint64_t seq;  // PBFT sequence number

    bool
    operator==(PbftSlotKey const& other) const noexcept
    {
        return view == other.view && seq == other.seq;
    }
};

struct PbftSlotKeyHash
{
    size_t
    operator()(PbftSlotKey const& k) const noexcept
    {
        return std::hash<uint64_t>()(k.view) ^
               (std::hash<uint64_t>()(k.seq) << 1);
    }
};

struct HashHash
{
    size_t
    operator()(Hash const& h) const noexcept
    {
        size_t out = 0;
        for (auto b : h)
        {
            out = (out * 131) ^ b;
        }
        return out;
    }
};

struct PbftState
{
    bool prePrepared = false;
    bool prepareSent = false;
    bool prepared = false;
    bool commitSent = false;
    bool committedLocal = false;
    bool executed = false;

    Hash digest{};
    std::string data;

    std::unordered_map<Hash, std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       HashHash>
        prepareVotersByDigest;

    std::unordered_map<Hash, std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       HashHash>
        commitVotersByDigest;
};

static size_t
pbftPrepareCount(PbftState const& st, Hash const& digest)
{
    auto it = st.prepareVotersByDigest.find(digest);
    if (it == st.prepareVotersByDigest.end())
    {
        return 0;
    }
    return it->second.size();
}

static size_t
pbftCommitCount(PbftState const& st, Hash const& digest)
{
    auto it = st.commitVotersByDigest.find(digest);
    if (it == st.commitVotersByDigest.end())
    {
        return 0;
    }
    return it->second.size();
}

static std::unordered_map<PbftSlotKey, PbftState, PbftSlotKeyHash> g_pbft;
static std::unordered_map<PbftSlotKey, Hash, PbftSlotKeyHash>
    g_pbftAcceptedDigest;

static uint64_t g_pbftView = 0;
static uint64_t g_pbftNextSeq = 1;
static uint64_t g_pbftLastExecuted = 0;

static Hash
pbftDigest(std::string const& data)
{
    return sha256(data);
}

void
OverlayManagerImpl::pbftProp()
{

    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    std::string shortID = KeyUtils::toShortString(selfID);

    if (!mApp.getConfig().SEND_CUSTOM_MESSAGE)
    {
        return;
    }

    if (g_pbftNextSeq != g_pbftLastExecuted + 1)
    {
        CLOG_DEBUG(Overlay,
                   "[PBFT PROP SKIP] one slot already in flight: "
                   "nextSeq={} lastExecuted={}",
                   g_pbftNextSeq, g_pbftLastExecuted);
        return;
    }

    TransactionBatch batch;
    bool fromExternalClient = false;
    std::vector<ClientAck> clientAcks;

    if (g_clientListenerActive)
    {
        if (!tryBuildServerBatch(batch, clientAcks))
        {
            CLOG_DEBUG(Overlay,
                       "[PBFT CLIENT] Only {} queued requests; need {}",
                       getClientRequestQueueSize(), SERVER_BATCH_SIZE);
            return;
        }

        fromExternalClient = true;

        releaseAssert(batch.transactions.size() == SERVER_BATCH_SIZE);
        releaseAssert(clientAcks.size() == SERVER_BATCH_SIZE);
    }
    else
    {
        batch =
            makeSyntheticYCSBBatch(mApp, shortID, txn_count, SERVER_BATCH_SIZE);
    }

    if (batch.transactions.empty())
    {
        CLOG_WARNING(Overlay, "[PBFT PROP SKIP] empty batch");
        return;
    }

    txn_count += batch.transactions.size();

    std::string data = batch.serialize();
    Hash digest = pbftDigest(data);

    CLOG_DEBUG(
        Overlay, "[PBFT BATCH SIZE] txns={} data_bytes={} bytes_per_txn={}",
        batch.transactions.size(), data.size(),
        batch.transactions.empty() ? 0
                                   : data.size() / batch.transactions.size());

    uint64_t view = g_pbftView;
    uint64_t seq = g_pbftNextSeq++;

    PbftSlotKey slot{view, seq};
    auto& st = g_pbft[slot];

    st.prePrepared = true;
    st.digest = digest;
    st.data = data;

    g_pbftAcceptedDigest[slot] = digest;

    if (fromExternalClient)
    {
        // For benchmark compatibility with current shab_client:
        // use seq as the "view" field in existing ACK map.
        BlockKey ackKey{seq, digest};
        auto& acks = g_blockClientAcks[ackKey];
        acks.insert(acks.end(), clientAcks.begin(), clientAcks.end());
    }

    CLOG_INFO(
        Overlay,
        "[PBFT SEND PRE-PREPARE] view={} seq={} digest={} txns={} source={}",
        view, seq, hexAbbrev(digest), batch.transactions.size(),
        fromExternalClient ? "external-client" : "synthetic");

    sendPBFTPrePrepare(view, seq, digest, data);

    // The primary does not send PREPARE;
    // replicas use a 2f PREPARE threshold.
    //
}

void
OverlayManagerImpl::sendPBFTPrePrepare(uint64_t pbftView, uint64_t seq,
                                       Hash const& digest,
                                       std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType = CUSTOM_PBFT_PRE_PREPARE;

    // Reuse fields:
    // view = PBFT sequence number
    // vp   = PBFT view number
    msg->customMessage().view = seq;
    msg->customMessage().vp = pbftView;
    msg->customMessage().blockHash = digest;
    msg->customMessage().data = data;

    broadcastMessage(msg);

    CLOG_DEBUG(Overlay, "[PBFT PRE-PREPARE] view={} seq={} digest={}", pbftView,
               seq, hexAbbrev(digest));
}

void
OverlayManagerImpl::sendPBFTPrepare(uint64_t pbftView, uint64_t seq,
                                    Hash const& digest)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType = CUSTOM_PBFT_PREPARE;
    msg->customMessage().view = seq;
    msg->customMessage().vp = pbftView;
    msg->customMessage().blockHash = digest;

    // Castro-Liskov, OSDI 1999, section 4.2: votes carry the digest,
    // not the request piggybacked on PRE-PREPARE.
    msg->customMessage().data.clear();

    broadcastMessage(msg);

    CLOG_DEBUG(Overlay, "[PBFT PREPARE] view={} seq={} digest={}", pbftView,
               seq, hexAbbrev(digest));
}

void
OverlayManagerImpl::sendPBFTCommit(uint64_t pbftView, uint64_t seq,
                                   Hash const& digest)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType = CUSTOM_PBFT_COMMIT;
    msg->customMessage().view = seq;
    msg->customMessage().vp = pbftView;
    msg->customMessage().blockHash = digest;

    // Castro-Liskov, OSDI 1999, section 4.2: votes carry the digest,
    // not the request piggybacked on PRE-PREPARE.
    msg->customMessage().data.clear();

    broadcastMessage(msg);

    CLOG_DEBUG(Overlay, "[PBFT COMMIT] view={} seq={} digest={}", pbftView, seq,
               hexAbbrev(digest));
}

void
OverlayManagerImpl::tryExecutePBFT()
{
    while (true)
    {
        uint64_t nextSeq = g_pbftLastExecuted + 1;
        PbftSlotKey slot{g_pbftView, nextSeq};

        auto it = g_pbft.find(slot);
        if (it == g_pbft.end())
        {
            return;
        }

        auto& st = it->second;

        if (!st.committedLocal || st.executed)
        {
            return;
        }

        TransactionBatch batch = TransactionBatch::deserialize(st.data);

        for (auto const& txn : batch.transactions)
        {
            std::istringstream ss(txn.payload);

            std::string op;
            std::string key;
            std::string value;

            ss >> op >> key;

            if (op == "READ")
            {
                auto kvIt = g_kvStore.find(key);
                (void)kvIt;
            }
            else if (op == "UPDATE" || op == "RMW")
            {
                ss >> value;
                g_kvStore[key] = value;
            }
        }

        st.executed = true;
        g_pbftLastExecuted = nextSeq;

        if (g_pbftLastExecuted > PBFT_HISTORY)
        {
            uint64_t cutoff = g_pbftLastExecuted - PBFT_HISTORY;

            for (auto it = g_pbft.begin(); it != g_pbft.end();)
            {
                if (it->first.seq < cutoff)
                    it = g_pbft.erase(it);
                else
                    ++it;
            }

            for (auto it = g_pbftAcceptedDigest.begin();
                 it != g_pbftAcceptedDigest.end();)
            {
                if (it->first.seq < cutoff)
                    it = g_pbftAcceptedDigest.erase(it);
                else
                    ++it;
            }
        }

        CLOG_DEBUG(Overlay, "[PBFT EXECUTED] view={} seq={} digest={} txns={}",
                   g_pbftView, nextSeq, hexAbbrev(st.digest),
                   batch.transactions.size());

        // Benchmark-compatible ACK to current shab_client.
        if (mApp.getConfig().SEND_CUSTOM_MESSAGE)
        {
            ackClientBatchesForBlock(nextSeq, st.digest);
        }

        if (mApp.getConfig().SEND_CUSTOM_MESSAGE)
        {
            prop();
        }
    }
}

void
OverlayManagerImpl::tryAdvancePBFT(uint64_t view, uint64_t seq,
                                   Hash const& digest)
{
    PbftSlotKey slot{view, seq};
    auto& st = g_pbft[slot];

    if (seq <= g_pbftLastExecuted)
    {
        CLOG_INFO(Overlay, "[PBFT DROP OLD] view={} seq={} lastExecuted={}",
                  view, seq, g_pbftLastExecuted);
        return;
    }

    if (!st.prePrepared || st.digest != digest)
    {
        return;
    }

    size_t prepareCount = pbftPrepareCount(st, digest);

    if (!st.prepared && prepareCount >= 2 * f)
    {
        st.prepared = true;

        CLOG_DEBUG(Overlay,
                   "[PBFT PREPARED] view={} seq={} digest={} prepares={} "
                   "threshold={} N={} f={}",
                   view, seq, hexAbbrev(digest), prepareCount, 2 * f, N, f);

        if (!st.commitSent)
        {
            st.commitSent = true;

            st.commitVotersByDigest[digest].insert(
                mApp.getConfig().NODE_SEED.getPublicKey());

            sendPBFTCommit(view, seq, digest);
        }
    }

    size_t commitCount = pbftCommitCount(st, digest);

    CLOG_DEBUG(Overlay,
               "[PBFT COMMIT CHECK] view={} seq={} digest={} commits={} "
               "threshold={} prepared={} N={} f={}",
               view, seq, hexAbbrev(digest), commitCount, 2 * f + 1,
               st.prepared, N, f);

    if (st.prepared && !st.committedLocal && commitCount >= 2 * f + 1)
    {
        st.committedLocal = true;

        CLOG_INFO(Overlay,
                  "[PBFT COMMITTED-LOCAL] view={} seq={} digest={} commits={}",
                  view, seq, hexAbbrev(digest), commitCount);

        tryExecutePBFT();
    }
}

void
OverlayManagerImpl::handlePBFTMessage(StellarMessage const& stellarMsg,
                                      Peer::pointer peer)
{
    auto const& cm = stellarMsg.customMessage();

    NodeID sender = peer->getPeerID();
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();

    uint64_t seq = cm.view;
    uint64_t view = cm.vp;
    Hash digest = cm.blockHash;

    if (view != g_pbftView)
    {
        CLOG_DEBUG(Overlay,
                   "[PBFT DROP] wrong view msgView={} localView={} seq={}",
                   view, g_pbftView, seq);
        return;
    }

    PbftSlotKey slot{view, seq};
    auto& st = g_pbft[slot];

    switch (cm.msgType)
    {
    case CUSTOM_PBFT_PRE_PREPARE:
    {
        auto existing = g_pbftAcceptedDigest.find(slot);
        if (existing != g_pbftAcceptedDigest.end() &&
            existing->second != digest)
        {
            CLOG_WARNING(
                Overlay,
                "[PBFT REJECT PRE-PREPARE] conflicting digest view={} seq={}",
                view, seq);
            return;
        }

        if (pbftDigest(cm.data) != digest)
        {
            CLOG_WARNING(
                Overlay,
                "[PBFT REJECT PRE-PREPARE] digest mismatch view={} seq={}",
                view, seq);
            return;
        }

        g_pbftAcceptedDigest[slot] = digest;

        st.prePrepared = true;
        st.digest = digest;
        st.data = cm.data;

        if (!st.prepareSent)
        {
            st.prepareSent = true;

            // Count own PREPARE.
            st.prepareVotersByDigest[digest].insert(selfID);

            sendPBFTPrepare(view, seq, digest);
        }

        CLOG_DEBUG(Overlay,
                   "[PBFT GOT PRE-PREPARE] view={} seq={} digest={} "
                   "prepares_buffered={} commits_buffered={}",
                   view, seq, hexAbbrev(digest), pbftPrepareCount(st, digest),
                   pbftCommitCount(st, digest));

        // Re-evaluate buffered votes after accepting the PRE-PREPARE.
        tryAdvancePBFT(view, seq, digest);

        return;
    }

    case CUSTOM_PBFT_PREPARE:
    {
        bool inserted = st.prepareVotersByDigest[digest].insert(sender).second;

        CLOG_DEBUG(Overlay,
                   "[PBFT PREPARE VOTE] view={} seq={} digest={} inserted={} "
                   "prepares={} threshold={} prePrepared={} N={} f={}",
                   view, seq, hexAbbrev(digest), inserted,
                   pbftPrepareCount(st, digest), 2 * f, st.prePrepared, N, f);

        // Do not drop early PREPAREs. Store them and advance if possible.
        tryAdvancePBFT(view, seq, digest);

        return;
    }

    case CUSTOM_PBFT_COMMIT:
    {
        bool inserted = st.commitVotersByDigest[digest].insert(sender).second;

        CLOG_DEBUG(Overlay,
                   "[PBFT COMMIT VOTE] view={} seq={} digest={} inserted={} "
                   "commits={} threshold={} prepared={} N={} f={}",
                   view, seq, hexAbbrev(digest), inserted,
                   pbftCommitCount(st, digest), 2 * f + 1, st.prepared, N, f);

        // Do not drop early COMMITs. Store them and advance if possible.
        tryAdvancePBFT(view, seq, digest);

        return;
    }

    default:
        return;
    }
}

} // namespace stellar
