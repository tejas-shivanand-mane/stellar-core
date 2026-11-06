// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{
using namespace experiment;

namespace experiment
{
void
updateForcedCollectState()
{
    if (shabdizStartTimeSet && !collectWindowArmed)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - shabdizStartTime);

        if (elapsed.count() >= FORCE_COLLECT_AFTER_SEC)
        {
            collectWindowArmed = true;
            collectWindowActive = true;
            collectAttempts = 0;

            collectWindowStartView = currentView;
            force_collect = true;

            CLOG_INFO(Overlay,
                      "⏱️ Forcing COLLECT for next {} views starting at view {}",
                      MAX_COLLECT_ATTEMPTS, collectWindowStartView);
        }
    }

    if (collectWindowActive)
    {
        force_collect = true;
    }
}
}

static std::pair<uint64_t, Hash>
maxPreparedFromCollection(
    std::unordered_map<NodeID, std::pair<uint64_t, Hash>, NodeIDHash,
                       NodeIDEq> const& collection)
{
    uint64_t bestView = 0;
    Hash bestBlock{};

    for (auto const& [node, vb] : collection)
    {
        auto const& [v, b] = vb;
        if (v > bestView)
        {
            bestView = v;
            bestBlock = b;
        }
    }
    return {bestView, bestBlock};
}

static void
rememberLocalPrepared(uint64_t view, Hash const& block)
{
    if (view >= g_localPreparedView)
    {
        g_localPreparedView = view;
        g_localPreparedBlock = block;
    }
}

void
OverlayManagerImpl::sendPrepare(uint64_t view, Hash const& blockHash,
                                std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType = CUSTOM_PREPARE;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;

    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "Broadcast PREPARE for block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendCommit(uint64_t view, Hash const& blockHash,
                               std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType = CUSTOM_COMMIT;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;

    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "Broadcast COMMIT for block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendExecute(uint64_t view, Hash const& blockHash,
                                std::string const& data)
{

    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType = CUSTOM_EXECUTE;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;

    //

    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "Broadcast EXECUTE for block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::shabdizProp()
{
    legacyProp(false);
}

void
OverlayManagerImpl::acceptShabdizLocalProposal(CustomMessage const& cm)
{
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    auto const& blockHash = cm.blockHash;
    BlockKey key{cm.view, cm.blockHash};
    auto& st = g_txn[key];

    bool selfExtendsCommitted = (latestCommittedView == currentView - 1) &&
                                (cm.vp == latestCommittedView) &&
                                (cm.bp == latestCommittedBlock);

    if (!st.preparedSent && selfExtendsCommitted)
    {
        st.preparedSent = true;
        st.preparedView = currentView;
        st.preparedBlock = blockHash;

        g_ps.insert(key);

        st.prepareVoters.insert(selfID);

        sendPrepare(cm.view, cm.blockHash, cm.data);

        CLOG_DEBUG(Overlay,
                   "[SELF-LOCAL SEND PREPARE] block={} view={} parentView={} "
                   "parentBlock={} prepareVotes={}",
                   hexAbbrev(blockHash), currentView, cm.vp, hexAbbrev(cm.bp),
                   st.prepareVoters.size());
    }
    else
    {
        CLOG_DEBUG(Overlay,
                   "[SELF-LOCAL NO PREPARE] block={} view={} preparedSent={} "
                   "latestCommittedView={} expectedPrev={} parentView={} "
                   "parentBlock={} localCommittedBlock={}",
                   hexAbbrev(blockHash), currentView, st.preparedSent,
                   latestCommittedView, currentView - 1, cm.vp,
                   hexAbbrev(cm.bp), hexAbbrev(latestCommittedBlock));
    }
}

void
OverlayManagerImpl::shabdizCollect()
{
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    std::string shortID = KeyUtils::toShortString(selfID);
    //
    // Slow path / forced COLLECT path
    //
    if (g_proposedViews.count(currentView))
    {
        CLOG_DEBUG(Overlay,
                   "[SLOW COLLECT SKIP] proposal already sent for view {}",
                   currentView);
        return;
    }

    if (lastCollectSentView == currentView)
    {
        CLOG_DEBUG(Overlay,
                   "[SLOW COLLECT SKIP] already sent COLLECT for view {}",
                   currentView);
        return;
    }

    lastCollectSentView = currentView;

    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType = CUSTOM_COLLECT;
    msg->customMessage().view = currentView;

    CLOG_INFO(Overlay, "Leader initiating COLLECT for view {}", currentView);

    broadcastMessage(msg);

    uint64_t vp = 0;
    Hash bp = Hash();

    if (g_localPreparedView > 0)
    {
        vp = g_localPreparedView;
        bp = g_localPreparedBlock;
    }
    else if (latestCommittedView > 0)
    {
        vp = latestCommittedView;
        bp = latestCommittedBlock;
    }
    else
    {
        vp = 0;
        bp = Hash();
    }

    auto sendMsg = std::make_shared<StellarMessage>();
    sendMsg->type(CUSTOM_MESSAGE);
    sendMsg->customMessage().msgType = CUSTOM_SEND;
    sendMsg->customMessage().view = currentView;
    sendMsg->customMessage().vp = vp;
    sendMsg->customMessage().bp = bp;
    sendMsg->customMessage().origin = selfID;

    broadcastMessage(sendMsg);

    CLOG_DEBUG(Overlay, "[SLOW SELF SEND] origin={} view={} vp={} bp={}",
               shortID, currentView, vp, hexAbbrev(bp));

    if (collectWindowActive)
    {
        collectAttempts++;

        CLOG_DEBUG(Overlay, "Forced COLLECT attempt {}/{}", collectAttempts,
                   MAX_COLLECT_ATTEMPTS);

        if (collectAttempts >= MAX_COLLECT_ATTEMPTS)
        {
            collectWindowActive = false;
            force_collect = false;

            CLOG_INFO(Overlay, "Forced COLLECT window ended after {} attempts",
                      MAX_COLLECT_ATTEMPTS);
        }
    }
}
void
OverlayManagerImpl::handleShabdizMessage(StellarMessage const& stellarMsg,
                                         Peer::pointer peer)
{
    auto const& cm = stellarMsg.customMessage();
    NodeID sender = peer->getPeerID();
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    BlockKey key{cm.view, cm.blockHash};
    auto& st = g_txn[key];
    auto tryLeaderSendSlowProposal = [&](decltype(st)& state,
                                         uint64_t collectView) {
        if (!mApp.getConfig().SEND_CUSTOM_MESSAGE)
        {
            return;
        }

        // This collection is stale. Do not consume another client batch.
        if (collectView != currentView)
        {
            CLOG_DEBUG(Overlay,
                       "[SLOW PROP SKIP] stale collectView={} currentView={}",
                       collectView, currentView);
            return;
        }

        // The view already committed. Do not build an orphan slow block.
        if (latestCommittedView >= collectView)
        {
            CLOG_DEBUG(Overlay,
                       "[SLOW PROP SKIP] view already committed collectView={} "
                       "latestCommittedView={}",
                       collectView, latestCommittedView);
            return;
        }

        // Slow proposal should only happen for a view where the leader actually
        // initiated COLLECT.
        if (lastCollectSentView != collectView)
        {
            CLOG_DEBUG(Overlay,
                       "[SLOW PROP SKIP] no active COLLECT for view {} "
                       "lastCollectSentView={}",
                       collectView, lastCollectSentView);
            return;
        }

        // Check for a duplicate proposal before consuming a client batch.
        //
        //
        if (state.proposalSentForView || g_proposedViews.count(collectView))
        {
            CLOG_WARNING(Overlay,
                         "[SLOW PROP SKIP] proposal already sent for view {} "
                         "stateSent={} globalSent={}",
                         collectView, state.proposalSentForView,
                         g_proposedViews.count(collectView));
            return;
        }

        if (state.collection.size() < 2 * f + 1)
        {
            return;
        }

        auto [maxView, maxBlock] = maxPreparedFromCollection(state.collection);

        TransactionBatch batch;
        bool fromExternalClient = false;
        std::vector<ClientAck> clientAcks;

        if (g_clientListenerActive)
        {
            if (!tryBuildServerBatch(batch, clientAcks))
            {
                CLOG_DEBUG(Overlay,
                           "[SLOW CLIENT] Only {} queued requests; need {} to "
                           "propose slow-path view {}",
                           getClientRequestQueueSize(), SERVER_BATCH_SIZE,
                           collectView);
                return;
            }

            fromExternalClient = true;

            releaseAssert(batch.transactions.size() == SERVER_BATCH_SIZE);
            releaseAssert(clientAcks.size() == SERVER_BATCH_SIZE);
        }
        else
        {
            batch =
                makeSyntheticYCSBBatch(mApp, KeyUtils::toShortString(selfID),
                                       txn_count, SERVER_BATCH_SIZE);
        }

        if (batch.transactions.empty())
        {
            CLOG_WARNING(Overlay, "[SLOW PROP SKIP] empty batch for view {}",
                         collectView);
            return;
        }

        txn_count += batch.transactions.size();

        Hash newBlock = makeBlock(maxBlock, txn_count);
        BlockKey newKey{collectView, newBlock};

        if (fromExternalClient)
        {
            auto& acks = g_blockClientAcks[newKey];
            acks.insert(acks.end(), clientAcks.begin(), clientAcks.end());
        }

        auto msg = std::make_shared<StellarMessage>();
        msg->type(CUSTOM_MESSAGE);
        msg->customMessage().msgType = CUSTOM_PROPOSE;
        msg->customMessage().view = collectView;
        msg->customMessage().blockHash = newBlock;

        // Slow-path proposal extends the highest prepared block
        // selected from the collection.
        msg->customMessage().vp = maxView;
        msg->customMessage().bp = maxBlock;
        msg->customMessage().data = batch.serialize();

        // Mark before broadcasting, so recursive/local processing cannot create
        // another proposal for the same view.
        state.proposalSentForView = true;
        g_proposedViews.insert(collectView);

        broadcastMessage(msg);

        CLOG_INFO(Overlay,
                  "[SLOW SEND PROPOSE] block={} view={} parentView={} "
                  "parentBlock={} txns={} source={}",
                  hexAbbrev(newBlock), collectView, maxView,
                  hexAbbrev(maxBlock), batch.transactions.size(),
                  fromExternalClient ? "external-client" : "synthetic");
    };

    auto deliverSlowValue = [&](decltype(st)& state,
                                OriginViewBlockKey const& target) {
        if (!state.delivered.insert(target).second)
        {
            return;
        }

        state.collection[target.origin] = {target.view, target.block};

        CLOG_DEBUG(
            Overlay,
            "[SLOW DELIVER] origin={} target=(vp={}, bp={}) collectionSize={}",
            KeyUtils::toShortString(target.origin), target.view,
            hexAbbrev(target.block), state.collection.size());

        tryLeaderSendSlowProposal(state, cm.view);
    };

    auto activatePendingCondReadyForDependency = [&](decltype(st)& state,
                                                     ViewBlockKey const&
                                                         dependency) {
        auto it = state.pendingCondReady.find(dependency);
        if (it == state.pendingCondReady.end())
        {
            return;
        }

        auto pendingVotes = std::move(it->second);
        state.pendingCondReady.erase(it);

        CLOG_INFO(
            Overlay,
            "[CONDREADY ACTIVATE] dependency=(vp={}, bp={}) pendingVotes={}",
            dependency.view, hexAbbrev(dependency.block), pendingVotes.size());

        for (auto const& vote : pendingVotes)
        {
            state.readies[vote.target].insert(vote.sender);

            CLOG_INFO(Overlay,
                      "[CONDREADY COUNTED] sender={} origin={} target=(vp={}, "
                      "bp={}) readies={}",
                      KeyUtils::toShortString(vote.sender),
                      KeyUtils::toShortString(vote.target.origin),
                      vote.target.view, hexAbbrev(vote.target.block),
                      state.readies[vote.target].size());

            if (state.readies[vote.target].size() >= 2 * f + 1)
            {
                deliverSlowValue(state, vote.target);
            }
        }
    };

    switch (cm.msgType)
    {
    // ================================================================
    case CUSTOM_PROPOSE:
    {

        CLOG_INFO(Overlay,
                  "OverlayManagerImpl tick; MEMORY RSS: {} MB, number of "
                  "elements: {}",
                  getRSS_MB(), g_txn.size());

        CLOG_DEBUG(
            Overlay,
            "[RECV PROPOSE] self={} sender={} view={} block={} currentView={} "
            "latestCommittedView={} parentView={} parentBlock={}",
            KeyUtils::toShortString(selfID), KeyUtils::toShortString(sender),
            cm.view, hexAbbrev(cm.blockHash), currentView, latestCommittedView,
            cm.vp, hexAbbrev(cm.bp));

        bool fastExtendsCommitted = (latestCommittedView == cm.view - 1) &&
                                    (cm.vp == latestCommittedView) &&
                                    (cm.bp == latestCommittedBlock);

        auto& collectSt = g_txn[BlockKey{cm.view, Hash()}];

        bool slowProposalOk = false;
        if (collectSt.collection.size() >= 2 * f + 1)
        {
            auto [maxView, maxBlock] =
                maxPreparedFromCollection(collectSt.collection);

            slowProposalOk = (cm.vp == maxView) && (cm.bp == maxBlock);

            CLOG_INFO(Overlay,
                      "[SLOW PROPOSE CHECK] collectionSize={} "
                      "selectedParentView={} selectedParentBlock={} "
                      "proposedParentView={} proposedParentBlock={} ok={}",
                      collectSt.collection.size(), maxView, hexAbbrev(maxBlock),
                      cm.vp, hexAbbrev(cm.bp), slowProposalOk);
        }
        else
        {
            CLOG_INFO(Overlay,
                      "[SLOW PROPOSE CHECK] insufficient collection size={} "
                      "threshold={}",
                      collectSt.collection.size(), 2 * f + 1);
        }

        bool extendsValidParent = fastExtendsCommitted || slowProposalOk;

        if (!st.preparedSent && extendsValidParent)
        {
            st.preparedSent = true;
            st.preparedView = cm.view;
            st.preparedBlock = cm.blockHash;

            g_ps.insert(BlockKey{cm.view, cm.blockHash});

            st.prepareVoters.insert(selfID);
            sendPrepare(cm.view, cm.blockHash, cm.data);

            CLOG_DEBUG(
                Overlay,
                "[SEND PREPARE] view={} block={} prepareVotes={} reason={}",
                cm.view, hexAbbrev(cm.blockHash), st.prepareVoters.size(),
                fastExtendsCommitted ? "fast-parent" : "slow-collection");
        }
        else
        {
            CLOG_DEBUG(
                Overlay,
                "[NO PREPARE] view={} block={} preparedSent={} "
                "fastExtendsCommitted={} slowProposalOk={} collectionSize={} "
                "latestCommittedView={} proposedParentView={} "
                "proposedParentBlock={} localCommittedBlock={}",
                cm.view, hexAbbrev(cm.blockHash), st.preparedSent,
                fastExtendsCommitted, slowProposalOk,
                collectSt.collection.size(), latestCommittedView, cm.vp,
                hexAbbrev(cm.bp), hexAbbrev(latestCommittedBlock));
        }

        break;
    }

    // ================================================================
    case CUSTOM_PREPARE:
    {
        bool inserted = st.prepareVoters.insert(sender).second;

        CLOG_DEBUG(Overlay,
                   "[FAST RECV PREPARE] self={} sender={} inserted={} view={} "
                   "block={} prepareVotes={} threshold={}",
                   KeyUtils::toShortString(selfID),
                   KeyUtils::toShortString(sender), inserted, cm.view,
                   hexAbbrev(cm.blockHash), st.prepareVoters.size(), 2 * f + 1);

        if (st.prepareVoters.size() >= 2 * f + 1 && g_csentView < currentView)
        {
            g_csentView = currentView;

            st.commitVoters.insert(selfID);
            sendCommit(cm.view, cm.blockHash, cm.data);

            st.preparedView = cm.view;
            st.preparedBlock = cm.blockHash;

            rememberLocalPrepared(cm.view, cm.blockHash);

            ViewBlockKey dependency{cm.view, cm.blockHash};
            activatePendingCondReadyForDependency(st, dependency);

            if (!NonPrune_MODE)
            {
                for (auto it = g_ps.begin(); it != g_ps.end();)
                {
                    if (it->view != st.preparedView)
                        it = g_ps.erase(it);
                    else
                        ++it;
                }
            }

            CLOG_DEBUG(Overlay,
                       "[FAST SEND COMMIT] view={} block={} prepareVotes={} "
                       "commitVotes={}",
                       cm.view, hexAbbrev(cm.blockHash),
                       st.prepareVoters.size(), st.commitVoters.size());
        }

        break;
    }

    // ================================================================
    case CUSTOM_COMMIT:
    {

        bool inserted = st.commitVoters.insert(sender).second;

        CLOG_DEBUG(Overlay,
                   "[FAST RECV COMMIT] self={} sender={} inserted={} view={} "
                   "block={} commitVotes={} fPlusOne={} quorum={}",
                   KeyUtils::toShortString(selfID),
                   KeyUtils::toShortString(sender), inserted, cm.view,
                   hexAbbrev(cm.blockHash), st.commitVoters.size(), f + 1,
                   2 * f + 1);

        // Amplification: f+1 COMMITs and I have not sent COMMIT in this view.
        if (st.commitVoters.size() >= f + 1 && g_csentView < currentView)
        {
            g_csentView = currentView;

            st.commitVoters.insert(selfID);
            sendCommit(cm.view, cm.blockHash, cm.data);

            st.preparedView = cm.view;
            st.preparedBlock = cm.blockHash;
            rememberLocalPrepared(cm.view, cm.blockHash);

            ViewBlockKey dependency{cm.view, cm.blockHash};
            activatePendingCondReadyForDependency(st, dependency);

            if (!NonPrune_MODE)
            {
                for (auto it = g_ps.begin(); it != g_ps.end();)
                {
                    if (it->view != st.preparedView)
                        it = g_ps.erase(it);
                    else
                        ++it;
                }
            }

            CLOG_DEBUG(Overlay,
                       "[FAST AMPLIFY COMMIT] view={} block={} commitVotes={}",
                       cm.view, hexAbbrev(cm.blockHash),
                       st.commitVoters.size());
        }

        // Final commit: 2f+1 COMMITs and not already committed this view.
        if (st.commitVoters.size() >= 2 * f + 1 &&
            latestCommittedView < currentView)
        {
            st.committedView = cm.view;
            st.committedBlock = cm.blockHash;

            latestCommittedView = cm.view;
            latestCommittedBlock = cm.blockHash;

            st.preparedView = cm.view;
            st.preparedBlock = cm.blockHash;
            rememberLocalPrepared(cm.view, cm.blockHash);

            ViewBlockKey dependency{cm.view, cm.blockHash};
            activatePendingCondReadyForDependency(st, dependency);

            if (!NonPrune_MODE)
            {
                for (auto it = g_ps.begin(); it != g_ps.end();)
                {
                    if (it->view != st.preparedView)
                        it = g_ps.erase(it);
                    else
                        ++it;
                }
            }

            TransactionBatch batch = TransactionBatch::deserialize(cm.data);
            for (auto const& txn : batch.transactions)
            {
                std::istringstream ss(txn.payload);
                std::string op, key, value;
                ss >> op >> key;
                if (op == "READ")
                {
                    auto it = g_kvStore.find(key);
                    (void)it;
                }
                else if (op == "UPDATE" || op == "RMW")
                {
                    ss >> value;
                    g_kvStore[key] = value;
                }
            }

            CLOG_INFO(Overlay,
                      "[FAST COMMITTED] block={} view={} txns={} nextView={}",
                      hexAbbrev(cm.blockHash), cm.view,
                      batch.transactions.size(), cm.view + 1);

            ackClientBatchesForBlock(cm.view, cm.blockHash);

            currentView = cm.view + 1;
            lastCollectSentView = UINT64_MAX;

            BlockKey nextKey{currentView, Hash()};
            g_txn[nextKey].proposalSentForView = false;

            // if (!(mApp.getConfig().MEMORY_PROF && cm.view > 20000 &&
            // cm.view%5000 < 1500))
            {
                cleanupOldTxnStates();
            }

            // Process messages buffered for the new view before proposing.
            deliverBufferedForCurrentView();

            // prop() is a no-op on non-leader nodes.
            if (latestCommittedView == currentView - 1)
            {
                prop();
            }
        }

        break;
    }

    // ================================================================
    case CUSTOM_COLLECT:
        CLOG_INFO(Overlay, "Received COLLECT for view {} from {}", cm.view,
                  KeyUtils::toShortString(sender));

        {
            uint64_t vp = 0;
            Hash bp = Hash();

            if (g_localPreparedView > 0)
            {
                vp = g_localPreparedView;
                bp = g_localPreparedBlock;

                CLOG_INFO(Overlay,
                          "[COLLECT REPLY] using local prepared vp={} bp={}",
                          vp, hexAbbrev(bp));
            }
            else if (latestCommittedView > 0)
            {
                vp = latestCommittedView;
                bp = latestCommittedBlock;

                CLOG_INFO(Overlay,
                          "[COLLECT REPLY] using latest committed vp={} bp={}",
                          vp, hexAbbrev(bp));
            }
            else
            {
                vp = 0;
                bp = Hash();

                CLOG_INFO(Overlay, "[COLLECT REPLY] using genesis vp=0 bp={}",
                          hexAbbrev(bp));
            }

            //  Broadcast this node's SEND.
            auto sendMsg = std::make_shared<StellarMessage>();
            sendMsg->type(CUSTOM_MESSAGE);
            sendMsg->customMessage().msgType = CUSTOM_SEND;
            sendMsg->customMessage().view = cm.view;
            sendMsg->customMessage().vp = vp;
            sendMsg->customMessage().bp = bp;
            sendMsg->customMessage().origin = selfID;
            broadcastMessage(sendMsg);
        }
        break;

    // ================================================================
    case CUSTOM_SEND:
    {
        CLOG_DEBUG(
            Overlay,
            "Received SEND target=(vp={}, bp={}) view={} from={} origin={}",
            cm.vp, hexAbbrev(cm.bp), cm.view, KeyUtils::toShortString(sender),
            KeyUtils::toShortString(cm.origin));

        OriginViewBlockKey ovb{cm.origin, cm.vp, cm.bp};

        if (!st.eSent.count(ovb))
        {
            st.eSent.insert(ovb);

            auto msg = std::make_shared<StellarMessage>();
            msg->type(CUSTOM_MESSAGE);
            msg->customMessage().msgType = CUSTOM_ECHO;
            msg->customMessage().view = cm.view;
            msg->customMessage().vp = cm.vp;
            msg->customMessage().bp = cm.bp;
            msg->customMessage().origin = cm.origin;

            broadcastMessage(msg);

            st.echoes[ovb].insert(selfID);

            CLOG_DEBUG(Overlay, "Sending ECHO origin={} target=(vp={}, bp={})",
                       KeyUtils::toShortString(cm.origin), cm.vp,
                       hexAbbrev(cm.bp));
        }

        break;
    }

    // ================================================================
    case CUSTOM_ECHO:
    {
        CLOG_DEBUG(
            Overlay,
            "Received ECHO origin={} target=(vp={}, bp={}) view={} from={}",
            KeyUtils::toShortString(cm.origin), cm.vp, hexAbbrev(cm.bp),
            cm.view, KeyUtils::toShortString(sender));

        OriginViewBlockKey ovb{cm.origin, cm.vp, cm.bp};
        st.echoes[ovb].insert(sender);

        if (st.echoes[ovb].size() >= 2 * f + 1 && !st.rSent.count(ovb))
        {
            if (g_ps.count(BlockKey{cm.vp, cm.bp}))
            {
                st.rSent.insert(ovb);

                auto msg = std::make_shared<StellarMessage>();
                msg->type(CUSTOM_MESSAGE);
                msg->customMessage().msgType = CUSTOM_READY;
                msg->customMessage().view = cm.view;
                msg->customMessage().vp = cm.vp;
                msg->customMessage().bp = cm.bp;
                msg->customMessage().origin = cm.origin;

                broadcastMessage(msg);

                st.readies[ovb].insert(selfID);

                CLOG_DEBUG(Overlay,
                           "Sending READY origin={} target=(vp={}, bp={})",
                           KeyUtils::toShortString(cm.origin), cm.vp,
                           hexAbbrev(cm.bp));

                if (st.readies[ovb].size() >= 2 * f + 1)
                {
                    deliverSlowValue(st, ovb);
                }
            }
            else if (cm.vp < g_localPreparedView)
            {
                st.rSent.insert(ovb);

                auto msg = std::make_shared<StellarMessage>();
                msg->type(CUSTOM_MESSAGE);
                msg->customMessage().msgType = CUSTOM_CONDREADY;
                msg->customMessage().view = cm.view;

                // Preserve the original SEND target and attach the higher
                // prepared dependency
                msg->customMessage().vp = cm.vp;
                msg->customMessage().bp = cm.bp;

                //
                msg->customMessage().origin = cm.origin;

                //
                msg->customMessage().dependencyVp = g_localPreparedView;
                msg->customMessage().dependencyBp = g_localPreparedBlock;

                broadcastMessage(msg);

                CLOG_DEBUG(Overlay,
                           "Sending CONDREADY origin={} target=(vp={}, bp={}) "
                           "dependency=(vp={}, bp={})",
                           KeyUtils::toShortString(cm.origin), cm.vp,
                           hexAbbrev(cm.bp), g_localPreparedView,
                           hexAbbrev(g_localPreparedBlock));
            }
        }

        break;
    }

    // ================================================================
    case CUSTOM_READY:
    {
        CLOG_DEBUG(
            Overlay,
            "Received READY origin={} target=(vp={}, bp={}) view={} from={}",
            KeyUtils::toShortString(cm.origin), cm.vp, hexAbbrev(cm.bp),
            cm.view, KeyUtils::toShortString(sender));

        OriginViewBlockKey ovb{cm.origin, cm.vp, cm.bp};

        st.readies[ovb].insert(sender);

        // READY amplification: f+1 READYs imply send READY if not already sent.
        if (st.readies[ovb].size() >= f + 1 && !st.rSent.count(ovb))
        {
            st.rSent.insert(ovb);

            auto msg = std::make_shared<StellarMessage>();
            msg->type(CUSTOM_MESSAGE);
            msg->customMessage().msgType = CUSTOM_READY;
            msg->customMessage().view = cm.view;
            msg->customMessage().vp = cm.vp;
            msg->customMessage().bp = cm.bp;
            msg->customMessage().origin = cm.origin;

            broadcastMessage(msg);

            st.readies[ovb].insert(selfID);

            CLOG_DEBUG(
                Overlay,
                "[READY AMPLIFY] origin={} target=(vp={}, bp={}) readies={}",
                KeyUtils::toShortString(cm.origin), cm.vp, hexAbbrev(cm.bp),
                st.readies[ovb].size());
        }

        // Delivery: 2f+1 READYs deliver this origin's reported prepared value.
        if (st.readies[ovb].size() >= 2 * f + 1)
        {
            deliverSlowValue(st, ovb);
        }

        break;
    }

    // ================================================================
    case CUSTOM_CONDREADY:
    {
        CLOG_DEBUG(Overlay,
                   "Received CONDREADY origin={} target=(vp={}, bp={}) "
                   "dependency=(vp={}, bp={}) view={} from={}",
                   KeyUtils::toShortString(cm.origin), cm.vp, hexAbbrev(cm.bp),
                   cm.dependencyVp, hexAbbrev(cm.dependencyBp), cm.view,
                   KeyUtils::toShortString(sender));

        OriginViewBlockKey target{cm.origin, cm.vp, cm.bp};
        ViewBlockKey dependency{cm.dependencyVp, cm.dependencyBp};

        bool dependencyKnown =
            g_ps.count(BlockKey{cm.dependencyVp, cm.dependencyBp}) ||
            (g_localPreparedView >= cm.dependencyVp &&
             g_localPreparedBlock == cm.dependencyBp) ||
            (latestCommittedView >= cm.dependencyVp &&
             latestCommittedBlock == cm.dependencyBp);

        if (dependencyKnown)
        {
            st.readies[target].insert(sender);

            CLOG_INFO(Overlay,
                      "CONDREADY counted origin={} target=(vp={}, bp={}) "
                      "dependency=(vp={}, bp={}) readies={}",
                      KeyUtils::toShortString(cm.origin), cm.vp,
                      hexAbbrev(cm.bp), cm.dependencyVp,
                      hexAbbrev(cm.dependencyBp), st.readies[target].size());

            if (st.readies[target].size() >= 2 * f + 1)
            {
                deliverSlowValue(st, target);
            }
        }
        else
        {
            PendingCondReadyVote pending;
            pending.sender = sender;
            pending.target = target;

            st.pendingCondReady[dependency].push_back(std::move(pending));

            CLOG_DEBUG(Overlay,
                       "CONDREADY deferred origin={} target=(vp={}, bp={}) "
                       "waiting for dependency=(vp={}, bp={})",
                       KeyUtils::toShortString(cm.origin), cm.vp,
                       hexAbbrev(cm.bp), cm.dependencyVp,
                       hexAbbrev(cm.dependencyBp));
        }

        break;
    }
    }
}

} // namespace stellar
