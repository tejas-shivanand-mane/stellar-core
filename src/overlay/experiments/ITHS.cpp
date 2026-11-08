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
using Blog = ITHSBlogConsensus<NodeID>;
uint64_t
ithsBlogNow()
{
    // Part One assumes synchronized clocks. A per-process steady-clock epoch
    // would silently give different view boundaries on different machines.
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
bool
validITHSInput(std::string const& data)
{
    try
    {
        auto batch = TransactionBatch::deserialize(data);
        return data.size() <= 90000 &&
               batch.transactions.size() == SERVER_BATCH_SIZE &&
               batch.serialize() == data;
    }
    catch (std::exception const&)
    {
        return false;
    }
}
}

void
OverlayManagerImpl::sendITHSBlog(uint64_t seq, ITHSBlogPacket const& packet,
                                 std::optional<NodeID> const& recipient)
{
    auto bytes = xdr::xdr_to_opaque(packet);
    auto out = std::make_shared<StellarMessage>();
    out->type(CUSTOM_MESSAGE);
    auto& cm = out->customMessage();
    cm.msgType = CUSTOM_ITHS_BLOG;
    cm.view = seq; // instance, not the blog's protocol view
    cm.data.assign(bytes.begin(), bytes.end());
    for (auto const& peer : getAuthenticatedPeers())
        if (mNormalMembers.count(peer.first) &&
            (!recipient || peer.first == *recipient))
            peer.second->sendMessage(out);
}

void
OverlayManagerImpl::startITHSBlog(uint64_t seq, std::string const& data,
                                  uint64_t epoch)
{
    auto& slot = mNormalSlots[seq];
    if (slot.iths)
        return;
    if (!mNormalLeader || data.size() > 90000)
        throw std::invalid_argument(
            "IT-HS blog requires an ingress leader and batch <=90000 bytes");
    bool ingressStart = epoch == 0;
    if (ingressStart)
        epoch = ithsBlogNow();
    auto leaders = mNormalLeaders;
    std::rotate(leaders.begin(),
                std::find(leaders.begin(), leaders.end(), *mNormalLeader),
                leaders.end());
    slot.ithsInput = data;
    slot.iths = std::make_unique<Blog>(
        mApp.getConfig().NODE_SEED.getPublicKey(), std::move(leaders), data,
        epoch, ithsBlogNow(), ITHS_BLOG_DELTA_MS);
    slot.proposed = true;
    mITHSBlogActive.insert(seq);
    if (ingressStart)
    {
        // Workload initialization, not a consensus vote: distribute the input
        // and one common epoch. Initial input delivery plus SUGGEST fits the
        // 2-Delta proposal wait after GST; later views use shared boundaries.
        ITHSBlogPacket start;
        start.wireVersion = 1;
        start.kind = 0;
        start.epochMs = epoch;
        start.value = data;
        sendITHSBlog(seq, start);
    }
    auto pending = std::move(slot.ithsEarly);
    slot.ithsEarly.clear();
    for (auto const& p : pending)
        if (p.second.epochMs == epoch)
            slot.iths->receive(p.first,
                               {p.second.kind, p.second.view, p.second.lockView,
                                p.second.value},
                               ithsBlogNow());
}

void
OverlayManagerImpl::handleITHSBlogMessage(CustomMessage const& cm,
                                          NodeID const& sender)
{
    auto seq = cm.view;
    if (!mNormalMembers.count(sender) || !seq ||
        (seq > mNormalExecuted && seq - mNormalExecuted > NORMAL_PATH_WINDOW) ||
        (seq <= mNormalExecuted && !mNormalSlots.count(seq)))
        return;
    ITHSBlogPacket packet;
    try
    {
        std::vector<char> bytes(cm.data.begin(), cm.data.end());
        xdr::xdr_from_opaque(bytes, packet);
        if (packet.wireVersion != 1 || packet.kind > Blog::Commit ||
            !packet.epochMs)
            return;
        auto it = mNormalSlots.find(seq);
        if ((it == mNormalSlots.end() || !it->second.iths ||
             it->second.ithsInput != packet.value) &&
            !validITHSInput(packet.value))
            return;
    }
    catch (std::exception const&)
    {
        return;
    }
    if (packet.kind == 0)
    {
        if (packet.view || packet.lockView || (!mNormalLeader && seq != 1) ||
            (mNormalLeader && !(sender == *mNormalLeader)))
            return;
        if (!mNormalLeader)
            mNormalLeader = sender;
        startITHSBlog(seq, packet.value, packet.epochMs);
    }
    else
    {
        auto& slot = mNormalSlots[seq];
        if (!slot.iths)
        {
            auto same = [&](auto const& p) {
                return p.first == sender && p.second == packet;
            };
            if (std::find_if(slot.ithsEarly.begin(), slot.ithsEarly.end(),
                             same) == slot.ithsEarly.end())
                slot.ithsEarly.emplace_back(sender, packet);
            return;
        }
        if (slot.iths->epoch() != packet.epochMs)
            return;
        slot.iths->receive(
            sender, {packet.kind, packet.view, packet.lockView, packet.value},
            ithsBlogNow());
    }
    normalAdvance(seq);
}

bool
OverlayManagerImpl::advanceITHSBlog(uint64_t seq)
{
    auto& slot = mNormalSlots.at(seq);
    if (!slot.iths)
        return false;
    for (auto const& out : slot.iths->takeOutgoing())
    {
        ITHSBlogPacket packet;
        packet.wireVersion = 1;
        packet.kind = out.message.kind;
        packet.epochMs = slot.iths->epoch();
        packet.view = out.message.view;
        packet.lockView = out.message.lockView;
        packet.value = out.message.value;
        sendITHSBlog(seq, packet, out.recipient);
    }
    bool decided = !slot.decided && slot.iths->decision().has_value();
    if (decided)
    {
        slot.data = *slot.iths->decision();
        slot.digest = sha256(slot.data);
        mITHSBlogActive.erase(seq);
    }
    armITHSBlogTimer();
    return decided;
}

void
OverlayManagerImpl::armITHSBlogTimer()
{
    if (mShuttingDown)
        return;
    auto now = ithsBlogNow();
    std::optional<uint64_t> deadline;
    for (auto seq : mITHSBlogActive)
    {
        auto next = mNormalSlots.at(seq).iths->nextDeadline(now);
        if (next && (!deadline || *next < *deadline))
            deadline = next;
    }
    if (deadline == mITHSBlogDeadline)
        return;
    if (!mITHSBlogTimer)
        mITHSBlogTimer = std::make_unique<VirtualTimer>(mApp);
    mITHSBlogTimer->cancel();
    mITHSBlogDeadline = deadline;
    if (!deadline)
        return;
    mITHSBlogTimer->expires_from_now(
        std::chrono::milliseconds(*deadline > now ? *deadline - now : 1));
    mITHSBlogTimer->async_wait(
        [this]() {
            mITHSBlogDeadline.reset();
            tickITHSBlog();
        },
        VirtualTimer::onFailureNoop);
}

void
OverlayManagerImpl::tickITHSBlog()
{
    if (mShuttingDown)
        return;
    auto now = ithsBlogNow();
    std::vector<uint64_t> active(mITHSBlogActive.begin(),
                                 mITHSBlogActive.end());
    for (auto seq : active)
    {
        auto& core = *mNormalSlots.at(seq).iths;
        auto before = core.view();
        core.tick(now);
        if (core.view() != before)
            CLOG_INFO(Overlay, "[IT-HS-BLOG VIEW] instance={} view={}", seq,
                      core.view());
        normalAdvance(seq);
    }
    armITHSBlogTimer();
}
}
