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

// IT-HS blog version, not the OPODIS paper:
// https://decentralizedthoughts.github.io/2021-09-20-information-theoretic-hotstuff-it-hs-part-one/
// ECHO, ACCEPT, LOCK and COMMIT carry b. Here b includes the batch data;
// preserve these value-carrying messages rather than applying PBFT's digests.

void
OverlayManagerImpl::sendITHSEcho(uint64_t view, Hash const& blockHash,
                                 std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType = CUSTOM_ITHS_ECHO;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;
    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "[IT-HS] Broadcast ECHO block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendITHSAccept(uint64_t view, Hash const& blockHash,
                                   std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType = CUSTOM_ITHS_ACCEPT;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;
    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "[IT-HS] Broadcast ACCEPT block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendITHSLock(uint64_t view, Hash const& blockHash,
                                 std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType = CUSTOM_ITHS_LOCK;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;
    broadcastMessage(msg);
}

void
OverlayManagerImpl::sendITHSCommit(uint64_t view, Hash const& blockHash,
                                   std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType = CUSTOM_ITHS_COMMIT;
    msg->customMessage().view = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data = data;

    broadcastMessage(msg);

    CLOG_DEBUG(Overlay, "[IT-HS] Broadcast COMMIT block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::ithsProp()
{
    legacyProp(true);
}

void
OverlayManagerImpl::acceptITHSLocalProposal(CustomMessage const& cm)
{
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    auto const& blockHash = cm.blockHash;
    auto& st = g_txn[BlockKey{cm.view, cm.blockHash}];

    bool firstEchoThisView = !g_ithsEchoSentForView.count(cm.view);
    // No view-change implementation: treat parent/lock metadata as the safety
    // check.
    bool safeForLocalLock = (cm.vp >= g_ithsLockView);

    if (firstEchoThisView && safeForLocalLock)
    {
        g_ithsEchoSentForView[cm.view] = cm.blockHash;

        st.ithsEchoSent = true;
        st.ithsEchoVoters.insert(selfID);

        sendITHSEcho(cm.view, cm.blockHash, cm.data);

        CLOG_DEBUG(Overlay,
                   "[IT-HS SELF-LOCAL SEND ECHO] block={} view={} "
                   "proposalLockView={} localLockView={}",
                   hexAbbrev(blockHash), currentView, cm.vp, g_ithsLockView);
    }
    else
    {
        CLOG_DEBUG(Overlay,
                   "[IT-HS SELF-LOCAL NO ECHO] block={} view={} "
                   "ithsEchoSent={} latestCommittedView={}",
                   hexAbbrev(blockHash), currentView, st.ithsEchoSent,
                   latestCommittedView);
    }
}
void
OverlayManagerImpl::handleITHSMessage(StellarMessage const& stellarMsg,
                                      Peer::pointer peer)
{
    auto const& cm = stellarMsg.customMessage();
    NodeID sender = peer->getPeerID();
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    BlockKey key{cm.view, cm.blockHash};
    auto& st = g_txn[key];
    switch (cm.msgType)
    {
    case CUSTOM_ITHS_PROPOSE:
        if (cm.view == currentView)
        {
            CLOG_DEBUG(Overlay, "[IT-HS] Received PROPOSE block {} view {}",
                       hexAbbrev(cm.blockHash), cm.view);

            bool firstEchoThisView = !g_ithsEchoSentForView.count(cm.view);

            // No view-change implementation: proposal's lock/parent view must
            // be at least the local IT-HS lock view.
            bool safeForLocalLock = (cm.vp >= g_ithsLockView);

            if (firstEchoThisView && safeForLocalLock)
            {
                g_ithsEchoSentForView[cm.view] = cm.blockHash;

                st.ithsEchoSent = true;
                st.ithsEchoVoters.insert(selfID);
                sendITHSEcho(cm.view, cm.blockHash, cm.data);
            }
            else
            {
                CLOG_DEBUG(
                    Overlay,
                    "[IT-HS] Reject/skip PROPOSE block {} view {} "
                    "firstEchoThisView={} proposalLockView={} localLockView={}",
                    hexAbbrev(cm.blockHash), cm.view, firstEchoThisView, cm.vp,
                    g_ithsLockView);
            }
        }
        break;

    // ================================================================
    case CUSTOM_ITHS_ECHO:
        if (cm.view == currentView)
        {
            st.ithsEchoVoters.insert(sender);

            CLOG_DEBUG(
                Overlay, "[IT-HS] Received ECHO block {} view {} echoes={}",
                hexAbbrev(cm.blockHash), cm.view, st.ithsEchoVoters.size());

            // IT-HS ECHO quorum: n - f.
            if (st.ithsEchoVoters.size() >= N - f &&
                !g_ithsAcceptSentForView.count(cm.view))
            {
                g_ithsAcceptSentForView[cm.view] = cm.blockHash;

                st.ithsAcceptSent = true;
                st.ithsAcceptVoters.insert(selfID);

                sendITHSAccept(cm.view, cm.blockHash, cm.data);

                CLOG_DEBUG(Overlay, "[IT-HS] Sent ACCEPT block {} view {}",
                           hexAbbrev(cm.blockHash), cm.view);
            }
        }
        break;

    // ================================================================
    case CUSTOM_ITHS_ACCEPT:
    {
        st.ithsAcceptVoters.insert(sender);

        CLOG_DEBUG(
            Overlay, "[IT-HS] Received ACCEPT block {} view {} accepts={}",
            hexAbbrev(cm.blockHash), cm.view, st.ithsAcceptVoters.size());

        // Relay ACCEPT after f + 1 votes, including for prior views.
        //
        if (st.ithsAcceptVoters.size() >= f + 1 &&
            !g_ithsAcceptSentForView.count(cm.view))
        {
            g_ithsAcceptSentForView[cm.view] = cm.blockHash;

            st.ithsAcceptSent = true;
            st.ithsAcceptVoters.insert(selfID);

            sendITHSAccept(cm.view, cm.blockHash, cm.data);

            CLOG_DEBUG(Overlay, "[IT-HS] Boosted ACCEPT block {} view {}",
                       hexAbbrev(cm.blockHash), cm.view);
        }

        // Main lock rule: n-f ACCEPT -> set lock and send LOCK.
        // Only do lock update for the current view.
        if (cm.view == currentView && st.ithsAcceptVoters.size() >= N - f &&
            !g_ithsLockSentForView.count(cm.view))
        {
            g_ithsLockView = cm.view;
            g_ithsLockBlock = cm.blockHash;

            g_ithsLockSentForView[cm.view] = cm.blockHash;

            st.ithsLockSent = true;
            st.ithsLockVoters.insert(selfID);

            sendITHSLock(cm.view, cm.blockHash, cm.data);

            CLOG_DEBUG(Overlay, "[IT-HS] Set lock=({},{}) and sent LOCK",
                       cm.view, hexAbbrev(cm.blockHash));
        }

        break;
    }

        // ================================================================

    case CUSTOM_ITHS_LOCK:
    {
        if (cm.view == currentView)
        {
            st.ithsLockVoters.insert(sender);

            CLOG_DEBUG(
                Overlay, "[IT-HS] Received LOCK block {} view {} locks={}",
                hexAbbrev(cm.blockHash), cm.view, st.ithsLockVoters.size());

            // Relay COMMIT after an n - f LOCK quorum
            // local commit occurs on a COMMIT quorum
            if (st.ithsLockVoters.size() >= N - f &&
                !g_ithsCommitSentForView.count(cm.view))
            {
                g_ithsCommitSentForView[cm.view] = cm.blockHash;

                st.ithsCommitSent = true;
                st.ithsCommitVoters.insert(selfID);

                sendITHSCommit(cm.view, cm.blockHash, cm.data);

                CLOG_DEBUG(
                    Overlay,
                    "[IT-HS] Sent COMMIT after LOCK quorum block {} view {}",
                    hexAbbrev(cm.blockHash), cm.view);
            }
        }

        break;
    }
    // ================================================================
    case CUSTOM_ITHS_COMMIT:
    {
        st.ithsCommitVoters.insert(sender);

        CLOG_DEBUG(
            Overlay, "[IT-HS] Received COMMIT block {} view {} commits={}",
            hexAbbrev(cm.blockHash), cm.view, st.ithsCommitVoters.size());

        // Relay COMMIT after f + 1 votes, including for prior views.
        //
        if (st.ithsCommitVoters.size() >= f + 1 &&
            !g_ithsCommitSentForView.count(cm.view))
        {
            g_ithsCommitSentForView[cm.view] = cm.blockHash;

            st.ithsCommitSent = true;
            st.ithsCommitVoters.insert(selfID);

            sendITHSCommit(cm.view, cm.blockHash, cm.data);

            CLOG_DEBUG(Overlay, "[IT-HS] Boosted COMMIT block {} view {}",
                       hexAbbrev(cm.blockHash), cm.view);
        }

        // Apply the batch and advance currentView after an n - f COMMIT quorum.
        //
        if (cm.view == currentView && st.ithsCommitVoters.size() >= N - f &&
            st.committedView < cm.view)
        {
            st.committedView = cm.view;
            st.committedBlock = cm.blockHash;
            latestCommittedView = cm.view;
            latestCommittedBlock = cm.blockHash;

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
                      "[IT-HS COMMITTED] block={} view={} txns={} nextView={}",
                      hexAbbrev(cm.blockHash), cm.view,
                      batch.transactions.size(), cm.view + 1);

            ackClientBatchesForBlock(cm.view, cm.blockHash);

            currentView = cm.view + 1;
            lastCollectSentView = UINT64_MAX;

            BlockKey nextKey{currentView, Hash()};
            g_txn[nextKey].proposalSentForView = false;

            cleanupOldTxnStates();
            deliverBufferedForCurrentView();

            if (latestCommittedView == currentView - 1)
            {
                prop();
            }
        }

        break;
    }
    }
}

} // namespace stellar
