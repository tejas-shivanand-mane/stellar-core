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
#include <fstream>

namespace stellar
{
using namespace experiment;

namespace experiment
{
std::map<uint64_t, std::vector<std::pair<StellarMessage, Peer::pointer>>>
    g_futureFastMsgs;

size_t N = 0;
size_t f = 0;

uint64_t g_ithsLockView = 0;
Hash g_ithsLockBlock = Hash();

std::unordered_map<uint64_t, Hash> g_ithsEchoSentForView;
std::unordered_map<uint64_t, Hash> g_ithsAcceptSentForView;
std::unordered_map<uint64_t, Hash> g_ithsLockSentForView;
std::unordered_map<uint64_t, Hash> g_ithsCommitSentForView;

std::chrono::steady_clock::time_point shabdizStartTime;
bool shabdizStartTimeSet = false;

bool collectWindowActive = false;
uint64_t collectWindowStartView = 0;

uint64_t collectAttempts = 0;

bool collectWindowArmed = false;
uint64_t lastCollectSentView = UINT64_MAX;

bool force_collect = false;

std::unordered_map<std::string, std::string> g_kvStore;

bool g_clientListenerActive = false;

std::unordered_map<BlockKey, std::vector<ClientAck>, BlockKeyHash>
    g_blockClientAcks;

std::unordered_map<BlockKey, TxnState, BlockKeyHash> g_txn;
std::unordered_set<BlockKey, BlockKeyHash> g_ps;

uint64_t currentView = 1;
uint64_t latestCommittedView = 0;
Hash latestCommittedBlock = Hash();
int txn_count = 0;
int shabdiz_start = 0;

uint64_t g_csentView =
    0; // algorithm variable csent: last view where COMMIT was sent
std::unordered_set<uint64_t> g_fastProposedViews;

// Tracks both fast and slow proposals.
// This prevents FAST(v) and SLOW(v) from both consuming client batches.
std::unordered_set<uint64_t> g_proposedViews;

uint64_t g_localPreparedView = 0;
Hash g_localPreparedBlock = Hash();

size_t
getRSS_MB()
{
    std::ifstream status("/proc/self/status");
    std::string line;

    while (std::getline(status, line))
    {
        if (line.rfind("VmRSS:", 0) == 0)
        {
            // Format: VmRSS:   123456 kB
            size_t kb = std::stoul(line.substr(6));
            return kb / 1024;
        }
    }
    return 0;
}

Hash
makeBlock(Hash const& prev, int txnCount)
{
    std::string input = std::to_string(txnCount) + binToHex(prev);
    return sha256(input);
}

void
cleanupOldTxnStates()
{
    if (NonPrune_MODE)
    {
        return;
    }

    static const uint64_t MAX_HISTORY = MAX_VIEW_HISTORY;

    // Nothing is old enough to clean yet.
    if (latestCommittedView <= MAX_HISTORY)
    {
        return;
    }

    bool clean_done = false;

    const uint64_t cutoffView = latestCommittedView - MAX_HISTORY;

    //
    // Per-block protocol state.
    // Safe to remove only states far behind latestCommittedView.
    // This also frees TxnState internals: prepareVoters, commitVoters,
    // collection, echoes, readies, delivered, pendingCondReady, etc.
    //
    for (auto it = g_txn.begin(); it != g_txn.end();)
    {
        if (it->first.view < cutoffView)
        {
            it = g_txn.erase(it);

            if (clean_done == false)
            {
                clean_done = true;
            }
        }
        else
        {
            ++it;
        }
    }

    //
    // Prepared set.
    // Keep recent prepared certificates because they may still be used
    // for safety checks / dependencies.
    //
    for (auto it = g_ps.begin(); it != g_ps.end();)
    {
        if (it->view < cutoffView)
        {
            it = g_ps.erase(it);
        }
        else
        {
            ++it;
        }
    }
    //
    // Client ACK bookkeeping.
    // Committed blocks are already erased by ackClientBatchesForBlock().
    // This removes abandoned/stale ACK records for old views only.
    //
    for (auto it = g_blockClientAcks.begin(); it != g_blockClientAcks.end();)
    {
        if (it->first.view < cutoffView)
        {
            it = g_blockClientAcks.erase(it);
        }
        else
        {
            ++it;
        }
    }
    //
    // Future-message buffer.
    // Retain future messages and prune only entries older than cutoffView
    //
    //
    for (auto it = g_futureFastMsgs.begin(); it != g_futureFastMsgs.end();)
    {
        uint64_t bufferedView = it->first;

        if (bufferedView < cutoffView)
        {
            it = g_futureFastMsgs.erase(it);
        }
        else
        {
            ++it;
        }
    }

    //
    // Fast-path proposed-view history.
    // Old proposed-view markers are not needed once those views are far
    // behind latestCommittedView.
    //
    for (auto it = g_fastProposedViews.begin();
         it != g_fastProposedViews.end();)
    {
        if (*it < cutoffView)
        {
            it = g_fastProposedViews.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = g_proposedViews.begin(); it != g_proposedViews.end();)
    {
        if (*it < cutoffView)
        {
            it = g_proposedViews.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // //
    // // IT-HotStuff per-view sent-message maps.
    // // These are only per-view "already sent" markers, so old views are safe.
    // // Do NOT clean g_ithsLockView / g_ithsLockBlock here.
    // //
    // for (auto it = g_ithsEchoSentForView.begin(); it !=
    // g_ithsEchoSentForView.end(); )
    // {
    //     if (it->first < cutoffView)
    //     {
    //         it = g_ithsEchoSentForView.erase(it);
    //     }
    //     else
    //     {
    //         ++it;
    //     }
    // }

    // for (auto it = g_ithsAcceptSentForView.begin(); it !=
    // g_ithsAcceptSentForView.end(); )
    // {
    //     if (it->first < cutoffView)
    //     {
    //         it = g_ithsAcceptSentForView.erase(it);
    //     }
    //     else
    //     {
    //         ++it;
    //     }
    // }

    // for (auto it = g_ithsLockSentForView.begin(); it !=
    // g_ithsLockSentForView.end(); )
    // {
    //     if (it->first < cutoffView)
    //     {
    //         it = g_ithsLockSentForView.erase(it);
    //     }
    //     else
    //     {
    //         ++it;
    //     }
    // }

    // for (auto it = g_ithsCommitSentForView.begin(); it !=
    // g_ithsCommitSentForView.end(); )
    // {
    //     if (it->first < cutoffView)
    //     {
    //         it = g_ithsCommitSentForView.erase(it);
    //     }
    //     else
    //     {
    //         ++it;
    //     }
    // }

    // if (clean_done)
    // {
    //     g_txn.rehash(0);
    //     g_ps.rehash(0);
    //     g_blockClientAcks.rehash(0);
    //     g_fastProposedViews.rehash(0);

    // }

    CLOG_DEBUG(Overlay,
               "[CLEANUP] latestCommittedView={} cutoffView={} "
               "g_txn={} g_ps={} g_blockClientAcks={} g_futureFastMsgs={} "
               "g_fastProposedViews={} ithsEcho={} ithsAccept={} ithsLock={} "
               "ithsCommit={}",
               latestCommittedView, cutoffView, g_txn.size(), g_ps.size(),
               g_blockClientAcks.size(), g_futureFastMsgs.size(),
               g_fastProposedViews.size(), g_ithsEchoSentForView.size(),
               g_ithsAcceptSentForView.size(), g_ithsLockSentForView.size(),
               g_ithsCommitSentForView.size());
}

}

static std::shared_ptr<StellarMessage>
makeProposalMessage(bool ithsMode, uint64_t view, Hash const& blockHash,
                    uint64_t parentView, Hash const& parentBlock,
                    std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType =
        ithsMode ? CUSTOM_ITHS_PROPOSE : CUSTOM_PROPOSE;

    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;

    // Parent metadata for Shabdiz fast-path validation.
    msg->customMessage().vp = parentView;
    msg->customMessage().bp = parentBlock;

    msg->customMessage().data = data;

    return msg;
}

void
OverlayManagerImpl::tickExperiment()
{
    if (paperShabdizEnabled())
    {
        tickPaperShabdiz();
        return;
    }
    if (FORGET_IT_MODE)
        tickForgetIT();
    if (shabdiz_start == 0)
    {
        size_t authenticatedPeers = getAuthenticatedPeersCount();
        size_t totalNodes = mApp.getConfig().KNOWN_PEERS.size();
        size_t expectedPeers = totalNodes - 1;

        CLOG_INFO(Overlay, "authenticatedPeers, expectedPeers: {}, {}",
                  authenticatedPeers, expectedPeers);

        // Strict condition: start only after all nodes are connected.
        if (authenticatedPeers == expectedPeers)
        {
            if (mApp.getConfig().SEND_CUSTOM_MESSAGE && !ENABLE_SCP_TRACKING &&
                !g_clientListenerActive)
            {
                startClientListener(CLIENT_PORT);
                CLOG_INFO(Overlay, "[SHABDIZ READY] Client listener started "
                                   "after all peers connected");
            }

            shabdiz_start = 1;

            // shabdizStartTime = std::chrono::steady_clock::now();
            // shabdizStartTimeSet = true;

            CLOG_INFO(Overlay,
                      "Shabdiz started — all peers connected, timer armed");
        }
    }

    // Retry proposal only after all peers are connected and listener is active.
    // This prevents the startup race where prop() runs before the client queue
    // has enough requests.
    if (shabdiz_start == 1 &&
        ((mApp.getConfig().SEND_CUSTOM_MESSAGE && g_clientListenerActive) ||
         TETRABFT_MODE || SIMPLE_IT_MODE))
    {
        if (PBFT_MODE || normalPathEnabled())
        {
            prop();
        }
        else if (latestCommittedView == currentView - 1)
        {
            prop();
        }
    }
}

void
OverlayManagerImpl::prop()
{
    if (paperShabdizEnabled())
    {
        tickPaperShabdiz();
        return;
    }
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    std::string shortID = KeyUtils::toShortString(selfID);

    size_t clientQueueSize = getClientRequestQueueSize();

    CLOG_DEBUG(Overlay,
               "prop(): self={}, txn_count={}, latestCommittedView={}, "
               "currentView={}, clientRequestQueue={}",
               shortID, txn_count, latestCommittedView, currentView,
               clientQueueSize);

    if (TETRABFT_MODE || SIMPLE_IT_MODE)
    {
        normalProp();
        return;
    }

    // Non-leader/custom-disabled nodes do not drive Shabdiz / IT-HS proposals.
    if (!mApp.getConfig().SEND_CUSTOM_MESSAGE)
    {
        txn_count++;
        return;
    }

    if (normalPathEnabled())
    {
        normalProp();
        return;
    }

    if (PBFT_MODE)
    {
        pbftProp();
        return;
    }

    if (ITHS_MODE)
        ithsProp();
    else
        shabdizProp();
}

void
OverlayManagerImpl::recvCustomMessage(StellarMessage const& stellarMsg,
                                      Peer::pointer peer)
{

    auto type = stellarMsg.customMessage().msgType;
    if (paperShabdizEnabled())
    {
        if (type == CUSTOM_SHABDIZ_PAPER)
            receivePaperShabdiz(stellarMsg.customMessage(), peer->getPeerID());
        return;
    }
    if (type == CUSTOM_SHABDIZ_PAPER)
        return;
    if (type == CUSTOM_FORGET_NATIVE)
    {
        if (FORGET_IT_MODE)
            handleForgetITMessage(stellarMsg.customMessage(),
                                  peer->getPeerID());
        return;
    }
    if (type == CUSTOM_EXPERIMENT_WORK)
    {
        if (TETRABFT_MODE || SIMPLE_IT_MODE)
            normalReceiveWork(stellarMsg.customMessage(), peer->getPeerID());
        return;
    }
    bool tetraMessage =
        type >= CUSTOM_TETRA_PROPOSE && type <= CUSTOM_TETRA_VOTE4;
    bool simpleMessage =
        type >= CUSTOM_SIMPLE_PROPOSE && type <= CUSTOM_SIMPLE_COMMIT;
    bool forgetMessage =
        type >= CUSTOM_FORGET_PREPARE && type <= CUSTOM_FORGET_VOTE2;
    if (normalPathEnabled())
    {
        if ((TETRABFT_MODE && tetraMessage) ||
            (SIMPLE_IT_MODE && simpleMessage) ||
            (FORGET_IT_MODE && forgetMessage))
            handleNormalMessage(stellarMsg, peer->getPeerID());
        return;
    }
    if (tetraMessage || simpleMessage || forgetMessage)
        return;

    auto const& cm = stellarMsg.customMessage();

    if (N == 0)
    {
        N = getAuthenticatedPeersCount() + 1;
        f = (N - 1) / 3;
    }

    if (PBFT_MODE &&
        (cm.msgType == CUSTOM_PBFT_PRE_PREPARE ||
         cm.msgType == CUSTOM_PBFT_PREPARE || cm.msgType == CUSTOM_PBFT_COMMIT))
    {
        handlePBFTMessage(stellarMsg, peer);
        return;
    }

    bool ithsOldBackgroundMsg =
        ITHS_MODE &&
        (cm.msgType == CUSTOM_ITHS_ACCEPT || cm.msgType == CUSTOM_ITHS_COMMIT);

    if (cm.view < currentView && !ithsOldBackgroundMsg)
    {
        CLOG_DEBUG(Overlay,
                   "[FAST DROP OLD] type={} view={} currentView={} block={}",
                   static_cast<int>(cm.msgType), cm.view, currentView,
                   hexAbbrev(cm.blockHash));
        return;
    }

    if (cm.view > currentView)
    {
        CLOG_DEBUG(
            Overlay,
            "[FAST BUFFER FUTURE] type={} view={} currentView={} block={}",
            static_cast<int>(cm.msgType), cm.view, currentView,
            hexAbbrev(cm.blockHash));

        g_futureFastMsgs[cm.view].push_back({stellarMsg, peer});
        return;
    }

    if (cm.msgType >= CUSTOM_ITHS_PROPOSE && cm.msgType <= CUSTOM_ITHS_COMMIT)
        handleITHSMessage(stellarMsg, peer);
    else
        handleShabdizMessage(stellarMsg, peer);
}

void
OverlayManagerImpl::deliverBufferedForCurrentView()
{
    while (true)
    {
        auto it = g_futureFastMsgs.find(currentView);
        if (it == g_futureFastMsgs.end())
        {
            return;
        }

        auto buffered = std::move(it->second);
        g_futureFastMsgs.erase(it);

        CLOG_DEBUG(Overlay, "[FAST DELIVER BUFFERED] view={} count={}",
                   currentView, buffered.size());

        for (auto& [msg, p] : buffered)
        {
            this->recvCustomMessage(msg, p);
        }
    }
}

void
OverlayManagerImpl::legacyProp(bool ithsMode)
{
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    std::string shortID = KeyUtils::toShortString(selfID);
    updateForcedCollectState();

    bool collectAlreadyStartedForThisView =
        (lastCollectSentView == currentView);

    bool proposalAlreadySentForThisView =
        (g_proposedViews.count(currentView) != 0);

    bool canUseFastPath = (latestCommittedView == currentView - 1) &&
                          !force_collect && !collectAlreadyStartedForThisView &&
                          !proposalAlreadySentForThisView;

    if (canUseFastPath)
    {
        // Avoid duplicate proposals for the same view.
        if (g_proposedViews.count(currentView) ||
            g_fastProposedViews.count(currentView))
        {
            CLOG_DEBUG(Overlay,
                       "[FAST PROP SKIP] proposal already sent in view {}",
                       currentView);
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
                           "[CLIENT] Only {} queued requests; need {} to "
                           "propose view {}",
                           getClientRequestQueueSize(), SERVER_BATCH_SIZE,
                           currentView);
                return;
            }

            fromExternalClient = true;

            releaseAssert(batch.transactions.size() == SERVER_BATCH_SIZE);
            releaseAssert(clientAcks.size() == SERVER_BATCH_SIZE);

            CLOG_DEBUG(
                Overlay,
                "[CLIENT SERVER-BATCH] built block txns={} requests={} view={}",
                batch.transactions.size(), clientAcks.size(), currentView);
        }
        else
        {
            batch = makeSyntheticYCSBBatch(mApp, shortID, txn_count,
                                           SERVER_BATCH_SIZE);

            CLOG_DEBUG(
                Overlay,
                "[INTERNAL] Using synthetic block batch, txns={}, view={}",
                batch.transactions.size(), currentView);
        }

        if (batch.transactions.empty())
        {
            CLOG_WARNING(Overlay, "[PROP SKIP] empty batch for view {}",
                         currentView);
            return;
        }

        // Only mark the view as proposed after we actually have a non-empty
        // batch.
        g_fastProposedViews.insert(currentView);
        g_proposedViews.insert(currentView);

        txn_count += batch.transactions.size();

        Hash blockHash = makeBlock(latestCommittedBlock, txn_count);
        BlockKey key{currentView, blockHash};

        // Remember which client batch should be ACKed when this exact block
        // commits.
        if (fromExternalClient)
        {
            auto& acks = g_blockClientAcks[key];
            acks.insert(acks.end(), clientAcks.begin(), clientAcks.end());
        }

        std::string serializedBatch = batch.serialize();
        CLOG_DEBUG(Overlay,
                   "[BATCH SIZE] path=fast txns={} serialized_batch_bytes={} "
                   "bytes_per_txn={}",
                   batch.transactions.size(), serializedBatch.size(),
                   batch.transactions.empty()
                       ? 0
                       : serializedBatch.size() / batch.transactions.size());

        auto msg = makeProposalMessage(ithsMode, currentView, blockHash,
                                       latestCommittedView,
                                       latestCommittedBlock, batch.serialize());

        CLOG_INFO(
            Overlay,
            "[FAST SEND PROPOSE] block={} view={} parentView={} parentBlock={} "
            "latestCommittedView={} latestCommittedBlock={} txns={} source={}",
            hexAbbrev(blockHash), currentView, msg->customMessage().vp,
            hexAbbrev(msg->customMessage().bp), latestCommittedView,
            hexAbbrev(latestCommittedBlock), batch.transactions.size(),
            fromExternalClient ? "external-client" : "synthetic");

        if (shabdizStartTimeSet == false)
        {
            shabdizStartTime = std::chrono::steady_clock::now();
            shabdizStartTimeSet = true;
        }

        broadcastMessage(msg);

        //
        // Local leader handling: the leader also participates.
        //
        auto const& cm = msg->customMessage();

        if (ithsMode)
            acceptITHSLocalProposal(cm);
        else
            acceptShabdizLocalProposal(cm);

        return;
    }

    shabdizCollect();
}

} // namespace stellar
