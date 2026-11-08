#include "overlay/ShabdizConsensus.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <random>

using Engine = stellar::ShabdizConsensus<int, std::string>;
using Kind = Engine::Kind;
using Message = Engine::Message;
using Block = Engine::Block;
using Pair = Engine::Pair;

std::string
digest(uint64_t view, std::string const& parent, std::string const& data)
{
    return std::to_string(view) + ":" + std::to_string(parent.size()) + ":" +
           parent + ":" + data;
}
Block
makeBlock(uint64_t view, std::string parent = "g", std::string data = "work")
{
    return Block{view, digest(view, parent, data), parent, data};
}
Message
msg(Kind kind, uint64_t view, Pair p = {}, int origin = 0)
{
    Message m;
    m.kind = kind;
    m.view = view;
    m.value = p;
    m.origin = origin;
    return m;
}
Message
proposal(Block const& b, bool slow = false, std::vector<int> selected = {})
{
    auto m = msg(slow ? Kind::SlowProposal : Kind::FastProposal, b.view,
                 {b.view, b.hash});
    m.block = b;
    m.selected = selected;
    return m;
}
size_t
count(std::vector<Engine::Outgoing> const& out, Kind kind)
{
    size_t n = 0;
    for (auto const& o : out)
        n += o.message.kind == kind;
    return n;
}

struct Fixture
{
    std::map<std::string, Block> blocks;
    Engine e;
    Fixture(int self = 3, bool fastPathOnly = false)
        : e(
              self, {0, 1, 2, 3}, "g",
              [&](auto const& h) -> std::optional<Block> {
                  auto it = blocks.find(h);
                  if (it != blocks.end())
                      return it->second;
                  return {};
              },
              [&](Block const& b) { blocks[b.hash] = b; }, digest,
              [](auto const&) -> std::optional<std::string> { return {}; },
              [](auto const& s) { return !s.empty(); }, false, fastPathOnly)
    {
        e.start();
        e.takeOutgoing();
    }
    Block
    add(uint64_t v, std::string parent = "g", std::string data = "work")
    {
        auto b = makeBlock(v, parent, data);
        blocks[b.hash] = b;
        return b;
    }
    void
    vote(Kind kind, Block const& b, std::initializer_list<int> senders)
    {
        for (int n : senders)
            e.receive(n, msg(kind, b.view, {b.view, b.hash}));
    }
    void
    enter(uint64_t v)
    {
        for (int n : {0, 1, 2})
            e.receive(n, msg(Kind::Wish, v));
        e.takeOutgoing();
    }
    void
    ready(int origin, Pair p, int sender, std::vector<int> deps = {})
    {
        auto r = msg(Kind::Ready, e.view(), p, origin);
        r.dependencies = deps;
        e.receive(sender, r);
    }
};

void
fastPipeline()
{
    Fixture f;
    auto b = f.add(1), c = f.add(2, b.hash);
    f.e.receive(0, proposal(b));
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 1);
    // Per-view guard blocks an equivocating leader's second proposal.
    auto fork = f.add(1, "g", "fork");
    f.e.receive(0, proposal(fork));
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 0);
    f.vote(Kind::Prepare, b, {0, 1}); // local vote plus two peers
    assert(f.e.prepared().view == 1 && f.e.view() == 2 &&
           f.e.committed().view == 0);
    f.e.receive(1, proposal(c));
    assert(count(f.e.takeOutgoing(), Kind::Prepare) ==
           1); // parent is prepared, not committed
    f.vote(Kind::Prepare, c, {0, 1});
    assert(f.e.prepared().view == 2 &&
           f.e.view() == 2); // must wait for previous commit
    f.vote(Kind::Commit, b, {0, 1, 2});
    assert(f.e.committed().view == 1 && f.e.view() == 3);
    f.vote(Kind::Commit, c, {0, 1, 2});
    auto executed = f.e.takeExecuted();
    assert(executed.size() == 2 && executed[0].hash == b.hash &&
           executed[1].hash == c.hash);
    f.vote(Kind::Commit, c, {0, 1, 2});
    assert(f.e.takeExecuted().empty());
    assert(f.e.retainedViews() == 2 && f.e.prepareHistorySize() == 1);
}

void
closeAndPrune()
{
    Fixture f;
    auto a = f.add(1), b = f.add(2, a.hash);
    f.enter(2);
    f.e.receive(1, msg(Kind::Collect, 2));
    assert(f.e.oldestOpenView() == 2);
    f.e.takeOutgoing();
    f.vote(Kind::Prepare, a, {0, 1, 2});
    assert(f.e.prepared().view == 0 &&
           count(f.e.takeOutgoing(), Kind::Commit) == 0);
    f.vote(Kind::Commit, a, {0, 1, 2});
    assert(f.e.committed().view == 1 &&
           f.e.prepared().view == 0); // closed view can commit
    // A pending future-view PREPARE must survive pruning of earlier history.
    Fixture g;
    auto x = g.add(1), y = g.add(2, x.hash);
    g.e.receive(0, proposal(x));
    g.vote(Kind::Prepare, x, {0, 1});
    g.e.receive(1, proposal(y));
    assert(g.e.prepareHistorySize() == 2);
    g.vote(Kind::Commit, x, {0, 1});
    assert(g.e.prepareHistorySize() ==
           2); // committing an old block must not prune y
    g.vote(Kind::Prepare, y, {0, 1});
    assert(g.e.prepareHistorySize() == 1);
}

void
dependencies()
{
    Fixture f;
    auto newer = f.add(1);
    Pair genesis{0, "g"};
    // Conditional READY cannot be justified by local block
    // knowledge/preparation.
    f.vote(Kind::Prepare, newer, {0, 1, 2}); // now view 2
    f.e.takeOutgoing();
    f.ready(0, genesis, 0, {1});
    f.ready(0, genesis, 1, {1});
    assert(count(f.e.takeOutgoing(), Kind::Ready) == 0);
    // Collecting origin 1 with a strictly higher view unblocks the dependency.
    for (int sender : {0, 1, 2})
        f.ready(1, {1, newer.hash}, sender);
    auto out = f.e.takeOutgoing();
    bool forwarded = false;
    for (auto const& o : out)
        if (o.message.kind == Kind::Ready && o.message.origin == 0)
        {
            assert(o.message.dependencies == std::vector<int>{1});
            forwarded = true;
        }
    assert(forwarded);
    // Initial conditional READY and later amplification are independent.
    Fixture g;
    auto hi = g.add(1);
    g.vote(Kind::Prepare, hi, {0, 1, 2});
    g.e.takeOutgoing();
    for (int n : {0, 1, 2})
        g.e.receive(n, msg(Kind::Echo, 2, genesis, 0));
    out = g.e.takeOutgoing();
    assert(count(out, Kind::Ready) == 1);
    for (auto const& o : out)
        if (o.message.kind == Kind::Ready)
            assert(o.message.dependencies == std::vector<int>{3});
    g.ready(0, genesis, 0);
    g.ready(0, genesis, 1);
    out = g.e.takeOutgoing();
    assert(count(out, Kind::Ready) == 1);
    // Echo only once per origin, even if that sender equivocates.
    Fixture h;
    h.e.receive(0, msg(Kind::Send, 1, genesis, 0));
    auto forged = msg(Kind::Send, 1, {100, "fake"}, 0);
    h.e.receive(0, forged);
    assert(count(h.e.takeOutgoing(), Kind::Echo) == 1);
    Fixture i;
    i.e.receive(1, msg(Kind::Send, 1, genesis, 0));
    assert(count(i.e.takeOutgoing(), Kind::Echo) ==
           0); // authenticated origin binding
}

void
slowProposalAndCatchup()
{
    Fixture f;
    f.enter(3);
    auto parent = f.add(1), child = f.add(3, parent.hash);
    auto p = proposal(child, true, {0, 1, 2});
    f.e.receive(2, p);
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 0);
    for (int origin : {0, 1, 2})
        for (int sender : {0, 1, 2})
            f.ready(origin, {1, parent.hash}, sender);
    assert(count(f.e.takeOutgoing(), Kind::Prepare) ==
           1); // proposal resumed after collection
    // Additional higher local entries outside P must not change proposal
    // validation.
    Fixture g;
    g.enter(3);
    auto a = g.add(1), higher = g.add(2, a.hash), b = g.add(3, a.hash);
    for (int origin : {0, 1, 2})
        for (int sender : {0, 1, 2})
            g.ready(origin, {1, a.hash}, sender);
    for (int sender : {0, 1, 2})
        g.ready(3, {2, higher.hash}, sender);
    g.e.takeOutgoing();
    g.e.receive(2, proposal(b, true, {0, 1, 2}));
    assert(count(g.e.takeOutgoing(), Kind::Prepare) == 1);
    // Catch-up counts distinct senders reporting a block OR descendants.
    Fixture h;
    auto x = h.add(4), y = h.add(6, x.hash);
    h.e.receive(0, msg(Kind::Committed, 0, {4, x.hash}));
    h.e.receive(0, msg(Kind::Committed, 0, {4, x.hash}));
    assert(h.e.committed().view == 0);
    h.e.receive(1, msg(Kind::Committed, 0, {6, y.hash}));
    assert(h.e.committed().hash == x.hash);
    h.e.receive(2, msg(Kind::Committed, 0, {6, y.hash}));
    assert(h.e.committed().hash == y.hash);
    assert(h.e.takeExecuted().size() == 2);
    // Higher-view forks are never executed on top of an already committed
    // chain.
    auto fork = h.add(9, "g", "fork");
    for (int n : {0, 1, 2})
        h.e.receive(n, msg(Kind::Committed, 0, {9, fork.hash}));
    assert(h.e.committed().hash == y.hash && h.e.takeExecuted().empty());
}

void
malformedAndMissing()
{
    Fixture f;
    auto b = makeBlock(1);
    auto p = proposal(b);
    p.block->data = "tampered";
    f.e.receive(0, p);
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 0);
    f.e.receive(1, proposal(b));
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 0);
    // Votes before block data cause a request and resume after verified reply.
    f.vote(Kind::Commit, b, {0, 1, 2});
    auto out = f.e.takeOutgoing();
    assert(count(out, Kind::GetBlock) > 0);
    auto response = msg(Kind::BlockData, 0, {1, b.hash});
    response.block = b;
    f.e.receive(0, response);
    assert(f.e.committed().view == 1);
    // One Byzantine wish cannot jump a view. Timers, not proposals, synchronize
    // it.
    Fixture g;
    g.e.receive(0, msg(Kind::Wish, 100));
    assert(g.e.view() == 1);
    g.e.receive(1, msg(Kind::Wish, 100));
    assert(g.e.view() ==
           100); // two wishes trigger self amplification, completing q=3
    assert(g.e.retainedViews() <= 2);
}

void
slowValidationBoundaries()
{
    Fixture f;
    f.enter(3);
    auto parent = f.add(1), higher = f.add(2, parent.hash),
         child = f.add(3, parent.hash);
    // Dependencies may be satisfied outside P, but do not constitute supp(P).
    for (int sender : {0, 1, 2})
        f.ready(3, {2, higher.hash}, sender);
    for (int origin : {0, 1, 2})
        for (int sender : {0, 1, 2})
            f.ready(origin, {1, parent.hash}, sender, {3});
    f.e.takeOutgoing();
    f.e.receive(2, proposal(child, true, {0, 1, 2}));
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 0);
    f.ready(0, {1, parent.hash}, 0);
    f.ready(0, {1, parent.hash}, 0);
    assert(count(f.e.takeOutgoing(), Kind::Prepare) ==
           0); // one distinct supporter
    f.ready(0, {1, parent.hash}, 1);
    assert(count(f.e.takeOutgoing(), Kind::Prepare) == 1); // f+1 unconditional

    Fixture g;
    g.enter(3);
    auto a = g.add(1), conflict = g.add(1, "g", "conflict"),
         b = g.add(3, a.hash);
    for (int origin : {0, 1, 2})
        for (int sender : {0, 1, 2})
            g.ready(origin, {1, origin == 2 ? conflict.hash : a.hash}, sender);
    g.e.takeOutgoing();
    g.e.receive(2, proposal(b, true, {0, 1, 2}));
    assert(count(g.e.takeOutgoing(), Kind::Prepare) == 0); // maximum not unique

    Fixture h;
    h.enter(3);
    auto p = h.add(1), c = h.add(2, p.hash);
    // Still serve a slow proposal and its collection in the previous view.
    h.e.receive(1, proposal(c, true, {0, 1, 2}));
    h.e.takeOutgoing();
    for (int origin : {0, 1, 2})
        for (int sender : {0, 1, 2})
            h.e.receive(sender, msg(Kind::Ready, 2, {1, p.hash}, origin));
    assert(count(h.e.takeOutgoing(), Kind::Prepare) == 1);
    h.enter(4);
    h.e.receive(1, proposal(c, true, {0, 1, 2}));
    assert(count(h.e.takeOutgoing(), Kind::Prepare) == 0); // pruned view
}

void
network(int n, int absent, unsigned seed, bool forceSlow, bool loseOldVotes,
        bool routedWork = false, bool fastPathOnly = false)
{
    struct Delivery
    {
        int from, to;
        Message m;
        bool work = false;
    };
    std::vector<int> members;
    for (int i = 0; i < n; ++i)
        members.push_back(i);
    std::vector<std::map<std::string, Block>> stores(n);
    std::vector<std::unique_ptr<Engine>> engines(n);
    std::vector<std::vector<Block>> executed(n);
    std::vector<Delivery> pending;
    std::vector<std::set<uint64_t>> availableWork(n);
    uint64_t forwardedView = 0;
    std::mt19937 rng(seed);
    const uint64_t limit = 8;
    for (int i = absent; i < n; ++i)
    {
        engines[i] = std::make_unique<Engine>(
            i, members, "g",
            [&, i](auto const& h) -> std::optional<Block> {
                auto it = stores[i].find(h);
                if (it != stores[i].end())
                    return it->second;
                return {};
            },
            [&, i](auto const& b) { stores[i][b.hash] = b; }, digest,
            [&, i](auto const&) -> std::optional<std::string> {
                if (engines[i]->committed().view >= limit ||
                    (routedWork && !availableWork[i].count(engines[i]->view())))
                    return {};
                return "data" + std::to_string(engines[i]->view());
            },
            [](auto const& d) { return !d.empty(); }, false, fastPathOnly);
        if (forceSlow)
            engines[i]->forceSlowViews(100);
    }
    auto drain = [&](int id) {
        // Only the ingress (last member) owns requests. Supply work to the
        // current leader, not every replica. Retry routing after view changes,
        // including when the old leader is one of the absent members.
        if (routedWork && id == n - 1 && forwardedView != engines[id]->view())
        {
            forwardedView = engines[id]->view();
            int leader = engines[id]->leader(forwardedView);
            if (leader == id)
            {
                availableWork[id].insert(forwardedView);
                engines[id]->workAvailable();
            }
            else if (leader >= absent)
                pending.push_back(
                    {id, leader, msg(Kind::Prepare, forwardedView), true});
        }
        auto blocks = engines[id]->takeExecuted();
        executed[id].insert(executed[id].end(), blocks.begin(), blocks.end());
        for (auto const& o : engines[id]->takeOutgoing())
        {
            if (fastPathOnly)
                assert(o.message.kind == Kind::FastProposal ||
                       o.message.kind == Kind::Prepare ||
                       o.message.kind == Kind::Commit ||
                       o.message.kind == Kind::GetBlock ||
                       o.message.kind == Kind::BlockData);
            for (int j = absent; j < n; ++j)
                if (j != id && (!o.to || *o.to == j))
                    pending.push_back({id, j, o.message});
        }
    };
    for (int i = absent; i < n; ++i)
    {
        engines[i]->start();
        drain(i);
    }
    size_t delivered = 0;
    for (int round = 0; round < 40; ++round)
    {
        while (!pending.empty())
        {
            assert(++delivered < 3000000);
            size_t index = rng() % pending.size();
            auto d = std::move(pending[index]);
            pending[index] = std::move(pending.back());
            pending.pop_back();
            if (d.work)
            {
                availableWork[d.to].insert(d.m.view);
                engines[d.to]->workAvailable();
                drain(d.to);
                continue;
            }
            // One late replica misses old phase votes but can catch up via
            // reports.
            if (loseOldVotes && d.to == n - 1 && d.m.view <= 2 &&
                (d.m.kind == Kind::Prepare || d.m.kind == Kind::Commit))
                continue;
            engines[d.to]->receive(d.from, d.m);
            if (rng() % 7 == 0)
                engines[d.to]->receive(d.from, d.m);
            drain(d.to);
        }
        bool finished = true;
        for (int i = absent; i < n; ++i)
            finished &= engines[i]->committed().view >= limit;
        if (finished)
            break;
        for (int i = absent; i < n; ++i)
        {
            engines[i]->timeout();
            engines[i]->retransmit();
            drain(i);
        }
    }
    for (int i = absent; i < n; ++i)
    {
        if (engines[i]->committed().view < limit)
            std::cerr << "stalled n=" << n << " absent=" << absent
                      << " seed=" << seed << " slow=" << forceSlow
                      << " lose=" << loseOldVotes << " node=" << i
                      << " routed=" << routedWork
                      << " view=" << engines[i]->view()
                      << " prepared=" << engines[i]->prepared().view
                      << " committed=" << engines[i]->committed().view << "\n";
        assert(engines[i]->committed().view >= limit);
        assert(engines[i]->retainedViews() <= 2);
        assert(engines[i]->prepareHistorySize() <= 3);
        assert(executed[i].size() == executed[absent].size());
        for (size_t j = 0; j < executed[i].size(); ++j)
        {
            assert(executed[i][j].hash == executed[absent][j].hash);
            if (j)
                assert(executed[i][j].parent == executed[i][j - 1].hash);
        }
    }
}

void
fastPathOnlyBoundaries()
{
    Fixture f(3, true);
    // An absent leader stalls. Timeouts and recovery messages cannot silently
    // activate recovery or manufacture normal-path progress.
    f.e.forceSlowViews(20);
    f.e.timeout();
    f.e.retransmit();
    for (int n : {0, 1, 2})
    {
        f.e.receive(n, msg(Kind::Wish, 5));
        f.e.receive(n, msg(Kind::Committed, 0, {5, "missing"}));
    }
    assert(f.e.view() == 1 && f.e.committed().view == 0);
    assert(f.e.takeOutgoing().empty());
    auto b = makeBlock(1);
    f.e.receive(0, proposal(b, true, {0, 1, 2}));
    assert(f.e.takeOutgoing().empty());
    assert(f.e.takeExecuted().empty());

    // The same fast proposal validation and commit quorum still apply.
    auto bad = b;
    bad.data += "tampered";
    f.e.receive(0, proposal(bad));
    assert(f.e.takeOutgoing().empty());
    f.e.receive(0, proposal(b));
    f.vote(Kind::Prepare, b, {0, 1}); // plus own PREPARE
    f.vote(Kind::Commit, b, {0});    // plus own COMMIT: below quorum
    assert(f.e.takeExecuted().empty());
    f.vote(Kind::Commit, b, {1});
    assert(f.e.committed().hash == b.hash);
    assert(f.e.takeExecuted().size() == 1);
    auto outgoing = f.e.takeOutgoing();
    assert(count(outgoing, Kind::Committed) == 0);
    assert(count(outgoing, Kind::Collect) == 0);
    f.e.retransmit();
    assert(f.e.takeOutgoing().empty());
}

int
main()
{
    fastPathOnlyBoundaries();
    fastPipeline();
    closeAndPrune();
    dependencies();
    slowProposalAndCatchup();
    malformedAndMissing();
    slowValidationBoundaries();
    int schedules = 0;
    for (int n : {4, 7, 8, 16})
        for (unsigned seed = 0; seed < 12; ++seed)
            for (bool slow : {false, true})
                for (int absent : {0, (n - 1) / 3})
                {
                    network(n, absent, seed, slow, seed % 2 == 0);
                    ++schedules;
                }
    network(48, 0, 1, false, false);
    network(48, 15, 2, true, false);
    schedules += 2;
    for (int n : {4, 7, 8, 16})
        for (unsigned seed = 0; seed < 4; ++seed)
            for (bool slow : {false, true})
                for (int absent : {0, (n - 1) / 3})
                {
                    network(n, absent, seed, slow, seed % 2 == 0, true);
                    ++schedules;
                }
    network(48, 0, 1, false, false, true);
    network(48, 15, 2, true, false, true);
    schedules += 2;
    for (int n : {4, 7, 8, 16, 48})
        for (unsigned seed = 0; seed < 4; ++seed)
            for (bool routed : {false, true})
            {
                // Even a forced-collection request is inert in fast-only mode.
                network(n, 0, seed, true, false, routed, true);
                ++schedules;
            }
    std::cout << "Shabdiz rules and " << schedules
              << " randomized network schedules passed\n";
}
