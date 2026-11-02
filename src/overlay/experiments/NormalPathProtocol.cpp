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
        int enabled = int(cfg.TETRABFT_MODE) + int(cfg.SIMPLE_IT_MODE) +
                      int(cfg.FORGET_IT_MODE);
        if (enabled != 1 || PBFT_MODE || ITHS_MODE || ENABLE_SCP_TRACKING)
            throw std::invalid_argument(
                "Enable only one custom consensus protocol");
        auto const& leader = cfg.TETRABFT_MODE    ? cfg.TETRABFT_LEADER
                             : cfg.SIMPLE_IT_MODE ? cfg.SIMPLE_IT_LEADER
                                                  : cfg.FORGET_IT_LEADER;
        mNormalLeader = KeyUtils::fromStrKey<PublicKey>(leader);
        mNormalMembers.insert(mApp.getConfig().NODE_SEED.getPublicKey());
        LocalNode::forAllNodes(cfg.QUORUM_SET, [&](NodeID const& id) {
            mNormalMembers.insert(id);
            return true;
        });
        if (mNormalMembers.size() < 4 || !mNormalMembers.count(mNormalLeader) ||
            cfg.SEND_CUSTOM_MESSAGE !=
                (mApp.getConfig().NODE_SEED.getPublicKey() == mNormalLeader))
            throw std::invalid_argument(
                "Normal-path protocols need >=4 fixed members and "
                "SEND_CUSTOM_MESSAGE only on their configured leader");
        mNormalQuorum = mNormalMembers.size() - (mNormalMembers.size() - 1) / 3;
        CLOG_INFO(Overlay,
                  "[{} CONFIG] members={} quorum={} simple_optimistic={} "
                  "normal_path_only=true",
                  normalPathName(), mNormalMembers.size(), mNormalQuorum,
                  cfg.SIMPLE_IT_MODE && cfg.SIMPLE_IT_OPTIMISTIC);
    }
}

bool
OverlayManagerImpl::normalPathEnabled() const
{
    auto const& cfg = mApp.getConfig();
    return cfg.TETRABFT_MODE || cfg.SIMPLE_IT_MODE || cfg.FORGET_IT_MODE;
}

char const*
OverlayManagerImpl::normalPathName() const
{
    auto const& cfg = mApp.getConfig();
    return cfg.TETRABFT_MODE    ? "TETRA"
           : cfg.SIMPLE_IT_MODE ? "SIMPLE-IT"
                                : "FORGET-IT";
}

CustomMessageType
OverlayManagerImpl::normalProposalType() const
{
    auto const& cfg = mApp.getConfig();
    return cfg.TETRABFT_MODE    ? CUSTOM_TETRA_PROPOSE
           : cfg.SIMPLE_IT_MODE ? CUSTOM_SIMPLE_PROPOSE
                                : CUSTOM_FORGET_PREPARE;
}

uint64_t
OverlayManagerImpl::normalMetadata(uint64_t seq) const
{
    auto const& cfg = mApp.getConfig();
    // Simple-IT parent round; Forget-IT fresh instance's view 1; Tetra view 0.
    return cfg.SIMPLE_IT_MODE ? seq - 1 : cfg.FORGET_IT_MODE ? 1 : 0;
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
    cm.vp = normalMetadata(seq);
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
    if (!(mApp.getConfig().NODE_SEED.getPublicKey() == mNormalLeader) ||
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
    if (mApp.getConfig().TETRABFT_MODE)
        slot.tetra.propose(digest);
    else if (mApp.getConfig().SIMPLE_IT_MODE)
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
    auto const& cfg = mApp.getConfig();
    // Simple-IT continues the RBC's auxiliary messages briefly after execution.
    bool retainedSimple =
        cfg.SIMPLE_IT_MODE && seq <= mNormalExecuted && mNormalSlots.count(seq);
    if (!mNormalMembers.count(sender) || seq == 0 ||
        cm.vp != normalMetadata(seq) ||
        (!retainedSimple && seq <= mNormalExecuted) ||
        (seq > mNormalExecuted && seq - mNormalExecuted > NORMAL_PATH_WINDOW))
        return;
    if (cm.msgType == normalProposalType())
    {
        if (!(sender == mNormalLeader) || sha256(cm.data) != cm.blockHash)
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
        bool accepted = cfg.TETRABFT_MODE ? slot.tetra.propose(cm.blockHash)
                        : cfg.SIMPLE_IT_MODE
                            ? slot.simple.propose(cm.blockHash)
                            : slot.forget.propose(cm.blockHash);
        if (!accepted)
            return;
        slot.digest = cm.blockHash;
        slot.proposed = true;
        slot.data = cm.data;
    }
    else
    {
        if (!cm.data.empty()) // votes contain only the batch digest
            return;
        if (cfg.TETRABFT_MODE)
            mNormalSlots[seq].tetra.vote(cm.msgType - CUSTOM_TETRA_VOTE1 + 1,
                                         sender, cm.blockHash);
        else if (cfg.SIMPLE_IT_MODE)
        {
            if (!cfg.SIMPLE_IT_OPTIMISTIC &&
                (cm.msgType == CUSTOM_SIMPLE_VOTE ||
                 cm.msgType == CUSTOM_SIMPLE_ACK))
                return;
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
    auto const& cfg = mApp.getConfig();
    bool decided = cfg.TETRABFT_MODE    ? advanceTetraBFT(seq)
                   : cfg.FORGET_IT_MODE ? advanceForgetIT(seq)
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
        if (mApp.getConfig().NODE_SEED.getPublicKey() == mNormalLeader)
            ackClientBatchesForBlock(mNormalExecuted, ready.digest);
        if (!cfg.SIMPLE_IT_MODE)
            mNormalSlots.erase(it);
    }
    if (cfg.SIMPLE_IT_MODE)
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
    if (mApp.getConfig().NODE_SEED.getPublicKey() == mNormalLeader)
        normalProp();
}

} // namespace stellar
