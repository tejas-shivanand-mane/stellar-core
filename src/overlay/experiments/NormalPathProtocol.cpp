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
        // Match the existing custom protocols: SEND_CUSTOM_MESSAGE selects
        // the proposer. Followers learn and pin its authenticated identity from
        // the first valid proposal; no separate leader key is configured.
        if (cfg.SEND_CUSTOM_MESSAGE)
            mNormalLeader = cfg.NODE_SEED.getPublicKey();
        mNormalMembers.insert(mApp.getConfig().NODE_SEED.getPublicKey());
        LocalNode::forAllNodes(cfg.QUORUM_SET, [&](NodeID const& id) {
            mNormalMembers.insert(id);
            return true;
        });
        if (mNormalMembers.size() < 4)
            throw std::invalid_argument(
                "Normal-path protocols need at least four fixed members");
        mNormalQuorum = mNormalMembers.size() - (mNormalMembers.size() - 1) / 3;
        CLOG_INFO(Overlay,
                  "[{} CONFIG] members={} quorum={} simple_optimistic={} "
                  "normal_path_only=true payload=digest_votes "
                  "simple_commit=round_only",
                  normalPathName(), mNormalMembers.size(), mNormalQuorum,
                  SIMPLE_IT_MODE && SIMPLE_IT_OPTIMISTIC);
    }
}

bool
OverlayManagerImpl::normalPathEnabled() const
{
    return TETRABFT_MODE || SIMPLE_IT_MODE || FORGET_IT_MODE;
}

char const*
OverlayManagerImpl::normalPathName() const
{
    return TETRABFT_MODE ? "TETRA" : SIMPLE_IT_MODE ? "SIMPLE-IT" : "FORGET-IT";
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
    cm.view = seq; // independent instance number, NOT a recovery view
    cm.vp = type == CUSTOM_SIMPLE_COMMIT ? 0 : normalMetadata(seq);
    // Forget-IT's first-view guard is SAFE, explicitly encoded on vote-2.
    cm.dependencyVp = type == CUSTOM_FORGET_VOTE2 ? 1 : 0;
    cm.blockHash = digest;
    cm.data = data;
    // Direct authenticated channels: identical votes from distinct senders
    // must not be collapsed by the overlay's content-based flood cache.
    for (auto const& peer : getAuthenticatedPeers())
        if (mNormalMembers.count(peer.first))
            peer.second->sendMessage(msg);
}

void
OverlayManagerImpl::normalProp()
{
    if (!mApp.getConfig().SEND_CUSTOM_MESSAGE ||
        mNormalNextSeq != mNormalExecuted + 1)
        return;
    TransactionBatch batch;
    std::vector<ClientAck> acks;
    if (!g_clientListenerActive || !tryBuildServerBatch(batch, acks))
        return;
    auto data = batch.serialize();
    auto digest = sha256(data);
    uint64_t seq = mNormalNextSeq++;
    g_blockClientAcks[BlockKey{seq, digest}] = std::move(acks);
    auto& slot = mNormalSlots[seq];
    slot.digest = digest;
    if (TETRABFT_MODE)
        slot.tetra.propose(digest);
    else if (SIMPLE_IT_MODE)
        slot.simple.propose(digest);
    else
        slot.forget.propose(digest);
    slot.proposed = true;
    slot.data = data;
    normalSend(normalProposalType(), seq, digest, data);
    normalAdvance(seq); // leader casts vote-1 as well
}

void
OverlayManagerImpl::handleNormalMessage(StellarMessage const& msg,
                                        NodeID const& sender)
{
    auto const& cm = msg.customMessage();
    auto seq = cm.view;
    // Bounded lookahead permits a faster replica to start the next instance
    // before this replica has received the last vote of the previous one.
    // Simple-IT continues the RBC's auxiliary messages briefly after execution.
    bool retainedSimple =
        SIMPLE_IT_MODE && seq <= mNormalExecuted && mNormalSlots.count(seq);
    if (!mNormalMembers.count(sender) || seq == 0 ||
        cm.vp != (cm.msgType == CUSTOM_SIMPLE_COMMIT ? 0 : normalMetadata(seq)) ||
        (!retainedSimple && seq <= mNormalExecuted) ||
        (seq > mNormalExecuted && seq - mNormalExecuted > NORMAL_PATH_WINDOW))
        return;
    if (cm.msgType == normalProposalType())
    {
        if ((mNormalLeader && !(sender == *mNormalLeader)) ||
            (!mNormalLeader && seq != 1) || sha256(cm.data) != cm.blockHash)
            return;
        try
        {
            auto batch = TransactionBatch::deserialize(cm.data);
            if (batch.transactions.size() != SERVER_BATCH_SIZE ||
                batch.serialize() != cm.data)
                return;
        }
        catch (std::exception const&)
        {
            return;
        }
        auto& slot = mNormalSlots[seq];
        bool accepted = TETRABFT_MODE    ? slot.tetra.propose(cm.blockHash)
                        : SIMPLE_IT_MODE ? slot.simple.propose(cm.blockHash)
                                         : slot.forget.propose(cm.blockHash);
        if (!accepted)
            return;
        // Bind only after membership, metadata, digest and payload validation.
        if (!mNormalLeader)
            mNormalLeader = sender;
        slot.digest = cm.blockHash;
        slot.proposed = true;
        slot.data = cm.data;
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
            mNormalSlots[seq].tetra.vote(cm.msgType - CUSTOM_TETRA_VOTE1 + 1,
                                         sender, cm.blockHash);
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
        else
        {
            if (cm.msgType == CUSTOM_FORGET_VOTE2 && cm.dependencyVp != 1)
                return; // only SAFE-guard first-view messages supported
            mNormalSlots[seq].forget.vote(cm.msgType - CUSTOM_FORGET_VOTE1 + 1,
                                          sender, cm.blockHash);
        }
    }
    normalAdvance(seq);
}

void
OverlayManagerImpl::normalAdvance(uint64_t seq)
{
    auto& slot = mNormalSlots.at(seq);
    if (!slot.proposed)
        return;
    bool decided = TETRABFT_MODE    ? advanceTetraBFT(seq)
                   : FORGET_IT_MODE ? advanceForgetIT(seq)
                                    : advanceSimpleIT(seq);
    if (decided)
        slot.decided = true;

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
            ackClientBatchesForBlock(mNormalExecuted, ready.digest);
        if (!SIMPLE_IT_MODE)
            mNormalSlots.erase(it);
    }
    if (SIMPLE_IT_MODE)
    {
        // Bounded retention lets slow RBC messages finish without re-execution.
        while (!mNormalSlots.empty() && mNormalExecuted > NORMAL_PATH_WINDOW &&
               mNormalSlots.begin()->first <
                   mNormalExecuted - NORMAL_PATH_WINDOW)
            mNormalSlots.erase(mNormalSlots.begin());
        auto next = mNormalSlots.find(mNormalExecuted + 1);
        if (next != mNormalSlots.end() && next->second.proposed &&
            seq != next->first)
            normalAdvance(next->first); // reconsider a now-safe parent
    }
    if (mApp.getConfig().SEND_CUSTOM_MESSAGE)
        normalProp();
}

} // namespace stellar
