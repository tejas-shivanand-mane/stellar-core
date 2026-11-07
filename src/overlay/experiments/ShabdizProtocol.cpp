// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "lib/util/finally.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/ShabdizConsensus.h"
#include "overlay/experiments/ExperimentState.h"
#include "scp/LocalNode.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
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

bool
validBatch(std::string const& data)
{
    if (data.empty() || data.size() > 90000)
        return false;
    try
    {
        auto batch = TransactionBatch::deserialize(data);
        if (batch.transactions.size() != SERVER_BATCH_SIZE ||
            batch.serialize() != data)
            return false;
        for (auto const& txn : batch.transactions)
        {
            std::istringstream in(txn.payload);
            std::string op, key, value, extra;
            if (!(in >> op >> key))
                return false;
            if (op != "READ")
            {
                if ((op != "UPDATE" && op != "RMW") || !(in >> value))
                    return false;
            }
            if (in >> extra)
                return false;
        }
        return true;
    }
    catch (std::exception const&)
    {
        return false;
    }
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
    std::filesystem::path archive;
    std::map<Hash, PaperShabdiz::Block> cache;
    std::deque<Hash> cacheOrder;
    std::map<Hash, std::string> work;
    std::deque<Hash> workOrder;
    std::map<Hash, std::vector<ClientAck>> localAcks;
    Hash applied{};
    bool started = false, timerArmed = false, forced = false, pumping = false;
    uint64_t generation = 0;
    Clock::time_point deadline, retry, startedAt;

    void
    remember(PaperShabdiz::Block const& b)
    {
        auto path = archive / binToHex(b.hash);
        if (!std::filesystem::exists(path))
        {
            PaperShabdiz::Message m;
            m.kind = PaperShabdiz::Kind::BlockData;
            m.value = {b.view, b.hash};
            m.block = b;
            auto bytes = xdr::xdr_to_opaque(encode(m));
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<char const*>(bytes.data()),
                      bytes.size());
            out.close();
            if (!out)
                throw std::runtime_error("Cannot archive Shabdiz block");
        }
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
        auto path = archive / binToHex(h);
        if (!std::filesystem::exists(path))
            return {};
        std::ifstream in(path, std::ios::binary);
        if (!in)
            throw std::runtime_error("Cannot read Shabdiz block archive");
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        ShabdizPacket p;
        xdr::xdr_from_opaque(bytes, p);
        auto m = decode(p);
        if (!m.block || m.block->hash != h ||
            blockDigest(m.block->view, m.block->parent, m.block->data) != h)
            throw std::runtime_error("Corrupt Shabdiz block archive");
        cacheBlock(*m.block);
        return m.block;
    }
    std::filesystem::path
    executedPath(Hash const& h) const
    {
        return archive / ("executed-" + binToHex(h));
    }
    bool
    addWork(Hash const& id, std::string const& data)
    {
        if (std::filesystem::exists(executedPath(id)))
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
    r->archive =
        std::filesystem::path(cfg.BUCKET_DIR_PATH) /
        ("shabdiz-" + std::to_string(getpid()) + "-" + std::to_string(stamp));
    std::filesystem::create_directories(r->archive);
    auto raw = r.get();
    r->engine = std::make_unique<PaperShabdiz>(
        r->self, std::vector<NodeID>(r->members.begin(), r->members.end()),
        Hash{}, [raw](Hash const& h) { return raw->read(h); },
        [raw](PaperShabdiz::Block const& b) { raw->remember(b); }, blockDigest,
        [raw](Hash const& h) { return raw->nextData(h); }, validBatch,
        NonPrune_MODE);
    CLOG_INFO(Overlay,
              "[SHABDIZ CONFIG] members={} quorum={} leader=round-robin "
              "prepared_pipeline=true archive={}",
              r->members.size(), r->engine->quorum(), r->archive.string());
    mPaperShabdiz = std::move(r);
    mPaperShabdizTimer = std::make_unique<VirtualTimer>(mApp);
}

void
OverlayManagerImpl::pumpPaperShabdiz()
{
    auto& r = *mPaperShabdiz;
    if (r.pumping || !r.started)
        return;
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
            if (!std::filesystem::exists(r.executedPath(id)))
            {
                auto batch = TransactionBatch::deserialize(b.data);
                for (auto const& txn : batch.transactions)
                {
                    std::istringstream in(txn.payload);
                    std::string op, key, value;
                    in >> op >> key;
                    if (op == "UPDATE" || op == "RMW")
                    {
                        in >> value;
                        g_kvStore[key] = value;
                    }
                }
                appliedTxns = batch.transactions.size();
                std::ofstream marker(r.executedPath(id));
                marker << b.view << '\n';
                marker.close();
                if (!marker)
                    throw std::runtime_error("Cannot mark Shabdiz execution");
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
    for (auto const& out : r.engine->takeOutgoing())
    {
        auto msg = wireMessage(encode(out.message));
        // No flood-cache coalescing: votes from distinct authenticated senders
        // must remain distinct, even when their bytes are identical.
        for (auto const& peer : getAuthenticatedPeers())
            if (r.members.count(peer.first) &&
                (!out.to || *out.to == peer.first))
                peer.second->sendMessage(msg);
    }
    if (r.generation != r.engine->timerGeneration())
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
                  "Shabdiz started; fixed quorum connected, view module armed");
    }
    // SEND_CUSTOM_MESSAGE remains the client ingress selector, not leader(v).
    // Disseminate each full batch so every rotating leader has requested work.
    if (mApp.getConfig().SEND_CUSTOM_MESSAGE && g_clientListenerActive)
    {
        TransactionBatch batch;
        std::vector<ClientAck> acks;
        while (tryBuildServerBatch(batch, acks))
        {
            auto data = batch.serialize();
            if (!validBatch(data))
                throw std::runtime_error("Invalid local Shabdiz batch");
            auto id = sha256(data);
            r.addWork(id, data);
            auto& local = r.localAcks[id];
            local.insert(local.end(), acks.begin(), acks.end());
            ShabdizPacket p{};
            p.wireVersion = WIRE_VERSION;
            p.kind = WORK_BATCH;
            p.blockHash = id;
            p.data = data;
            p.origin = r.self;
            auto msg = wireMessage(p);
            for (auto const& peer : getAuthenticatedPeers())
                if (r.members.count(peer.first))
                    peer.second->sendMessage(msg);
        }
    }
    if (!r.forced && Clock::now() - r.startedAt >=
                         std::chrono::seconds(FORCE_COLLECT_AFTER_SEC))
    {
        r.forced = true;
        r.engine->forceSlowViews(MAX_COLLECT_ATTEMPTS);
    }
    r.engine->workAvailable();
    pumpPaperShabdiz();
    if (Clock::now() >= r.deadline)
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
        for (auto const& ack : r.localAcks)
        {
            auto it = r.work.find(ack.first);
            if (it == r.work.end())
                continue;
            ShabdizPacket p{};
            p.wireVersion = WIRE_VERSION;
            p.kind = WORK_BATCH;
            p.blockHash = ack.first;
            p.data = it->second;
            p.origin = r.self;
            auto msg = wireMessage(p);
            for (auto const& peer : getAuthenticatedPeers())
                if (r.members.count(peer.first))
                    peer.second->sendMessage(msg);
        }
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
            !p.selected.empty() || !validBatch(p.data) ||
            sha256(p.data) != p.blockHash)
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
