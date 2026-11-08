// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "lib/util/finally.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/ShabdizArchive.h"
#include "overlay/ShabdizBatch.h"
#include "overlay/ShabdizConsensus.h"
#include "overlay/experiments/ExperimentState.h"
#include "scp/LocalNode.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <unistd.h>

namespace stellar
{
using namespace experiment;
using PaperShabdiz = ShabdizConsensus<NodeID, Hash>;
namespace
{
constexpr uint32_t WIRE_VERSION = 1;
constexpr uint32_t WORK_BATCH = 100;
constexpr size_t BLOCK_CACHE_SIZE = 128;
using Clock = std::chrono::steady_clock;

Hash
blockDigest(uint64_t view, Hash const& parent, std::string const& data)
{
    // Domain-separated, canonical commitment to ALL block fields.
    return sha256(xdr::xdr_to_opaque(xdr::xstring<>("shabdiz-block-v1"), view,
                                     parent, xdr::xstring<90000>(data)));
}

ShabdizPacket
encode(PaperShabdiz::Message const& m)
{
    ShabdizPacket p{};
    p.wireVersion = WIRE_VERSION;
    p.kind = static_cast<uint32_t>(m.kind);
    p.view = m.view;
    p.blockView = m.value.view;
    p.blockHash = m.value.hash;
    p.origin = m.origin;
    p.dependencies.assign(m.dependencies.begin(), m.dependencies.end());
    p.selected.assign(m.selected.begin(), m.selected.end());
    p.hasBlock = m.block.has_value();
    if (m.block)
    {
        p.parentHash = m.block->parent;
        p.data = m.block->data;
    }
    return p;
}

PaperShabdiz::Message
decode(ShabdizPacket const& p)
{
    PaperShabdiz::Message m;
    m.kind = static_cast<PaperShabdiz::Kind>(p.kind);
    m.view = p.view;
    m.value = {p.blockView, p.blockHash};
    m.origin = p.origin;
    m.dependencies.assign(p.dependencies.begin(), p.dependencies.end());
    m.selected.assign(p.selected.begin(), p.selected.end());
    if (p.hasBlock)
        m.block = PaperShabdiz::Block{p.blockView, p.blockHash, p.parentHash,
                                      std::string(p.data)};
    return m;
}

std::shared_ptr<StellarMessage>
wireMessage(ShabdizPacket const& p)
{
    auto bytes = xdr::xdr_to_opaque(p);
    if (bytes.size() > 100000)
        throw std::runtime_error("Shabdiz packet exceeds the overlay envelope");
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    auto& cm = msg->customMessage();
    cm.msgType = CUSTOM_SHABDIZ_PAPER;
    cm.view = p.view;
    cm.blockHash = p.blockHash;
    cm.data.assign(reinterpret_cast<char const*>(bytes.data()), bytes.size());
    return msg;
}
}

struct ShabdizRuntime
{
    NodeID self;
    std::set<NodeID> members;
    std::unique_ptr<PaperShabdiz> engine;
    // Files are an archive of blocks, not retained consensus votes. Only a
    // bounded working cache is in memory; ancestry remains fetchable after GC.
    std::unique_ptr<ShabdizArchive> archive;
    std::map<Hash, ShabdizBatch> parsed;
    std::deque<Hash> parsedOrder;
    std::map<Hash, PaperShabdiz::Block> cache;
    std::deque<Hash> cacheOrder;
    std::map<Hash, std::string> work;
    std::deque<Hash> workOrder;
    std::map<Hash, std::vector<ClientAck>> localAcks;
    std::map<Hash, std::set<NodeID>> forwardedTo;
    Hash applied{};
    bool started = false, timerArmed = false, forced = false, pumping = false;
    uint64_t generation = 0, forwardedView = 0;
    uint64_t fastProposals = 0, slowProposals = 0, collects = 0, wishes = 0;
    bool newWork = false, retryWork = false;
    Clock::time_point deadline, retry, startedAt;

    ShabdizBatch const*
    batch(std::string const& data, Hash const& id)
    {
        auto it = parsed.find(id);
        if (it != parsed.end())
            return &it->second;
        auto result = parseShabdizBatch(data, SERVER_BATCH_SIZE);
        if (!result)
            return nullptr;
        it = parsed.emplace(id, std::move(*result)).first;
        parsedOrder.push_back(id);
        while (parsed.size() > BLOCK_CACHE_SIZE)
        {
            parsed.erase(parsedOrder.front());
            parsedOrder.pop_front();
        }
        return &it->second;
    }
    void
    remember(PaperShabdiz::Block const& b)
    {
        if (cache.count(b.hash))
            return;
        PaperShabdiz::Message m;
        m.kind = PaperShabdiz::Kind::BlockData;
        m.value = {b.view, b.hash};
        m.block = b;
        auto bytes = xdr::xdr_to_opaque(encode(m));
        archive->put("b-" + binToHex(b.hash),
                     std::string(reinterpret_cast<char const*>(bytes.data()),
                                 bytes.size()));
        cacheBlock(b);
    }
    void
    cacheBlock(PaperShabdiz::Block const& b)
    {
        if (cache.emplace(b.hash, b).second)
            cacheOrder.push_back(b.hash);
        while (cache.size() > BLOCK_CACHE_SIZE)
        {
            cache.erase(cacheOrder.front());
            cacheOrder.pop_front();
        }
    }
    std::optional<PaperShabdiz::Block>
    read(Hash const& h)
    {
        auto it = cache.find(h);
        if (it != cache.end())
            return it->second;
        auto stored = archive->get("b-" + binToHex(h));
        if (!stored)
            return {};
        std::vector<char> bytes(stored->begin(), stored->end());
        ShabdizPacket p;
        xdr::xdr_from_opaque(bytes, p);
        auto m = decode(p);
        if (!m.block || m.block->hash != h ||
            blockDigest(m.block->view, m.block->parent, m.block->data) != h)
            throw std::runtime_error("Corrupt Shabdiz block archive");
        cacheBlock(*m.block);
        return m.block;
    }
    bool
    executed(Hash const& h)
    {
        return archive->contains("e-" + binToHex(h));
    }
    bool
    addWork(Hash const& id, std::string const& data)
    {
        if (executed(id))
            return false;
        if (work.emplace(id, data).second)
        {
            workOrder.push_back(id);
            return true;
        }
        return false;
    }
    std::optional<std::string>
    nextData(Hash parent)
    {
        // Exclude batches already on the proposed ancestry, including blocks
        // committed by the engine but not yet applied by the adapter.
        std::set<Hash> onBranch;
        while (parent != applied)
        {
            auto b = read(parent);
            if (!b)
                return {};
            onBranch.insert(sha256(b->data));
            parent = b->parent;
        }
        for (auto const& id : workOrder)
        {
            auto it = work.find(id);
            if (it != work.end() && !onBranch.count(id))
                return it->second;
        }
        return {};
    }
};

bool
OverlayManagerImpl::paperShabdizEnabled() const
{
    return !ITHS_MODE && !PBFT_MODE && !ENABLE_SCP_TRACKING && !TETRABFT_MODE &&
           !SIMPLE_IT_MODE && !FORGET_IT_MODE;
}

void
OverlayManagerImpl::initializePaperShabdiz()
{
    if (mPaperShabdiz || !paperShabdizEnabled())
        return;
    auto r = std::make_shared<ShabdizRuntime>();
    auto const& cfg = mApp.getConfig();
    r->self = cfg.NODE_SEED.getPublicKey();
    r->members.insert(r->self);
    LocalNode::forAllNodes(cfg.QUORUM_SET, [&](NodeID const& id) {
        r->members.insert(id);
        return true;
    });
    if (r->members.size() < 4 || r->members.size() > 256)
        throw std::invalid_argument("Shabdiz requires 4..256 fixed members");
    // Each experiment starts with genesis. This archive supports chain fetch,
    // not crash/restart recovery of the consensus state.
    auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    auto archivePath =
        std::filesystem::path(cfg.BUCKET_DIR_PATH) /
        ("shabdiz-" + std::to_string(getpid()) + "-" + std::to_string(stamp));
    r->archive = std::make_unique<ShabdizArchive>(archivePath);
    auto raw = r.get();
    r->engine = std::make_unique<PaperShabdiz>(
        r->self, std::vector<NodeID>(r->members.begin(), r->members.end()),
        Hash{}, [raw](Hash const& h) { return raw->read(h); },
        [raw](PaperShabdiz::Block const& b) { raw->remember(b); }, blockDigest,
        [raw](Hash const& h) { return raw->nextData(h); },
        [raw](std::string const& data) {
            return raw->batch(data, sha256(data)) != nullptr;
        },
        NonPrune_MODE, SHABDIZ_FAST_PATH_ONLY);
    CLOG_INFO(Overlay,
              "[SHABDIZ CONFIG] members={} quorum={} leader=round-robin "
              "prepared_pipeline=true fast_path_only={} archive={}",
              r->members.size(), r->engine->quorum(), SHABDIZ_FAST_PATH_ONLY,
              archivePath.string());
    mPaperShabdiz = std::move(r);
    mPaperShabdizTimer = std::make_unique<VirtualTimer>(mApp);
}

void
OverlayManagerImpl::pumpPaperShabdiz()
{
    auto& r = *mPaperShabdiz;
    if (r.pumping || !r.started)
        return;
    r.archive->check();
    r.pumping = true;
    auto reset = gsl::finally([&r]() { r.pumping = false; });
    for (;;)
    {
        auto executed = r.engine->takeExecuted();
        if (executed.empty())
            break;
        for (auto const& b : executed)
        {
            auto id = sha256(b.data);
            size_t appliedTxns = 0;
            if (!r.executed(id))
            {
                auto batch = r.batch(b.data, id);
                if (!batch)
                    throw std::runtime_error("Invalid committed Shabdiz batch");
                for (auto const& op : *batch)
                    if (op.write)
                        g_kvStore[op.key] = op.value;
                appliedTxns = batch->size();
                r.archive->put("e-" + binToHex(id), {});
                auto acks = r.localAcks.find(id);
                if (acks != r.localAcks.end())
                {
                    auto key = BlockKey{b.view, b.hash};
                    g_blockClientAcks[key] = std::move(acks->second);
                    r.localAcks.erase(acks);
                    ackClientBatchesForBlock(b.view, b.hash);
                }
            }
            r.work.erase(id);
            r.forwardedTo.erase(id);
            r.applied = b.hash;
            CLOG_INFO(
                Overlay,
                "[FAST COMMITTED] block={} view={} txns={} currentView={}",
                hexAbbrev(b.hash), b.view, appliedTxns, r.engine->view());
        }
        // Remove all executed IDs, not only the head: leaders may consume work
        // in different arrival orders during recovery.
        r.workOrder.erase(
            std::remove_if(r.workOrder.begin(), r.workOrder.end(),
                           [&](Hash const& h) { return !r.work.count(h); }),
            r.workOrder.end());
        r.engine->workAvailable();
    }
    auto peers = getAuthenticatedPeers();
    for (auto const& out : r.engine->takeOutgoing())
    {
        r.fastProposals += out.message.kind == PaperShabdiz::Kind::FastProposal;
        r.slowProposals += out.message.kind == PaperShabdiz::Kind::SlowProposal;
        r.collects += out.message.kind == PaperShabdiz::Kind::Collect;
        r.wishes += out.message.kind == PaperShabdiz::Kind::Wish;
        auto msg = wireMessage(encode(out.message));
        // No flood-cache coalescing: votes from distinct authenticated senders
        // must remain distinct, even when their bytes are identical.
        for (auto const& peer : peers)
            if (r.members.count(peer.first) &&
                (!out.to || *out.to == peer.first))
                peer.second->sendMessage(msg);
    }
    // Client ingress is transport, not an extra all-to-all consensus phase.
    // Forward only to leader(v), and retry at view changes or the retry tick.
    // The leader's paper proposal supplies the batch to all other replicas.
    if (r.newWork || r.forwardedView != r.engine->view())
    {
        r.newWork = false;
        r.forwardedView = r.engine->view();
        auto leader = r.engine->leader(r.engine->view());
        auto peer = peers.find(leader);
        if (!(leader == r.self) && peer != peers.end())
            for (auto const& ack : r.localAcks)
            {
                auto it = r.work.find(ack.first);
                if (it == r.work.end())
                    continue;
                auto& sent = r.forwardedTo[ack.first];
                if (!r.retryWork && sent.count(leader))
                    continue;
                ShabdizPacket p{};
                p.wireVersion = WIRE_VERSION;
                p.kind = WORK_BATCH;
                p.blockHash = ack.first;
                p.data = it->second;
                p.origin = r.self;
                peer->second->sendMessage(wireMessage(p));
                sent.insert(leader);
            }
        r.retryWork = false;
    }
    if (!SHABDIZ_FAST_PATH_ONLY && r.generation != r.engine->timerGeneration())
    {
        r.generation = r.engine->timerGeneration();
        auto w = r.engine->confirmedView();
        auto maxMillis =
            uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                         Clock::duration::max())
                         .count() /
                     4);
        auto step = std::max<uint64_t>(1, SHABDIZ_VIEW_TIMEOUT_STEP_MS);
        auto duration =
            SHABDIZ_VIEW_TIMEOUT_MS +
            std::min(w, (maxMillis - SHABDIZ_VIEW_TIMEOUT_MS) / step) * step;
        r.deadline = Clock::now() + std::chrono::milliseconds(duration);
    }
}

void
OverlayManagerImpl::tickPaperShabdiz()
{
    if (!paperShabdizEnabled() || mShuttingDown)
        return;
    initializePaperShabdiz();
    auto& r = *mPaperShabdiz;
    if (!r.timerArmed)
    {
        r.timerArmed = true;
        mPaperShabdizTimer->expires_from_now(std::chrono::milliseconds(100));
        mPaperShabdizTimer->async_wait(
            [this]() {
                mPaperShabdiz->timerArmed = false;
                tickPaperShabdiz();
            },
            VirtualTimer::onFailureNoop);
    }
    if (!r.started)
    {
        size_t connected = 1;
        for (auto const& peer : getAuthenticatedPeers())
            connected += r.members.count(peer.first);
        if (connected < r.engine->quorum())
            return;
        r.started = true;
        r.startedAt = Clock::now();
        r.retry = r.startedAt + std::chrono::seconds(1);
        if (mApp.getConfig().SEND_CUSTOM_MESSAGE && !g_clientListenerActive)
            startClientListener(CLIENT_PORT);
        r.engine->start();
        CLOG_INFO(Overlay,
                  "Shabdiz started; fixed quorum connected, fast_path_only={}",
                  SHABDIZ_FAST_PATH_ONLY);
    }
    // SEND_CUSTOM_MESSAGE remains the client ingress selector, not leader(v).
    // Queue full batches locally; pump forwards them only to the current
    // leader.
    if (mApp.getConfig().SEND_CUSTOM_MESSAGE && g_clientListenerActive)
    {
        TransactionBatch batch;
        std::vector<ClientAck> acks;
        while (tryBuildServerBatch(batch, acks))
        {
            auto data = batch.serialize();
            auto id = sha256(data);
            if (!r.batch(data, id))
                throw std::runtime_error("Invalid local Shabdiz batch");
            r.addWork(id, data);
            auto& local = r.localAcks[id];
            local.insert(local.end(), acks.begin(), acks.end());
            r.newWork = true;
        }
    }
    if (!SHABDIZ_FAST_PATH_ONLY && !r.forced && Clock::now() - r.startedAt >=
                         std::chrono::seconds(FORCE_COLLECT_AFTER_SEC))
    {
        r.forced = true;
        r.engine->forceSlowViews(MAX_COLLECT_ATTEMPTS);
    }
    r.engine->workAvailable();
    pumpPaperShabdiz();
    if (!SHABDIZ_FAST_PATH_ONLY && Clock::now() >= r.deadline)
    {
        r.engine->timeout();
        // Once expired, resend the highest wish until confirmation/request
        // resets the timer, as in section 10.2.
        r.deadline = Clock::now() + std::chrono::seconds(1);
        pumpPaperShabdiz();
    }
    if (Clock::now() >= r.retry)
    {
        r.retry = Clock::now() + std::chrono::seconds(1);
        // Keep the notebook's memory-log format. Elements are PS entries;
        // archived application blocks are separate from consensus retention.
        CLOG_INFO(Overlay,
                  "OverlayManagerImpl tick; MEMORY RSS: {} MB, number of "
                  "elements: {}",
                  getRSS_MB(), r.engine->prepareHistorySize());
        r.engine->retransmit();
        r.newWork = true;
        r.retryWork = true;
        CLOG_INFO(Overlay,
                  "[SHABDIZ STATE] view={} prepared={} committed={} fast={} "
                  "slow={} collects={} wishes={}",
                  r.engine->view(), r.engine->prepared().view,
                  r.engine->committed().view, r.fastProposals, r.slowProposals,
                  r.collects, r.wishes);
        pumpPaperShabdiz();
    }
}

void
OverlayManagerImpl::receivePaperShabdiz(CustomMessage const& cm,
                                        NodeID const& sender)
{
    initializePaperShabdiz();
    auto& r = *mPaperShabdiz;
    if (!r.members.count(sender))
        return;
    ShabdizPacket p;
    try
    {
        std::vector<char> bytes(cm.data.begin(), cm.data.end());
        xdr::xdr_from_opaque(bytes, p);
    }
    catch (std::exception const&)
    {
        CLOG_WARNING(Overlay, "[SHABDIZ REJECT] malformed packet");
        return;
    }
    if (p.wireVersion != WIRE_VERSION)
        return;
    if (p.kind == WORK_BATCH)
    {
        if (!(p.origin == sender) || p.hasBlock || !p.dependencies.empty() ||
            !p.selected.empty() || sha256(p.data) != p.blockHash ||
            !r.batch(p.data, p.blockHash))
            return;
        r.addWork(p.blockHash, p.data);
        r.engine->workAvailable();
    }
    else
    {
        if (p.kind > static_cast<uint32_t>(PaperShabdiz::Kind::BlockData))
            return;
        auto m = decode(p);
        bool blockMessage = m.kind == PaperShabdiz::Kind::FastProposal ||
                            m.kind == PaperShabdiz::Kind::SlowProposal ||
                            m.kind == PaperShabdiz::Kind::BlockData;
        if (m.block.has_value() != blockMessage ||
            (!blockMessage && !p.data.empty()) ||
            (m.kind != PaperShabdiz::Kind::Ready && !m.dependencies.empty()) ||
            (m.kind != PaperShabdiz::Kind::SlowProposal && !m.selected.empty()))
            return;
        r.engine->receive(sender, m);
    }
    pumpPaperShabdiz();
}
} // namespace stellar
