// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

namespace stellar
{
using namespace experiment;
namespace
{
using Forget = ForgetITConsensus<NodeID>;
uint64_t
forgetNow()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
bool
validForgetValue(Forget::Value const& value)
{
    if (!value.valid())
        return false;
    if (value.tag != Forget::Value::Data)
        return true;
    try
    {
        auto batch = TransactionBatch::deserialize(value.data);
        return value.data.size() <= 90000 &&
               batch.transactions.size() == SERVER_BATCH_SIZE &&
               batch.serialize() == value.data;
    }
    catch (std::exception const&)
    {
        return false;
    }
}
}

void
OverlayManagerImpl::sendForgetIT(uint64_t seq, Forget::Message const& msg)
{
    ForgetITPacket packet;
    packet.wireVersion = 1;
    packet.kind = msg.kind;
    packet.view = msg.view;
    packet.valueKind = msg.value.tag;
    packet.value = msg.value.data;
    packet.guardKind = msg.guard.tag;
    packet.guard = msg.guard.data;
    auto bytes = xdr::xdr_to_opaque(packet);
    auto out = std::make_shared<StellarMessage>();
    out->type(CUSTOM_MESSAGE);
    auto& cm = out->customMessage();
    cm.msgType = CUSTOM_FORGET_NATIVE;
    cm.view = seq; // independent BA instance; packet.view is its protocol view
    cm.data.assign(bytes.begin(), bytes.end());
    for (auto const& peer : getAuthenticatedPeers())
        if (mNormalMembers.count(peer.first))
            peer.second->sendMessage(out);
}

void
OverlayManagerImpl::startForgetIT(uint64_t seq, std::string const& data)
{
    auto& slot = mNormalSlots[seq];
    if (slot.forget)
        return;
    if (!mNormalLeader || data.size() > 90000)
        throw std::invalid_argument(
            "Forget-IT requires a first leader and a batch <=90000 bytes");
    auto leaders = mNormalLeaders;
    auto first = std::find(leaders.begin(), leaders.end(), *mNormalLeader);
    std::rotate(leaders.begin(), first, leaders.end());
    // Preserve the benchmark's existing initial proposer. Later views follow
    // one fixed round-robin schedule; committing never advances the view.
    slot.forget = std::make_unique<Forget>(
        mApp.getConfig().NODE_SEED.getPublicKey(), std::move(leaders), data,
        forgetNow(), FORGET_IT_DELTA_MS);
    slot.proposed = true;
    auto early = std::move(slot.forgetEarly);
    slot.forgetEarly.clear();
    for (auto const& m : early)
        slot.forget->receive(m.first, m.second, forgetNow());
}

void
OverlayManagerImpl::handleForgetITMessage(CustomMessage const& cm,
                                          NodeID const& sender)
{
    auto seq = cm.view;
    if (!mNormalMembers.count(sender) || !seq ||
        (seq > mNormalExecuted && seq - mNormalExecuted > NORMAL_PATH_WINDOW) ||
        (seq <= mNormalExecuted && !mNormalSlots.count(seq)))
        return;
    Forget::Message msg;
    try
    {
        ForgetITPacket packet;
        std::vector<char> bytes(cm.data.begin(), cm.data.end());
        xdr::xdr_from_opaque(bytes, packet);
        if (packet.wireVersion != 1)
            return;
        msg = {packet.kind,
               packet.view,
               {packet.valueKind, packet.value},
               {packet.guardKind, packet.guard}};
        auto known = mNormalSlots.find(seq);
        auto valid = [&](Forget::Value const& value) {
            // The common value was already parsed when this instance started.
            // Full-value equality is retained; avoid reparsing every vote.
            if (value.tag == Forget::Value::Data &&
                known != mNormalSlots.end() && known->second.forget &&
                value.data == known->second.forget->state().input)
                return true;
            return validForgetValue(value);
        };
        if (!msg.view || msg.kind < Forget::Prepare ||
            msg.kind > Forget::Echo3 || !valid(msg.value) || !valid(msg.guard))
            return;
    }
    catch (std::exception const&)
    {
        return;
    }
    if (!mNormalLeader)
    {
        // Same ingress discovery as the existing benchmark, not leader
        // election.
        if (seq == 1 && msg.kind == Forget::Prepare && msg.view == 1 &&
            msg.value.tag == Forget::Value::Data &&
            msg.guard.tag == Forget::Value::Bottom)
            mNormalLeader = sender;
    }
    auto& slot = mNormalSlots[seq];
    if (!slot.forget)
    {
        if (mNormalLeader && sender == *mNormalLeader &&
            msg.kind == Forget::Prepare && msg.view == 1 &&
            msg.value.tag == Forget::Value::Data &&
            msg.guard.tag == Forget::Value::Bottom)
            startForgetIT(seq, msg.value.data);
        else
        {
            // Votes can overtake the initial proposal across TCP connections.
            auto same = [&](auto const& p) {
                return p.first == sender && p.second == msg;
            };
            if (std::find_if(slot.forgetEarly.begin(), slot.forgetEarly.end(),
                             same) == slot.forgetEarly.end())
                slot.forgetEarly.emplace_back(sender, msg);
            return;
        }
    }
    slot.forget->receive(sender, msg, forgetNow());
    normalAdvance(seq);
}

bool
OverlayManagerImpl::advanceForgetIT(uint64_t seq)
{
    auto& slot = mNormalSlots.at(seq);
    if (!slot.forget)
        return false;
    for (auto const& msg : slot.forget->takeOutgoing())
        sendForgetIT(seq, msg);
    if (!slot.decided && slot.forget->state().decision)
    {
        slot.data = *slot.forget->state().decision;
        slot.digest = sha256(slot.data);
        return true;
    }
    return false;
}

void
OverlayManagerImpl::tickForgetIT()
{
    // Message handlers drive the healthy path without waiting for this tick.
    // The timer advances views only under Algorithm 1's timeout/echo rules.
    auto now = forgetNow();
    std::vector<uint64_t> instances;
    for (auto const& slot : mNormalSlots)
        if (slot.second.forget)
            instances.push_back(slot.first);
    for (auto seq : instances)
    {
        auto it = mNormalSlots.find(seq);
        if (it == mNormalSlots.end())
            continue;
        auto oldView = it->second.forget->state().view;
        it->second.forget->tick(now);
        auto view = it->second.forget->state().view;
        if (oldView != view)
            CLOG_INFO(Overlay, "[FORGET-IT VIEW] instance={} view={}", seq,
                      view);
        normalAdvance(seq);
    }
}
}
