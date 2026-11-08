#include "overlay/ForgetITConsensus.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <random>

using Core = stellar::ForgetITConsensus<int>;
using Value = Core::Value;
using Message = Core::Message;
struct Delivery
{
    int from, to;
    Message msg;
};
struct Network
{
    std::vector<std::unique_ptr<Core>> nodes;
    std::vector<Delivery> queue;
    std::vector<Message> sent;
    uint64_t now = 0;
    std::mt19937 rng;
    Network(int n, unsigned seed, int silent = -1) : nodes(n), rng(seed)
    {
        std::vector<int> members;
        for (int i = 0; i < n; ++i)
            members.push_back(i);
        for (int i = 0; i < n; ++i)
            if (i != silent)
                nodes[i] = std::make_unique<Core>(
                    i, members, "input-" + std::to_string(i), now, 10);
        collect();
    }
    void
    collect()
    {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i])
                for (auto const& msg : nodes[i]->takeOutgoing())
                {
                    sent.push_back(msg);
                    for (size_t j = 0; j < nodes.size(); ++j)
                        if (j != i && nodes[j])
                            queue.push_back({int(i), int(j), msg});
                }
    }
    void
    drain(bool discardVote2 = false)
    {
        size_t count = 0;
        while (!queue.empty())
        {
            assert(++count < 100000);
            auto k = rng() % queue.size();
            auto d = queue[k];
            queue[k] = queue.back();
            queue.pop_back();
            if (discardVote2 && d.msg.kind == Core::Vote2)
                continue;
            nodes[d.to]->receive(d.from, d.msg, now);
            if (rng() % 4 == 0)
                nodes[d.to]->receive(d.from, d.msg, now);
            collect();
        }
    }
    void
    tick(uint64_t t)
    {
        now = t;
        for (auto& n : nodes)
            if (n)
                n->tick(now);
        collect();
    }
    void
    agree(std::string const& value)
    {
        for (auto const& n : nodes)
            if (n)
            {
                assert(n->state().decision == value);
                assert(n->state().views.size() <= 3);
            }
    }
};
void
healthy(int n, unsigned seed)
{
    Network net(n, seed);
    net.drain();
    net.agree("input-0");
    for (auto const& node : net.nodes)
        assert(node->state().view == 1);
    for (auto const& msg : net.sent)
        assert(msg.kind <= Core::Vote2);
}
void
missingLeader(int n, unsigned seed)
{
    Network net(n, seed, 0);
    net.drain();
    net.tick(29);
    net.drain();
    assert(net.sent.empty());
    net.tick(30);
    net.drain();
    for (auto const& msg : net.sent)
        assert(msg.kind == Core::Vote2 && msg.value.tag == Value::Bottom);
    net.tick(40);
    net.drain();
    net.agree("input-1");
    for (auto const& node : net.nodes)
        if (node)
            assert(node->state().view == 2);
}
void
preserveLocks(int n, unsigned seed)
{
    Network net(n, seed);
    net.drain(true); // all lock input-0; no remote vote-2 reaches anyone
    for (auto const& node : net.nodes)
        assert(!node->state().decision);
    // Retransmit alive messages after delay, then move views. Initial values
    // differ at every new leader, so agreement depends on the retained guard.
    for (size_t i = 0; i < net.nodes.size(); ++i)
        for (auto const& m : net.nodes[i]->alive())
            for (size_t j = 0; j < net.nodes.size(); ++j)
                if (i != j)
                    net.queue.push_back({int(i), int(j), m});
    net.drain();
    net.agree("input-0");
    for (uint64_t round = 1; round <= 12; ++round)
    {
        net.tick(round * 40);
        net.drain();
        net.agree("input-0");
        for (auto const& msg : net.sent)
            if (msg.value.tag == Value::Data)
                assert(msg.value.data == "input-0");
        for (auto const& node : net.nodes)
        {
            assert(node->state().view == round + 1);
            assert(node->leader(node->state().view) == int(round % n));
            for (auto const& mem : node->state().views)
                for (auto const& echoes : mem.second.echoes)
                    assert(echoes.size() <= 2);
        }
    }
}
void
recovery(unsigned seed)
{
    Network net(4, seed);
    net.drain(true);
    for (auto& node : net.nodes)
    {
        auto saved = node->state();
        node->recover(saved, net.now);
        for (auto const& m : node->alive())
            assert(m.kind != Core::Prepare);
    }
    net.collect();
    net.drain();
    net.tick(40);
    net.drain();
    net.agree("input-0");
}
void
targeted()
{
    std::vector<int> members{0, 1, 2, 3};
    auto data = Value::value("A"), other = Value::value("B"),
         safe = Value::safe();
    Core c(3, members, "own", 0, 10);
    c.receive(1, {Core::Prepare, 1, data, {}}, 0);
    assert(c.takeOutgoing().empty());
    c.receive(0, {Core::Prepare, 1, data, {}}, 0);
    assert(c.takeOutgoing().size() == 1);
    // Non-bottom vote-2 cannot count until f+1 vote-1 have been received.
    for (int from : {0, 1, 2})
        c.receive(from, {Core::Vote2, 1, data, safe}, 0);
    assert(!c.state().decision);
    c.receive(0, {Core::Vote1, 1, data, {}}, 0);
    assert(c.state().decision == "A"); // own vote-1 + sender 0
    // Duplicate senders do not form a certificate. SAFE can support a data
    // certificate, but a data echo cannot support a SAFE certificate.
    Core passive(3, members, "own", 0, 10);
    for (int k = 0; k < 5; ++k)
        passive.receive(0, {Core::Echo1, 8, data, {}}, 0);
    assert(passive.state().view == 1);
    passive.receive(1, {Core::Echo1, 8, safe, {}}, 0);
    assert(passive.state().view == 8 && passive.state().views.at(8).passive);
    assert(passive.state().views.at(8).guard == data);
    // At most one specific echo followed by SAFE; never two different values.
    Core upgrade(3, members, "own", 0, 10);
    for (int from : {0, 1, 2})
        upgrade.receive(from, {Core::Vote2, 1, {}, data}, 0);
    upgrade.tick(40);
    auto first = upgrade.takeOutgoing();
    bool echoedA = false;
    for (auto const& m : first)
        if (m.kind == Core::Echo1 && m.value == data)
            echoedA = true;
    assert(echoedA);
    // Quorum echo-1 for B, then quorum for SAFE upgrades echo-2 safely.
    for (int from : {0, 1, 2})
        upgrade.receive(from, {Core::Echo1, 1, other, {}}, 40);
    for (int from : {0, 1, 2})
        upgrade.receive(from, {Core::Echo1, 1, safe, {}}, 40);
    auto const& e = upgrade.state().views.at(1).echoes[1];
    assert(e.size() == 2 && e[0] == other && e[1] == safe);
    // First ACCEPTED vote-2 matters, not an earlier unsupported lock.
    Core accepted(3, members, "own", 0, 10);
    for (int from : {0, 1, 2})
    {
        accepted.receive(from, {Core::Vote2, 1, data, safe}, 0);
        accepted.receive(from, {Core::Vote2, 1, {}, safe}, 0);
    }
    accepted.tick(40);
    assert(accepted.state().views.at(1).alert == false);
    bool rejected = false;
    try
    {
        Core invalid(0, {0, 1, 2, 3, 4, 5, 6, 7}, "A", 0, 10);
    }
    catch (std::invalid_argument const&)
    {
        rejected = true;
    }
    assert(rejected);
}
int
main()
{
    targeted();
    for (unsigned seed = 0; seed < 30; ++seed)
    {
        for (int n : {4, 7, 16})
        {
            healthy(n, seed);
            missingLeader(n, seed);
            preserveLocks(n, seed);
        }
        recovery(seed);
    }
    std::cout << "Forget-IT: 300 schedules passed (healthy path, silent "
                 "leader, 12 native view changes, locked-value preservation, "
                 "recovery), plus targeted guard/certificate tests.\n";
}
