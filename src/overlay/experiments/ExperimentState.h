#pragma once

#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "overlay/CustomProtocolTypes.h"
#include "overlay/Peer.h"
#include "overlay/experiments/ExperimentConfig.h"
#include <chrono>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stellar::experiment
{
// Shared benchmark bookkeeping. Definitions live in Experiment.cpp; protocol
// transitions live in their respective protocol translation units.
struct ViewBlockKey
{
    uint64_t view;
    Hash block;

    bool
    operator==(ViewBlockKey const& other) const noexcept
    {
        return view == other.view && block == other.block;
    }
};

struct OriginViewBlockKey
{
    NodeID origin;
    uint64_t view;
    Hash block;

    bool
    operator==(OriginViewBlockKey const& other) const noexcept
    {
        return origin == other.origin && view == other.view &&
               block == other.block;
    }
};

struct ViewBlockKeyHash
{
    size_t
    operator()(ViewBlockKey const& k) const noexcept
    {
        size_t h1 = std::hash<uint64_t>()(k.view);

        // simple hash combine for the 32-byte Hash
        size_t h2 = 0;
        for (auto b : k.block)
        {
            h2 = (h2 * 131) ^ b;
        }

        return h1 ^ (h2 << 1);
    }
};

struct PendingCondReadyVote
{
    NodeID sender;
    OriginViewBlockKey target;
};

struct BlockKey
{
    uint64_t view;
    Hash blockHash;

    bool
    operator==(BlockKey const& other) const noexcept
    {
        return view == other.view && blockHash == other.blockHash;
    }
};

struct BlockKeyHash
{
    size_t
    operator()(BlockKey const& k) const noexcept
    {
        size_t h1 = std::hash<uint64_t>()(k.view);
        size_t h2 = std::hash<std::string>()(binToHex(k.blockHash));
        return h1 ^ (h2 << 1);
    }
};

struct ClientAck
{
    uint64_t batchId;
    int clientFd;
};

struct NodeIDHash
{
    size_t
    operator()(NodeID const& n) const noexcept
    {
        return std::hash<std::string>()(KeyUtils::toStrKey(n));
    }
};
struct NodeIDEq
{
    bool
    operator()(NodeID const& a, NodeID const& b) const noexcept
    {
        return a == b;
    }
};

struct OriginViewBlockKeyHash
{
    size_t
    operator()(OriginViewBlockKey const& k) const noexcept
    {
        size_t h0 = NodeIDHash{}(k.origin);
        size_t h1 = std::hash<uint64_t>()(k.view);

        size_t h2 = 0;
        for (auto b : k.block)
        {
            h2 = (h2 * 131) ^ b;
        }

        return h0 ^ (h1 << 1) ^ (h2 << 2);
    }
};

struct TxnState
{
    bool preparedSent = false;
    uint64_t commitView = 0; // last view we sent a commit
    uint64_t preparedView = 0;
    uint64_t committedView = 0;
    uint64_t executedView = 0;

    bool proposalSentForView = false;

    bool ithsEchoSent = false;
    bool ithsAcceptSent = false;
    bool ithsLockSent = false;
    bool ithsCommitSent = false;

    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsEchoVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsAcceptVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsLockVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsCommitVoters;

    Hash preparedBlock;
    Hash committedBlock;

    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> prepareVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> commitVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> executeVoters;

    //  For collection / Bracha-like broadcast

    // Which origin/value I already echoed.
    std::unordered_set<OriginViewBlockKey, OriginViewBlockKeyHash> eSent;

    // Which origin/value I already readied or cond-readied.
    std::unordered_set<OriginViewBlockKey, OriginViewBlockKeyHash> rSent;

    // Echoes received for one origin's reported prepared value.
    std::unordered_map<OriginViewBlockKey,
                       std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       OriginViewBlockKeyHash>
        echoes;

    // Ready votes received for one origin's reported prepared value.
    std::unordered_map<OriginViewBlockKey,
                       std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       OriginViewBlockKeyHash>
        readies;

    // Delivered origin/value pairs, to avoid recording collection repeatedly.
    std::unordered_set<OriginViewBlockKey, OriginViewBlockKeyHash> delivered;

    // Collection: origin process p′ -> (vp,bp) it reported
    std::unordered_map<NodeID, std::pair<uint64_t, Hash>, NodeIDHash, NodeIDEq>
        collection;

    // Conditional-ready state

    // Deferred CondReady votes:
    // dependency (vp,bp) -> list of CONDREADY votes waiting for that
    // dependency.
    std::unordered_map<ViewBlockKey, std::vector<PendingCondReadyVote>,
                       ViewBlockKeyHash>
        pendingCondReady;
};

extern std::map<uint64_t, std::vector<std::pair<StellarMessage, Peer::pointer>>>
    g_futureFastMsgs;

extern size_t N;
extern size_t f;

extern uint64_t g_ithsLockView;
extern Hash g_ithsLockBlock;

extern std::unordered_map<uint64_t, Hash> g_ithsEchoSentForView;
extern std::unordered_map<uint64_t, Hash> g_ithsAcceptSentForView;
extern std::unordered_map<uint64_t, Hash> g_ithsLockSentForView;
extern std::unordered_map<uint64_t, Hash> g_ithsCommitSentForView;

extern std::chrono::steady_clock::time_point shabdizStartTime;
extern bool shabdizStartTimeSet;

extern bool collectWindowActive;
extern uint64_t collectWindowStartView;

extern uint64_t collectAttempts;

extern bool collectWindowArmed;
extern uint64_t lastCollectSentView;

extern bool force_collect;

extern std::unordered_map<std::string, std::string> g_kvStore;

extern bool g_clientListenerActive;

extern std::unordered_map<BlockKey, std::vector<ClientAck>, BlockKeyHash>
    g_blockClientAcks;

extern std::unordered_map<BlockKey, TxnState, BlockKeyHash> g_txn;
extern std::unordered_set<BlockKey, BlockKeyHash> g_ps;

extern uint64_t currentView;
extern uint64_t latestCommittedView;
extern Hash latestCommittedBlock;
extern int txn_count;
extern int shabdiz_start;

extern uint64_t
    g_csentView; // algorithm variable csent: last view where COMMIT was sent
extern std::unordered_set<uint64_t> g_fastProposedViews;

// Tracks both fast and slow proposals.
// This prevents FAST(v) and SLOW(v) from both consuming client batches.
extern std::unordered_set<uint64_t> g_proposedViews;

extern uint64_t g_localPreparedView;
extern Hash g_localPreparedBlock;

size_t getRSS_MB();
Hash makeBlock(Hash const& prev, int txnCount);
void ackClientBatchesForBlock(uint64_t view, Hash const& blockHash);
size_t getClientRequestQueueSize();
bool tryBuildServerBatch(TransactionBatch& batch, std::vector<ClientAck>& acks);
TransactionBatch makeSyntheticYCSBBatch(Application& app,
                                        std::string const& senderShortID,
                                        uint64_t startTxnId, size_t batchSize);
void cleanupOldTxnStates();
void updateForcedCollectState();
} // namespace stellar::experiment
