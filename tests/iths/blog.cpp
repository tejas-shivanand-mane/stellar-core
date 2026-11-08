#include "overlay/ITHSBlogConsensus.h"
#include <cassert>
#include <iostream>
#include <memory>
#include <random>

using Core = stellar::ITHSBlogConsensus<int>;
using Message = Core::Message;
struct Delivery
{
    int from, to;
    Message msg;
};
struct Network
{
    std::vector<std::unique_ptr<Core>> nodes;
    std::vector<Delivery> events;
    std::vector<Message> sent;
    uint64_t now = 1000;
    std::mt19937 rng;
    Network(int n, unsigned seed, int silent = -1) : nodes(n), rng(seed)
    {
        std::vector<int> members;
        for (int i = 0; i < n; ++i)
            members.push_back(i);
        for (int i = 0; i < n; ++i)
            if (i != silent)
                nodes[i] = std::make_unique<Core>(
                    i, members, i == 0 ? "A" : "B", 1000, now, 10);
        collect();
    }
    void
    collect()
    {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i])
                for (auto const& out : nodes[i]->takeOutgoing())
                {
                    sent.push_back(out.message);
                    assert((out.message.kind == Core::Suggest) ==
                           out.recipient.has_value());
                    if (out.message.kind == Core::Commit)
                        assert(out.message.view == 0);
                    for (size_t j = 0; j < nodes.size(); ++j)
                        if (j != i && nodes[j] &&
                            (!out.recipient || *out.recipient == int(j)))
                            events.push_back({int(i), int(j), out.message});
                }
    }
    void
    drain(bool dropLocks = false)
    {
        size_t count = 0;
        while (!events.empty())
        {
            assert(++count < 100000);
            auto k = rng() % events.size();
            auto e = events[k];
            events[k] = events.back();
            events.pop_back();
            if (dropLocks && e.msg.kind == Core::Lock)
                continue;
            nodes[e.to]->receive(e.from, e.msg, now);
            if (rng() % 3 == 0)
                nodes[e.to]->receive(e.from, e.msg, now);
            collect();
        }
    }
    void
    tick(uint64_t t)
    {
        now = t;
        for (auto& node : nodes)
            if (node)
                node->tick(now);
        collect();
    }
    void
    decided(std::string const& value)
    {
        for (auto const& node : nodes)
            if (node)
                assert(node->decision() == value);
    }
};
void
healthy(int n, unsigned seed)
{
    Network net(n, seed);
    net.drain();
    net.tick(1019);
    net.drain();
    for (auto const& m : net.sent)
        assert(m.kind == Core::Suggest);
    net.tick(1020);
    net.drain();
    net.decided("A");
    net.tick(1500);
    for (auto const& node : net.nodes)
        assert(node->view() == 1);
}
void
rotate(int n, unsigned seed)
{
    Network net(n, seed, 0);
    net.drain();
    net.tick(1020);
    net.drain();
    for (auto const& node : net.nodes)
        if (node)
            assert(!node->decision());
    net.tick(1099);
    net.drain();
    for (auto const& node : net.nodes)
        if (node)
            assert(node->view() == 1);
    net.tick(1100);
    net.drain();
    for (auto const& node : net.nodes)
        if (node)
        {
            assert(node->view() == 2);
            assert(node->leader(2) == 1);
        }
    net.tick(1119);
    net.drain();
    for (auto const& node : net.nodes)
        if (node)
            assert(!node->decision());
    net.tick(1120);
    net.drain();
    net.decided("B");
}
void
preserve(int n, unsigned seed)
{
    Network net(n, seed);
    net.drain();
    net.tick(1020);
    net.drain(true);
    for (auto const& node : net.nodes)
    {
        assert(!node->decision());
        assert(node->lock().view == 1 && node->lock().value == "A");
    }
    net.tick(1100);
    net.drain();
    net.tick(1120);
    net.drain();
    net.decided("A");
    for (auto const& m : net.sent)
        if (m.kind == Core::Propose && m.view == 2)
            assert(m.lockView == 1 && m.value == "A");
}
void
evidence()
{
    std::vector<int> members{0, 1, 2, 3};
    Core leader(1, members, "B", 0, 100, 10); // leader of view 2
    leader.takeOutgoing();
    for (int from : {0, 2, 3})
        leader.receive(from, {Core::Suggest, 2, 1, "A"}, 120);
    assert(leader.takeOutgoing().empty()); // unproven suggestions do not count
    for (int from : {0, 2, 3})
        leader.receive(from, {Core::Echo, 1, 0, "A"}, 120);
    assert(leader.takeOutgoing().empty()); // ECHO evidence is insufficient
    for (int from : {0, 2, 3})
        leader.receive(from, {Core::Accept, 1, 0, "A"}, 120);
    auto messages = leader.takeOutgoing();
    bool proposed = false;
    for (auto const& o : messages)
        if (o.message.kind == Core::Propose)
        {
            proposed = true;
            assert(o.message.lockView == 1 && o.message.value == "A");
        }
    assert(proposed &&
           leader.lock().view == 0); // old ACCEPT only validates evidence

    Core follower(3, members, "B", 0, 100, 10);
    follower.takeOutgoing();
    follower.receive(0, {Core::Propose, 2, 0, "B"}, 120);
    assert(follower.takeOutgoing().empty()); // wrong leader
    follower.receive(1, {Core::Propose, 2, 1, "A"}, 120);
    assert(follower.takeOutgoing().empty());
    for (int from : {0, 1, 2})
        follower.receive(from, {Core::Echo, 1, 0, "A"}, 120);
    assert(follower.takeOutgoing().empty());
    for (int from : {0, 1, 2})
        follower.receive(from, {Core::Accept, 1, 0, "A"}, 120);
    bool echoed = false;
    for (auto const& o : follower.takeOutgoing())
        if (o.message.kind == Core::Echo)
        {
            echoed = true;
            assert(o.message.view == 2 && o.message.value == "A");
        }
    assert(echoed);

    // A real lock forbids an unproved lower-view proposal in the next view.
    Core locked(3, members, "A", 0, 0, 10);
    locked.receive(0, {Core::Propose, 1, 0, "A"}, 20);
    for (int from : {0, 1, 2})
        locked.receive(from, {Core::Echo, 1, 0, "A"}, 20);
    for (int from : {0, 1, 2})
        locked.receive(from, {Core::Accept, 1, 0, "A"}, 20);
    assert(locked.lock().view == 1 && !locked.decision());
    locked.tick(100);
    locked.takeOutgoing();
    locked.receive(1, {Core::Propose, 2, 0, "B"}, 120);
    assert(locked.takeOutgoing().empty());
    locked.tick(200);
    locked.takeOutgoing();
    locked.receive(2, {Core::Propose, 3, 1, "A"}, 220);
    auto safe = locked.takeOutgoing();
    assert(safe.size() == 1 && safe[0].message.kind == Core::Echo &&
           safe[0].message.value == "A");

    // COMMIT is independent of protocol views, boosted at f+1, decided at n-f.
    Core commits(3, members, "B", 0, 500, 10);
    commits.takeOutgoing();
    for (int i = 0; i < 5; ++i)
        commits.receive(0, {Core::Commit, 0, 0, "A"}, 500);
    assert(!commits.decision() && commits.takeOutgoing().empty());
    commits.receive(1, {Core::Commit, 5, 0, "A"}, 500);
    assert(!commits.decision());
    commits.receive(1, {Core::Commit, 0, 0, "A"}, 500);
    assert(commits.decision() == "A");
    auto out = commits.takeOutgoing();
    assert(out.size() == 1 && out[0].message.kind == Core::Commit &&
           out[0].message.view == 0);

    // One common epoch, not ten Delta after each replica happens to receive
    // input.
    Core early(0, members, "A", 1000, 1000, 10),
        late(1, members, "A", 1000, 1008, 10);
    assert(early.nextDeadline(1008) == 1020 && late.nextDeadline(1008) == 1100);
    assert(early.nextDeadline(1100) == 1100 && late.nextDeadline(1100) == 1100);
    early.tick(1100);
    late.tick(1100);
    assert(early.view() == 2 && late.view() == 2);
    assert(late.nextDeadline(1100) == 1120);
}
int
main()
{
    evidence();
    for (unsigned seed = 0; seed < 25; ++seed)
        for (int n : {4, 7, 8, 16})
        {
            healthy(n, seed);
            rotate(n, seed);
            preserve(n, seed);
        }
    std::cout << "IT-HS blog: 300 schedules passed; 2-Delta wait, 10-Delta "
                 "rotation, silent leader, highest-lock preservation, ACCEPT "
                 "evidence, background boosting and view-independent COMMIT.\n";
}
