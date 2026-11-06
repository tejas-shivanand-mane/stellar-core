#include "overlay/NormalPathConsensus.h"
#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <random>
#include <string>

using State = stellar::TetraBFTNormal<int, std::string>;

void network(int n, int absent, unsigned seed)
{
    struct Message { int sender, receiver; unsigned phase; };
    const int active = n - absent;
    const size_t q = n - (n - 1) / 3;
    std::vector<State> states(active);
    std::vector<bool> decided(active);
    std::vector<Message> messages;
    std::mt19937 rng(seed);
    auto advance = [&](int id) {
        for (auto phase : states[id].advance(id, q))
            for (int other = 0; other < active; ++other)
                if (id != other)
                    messages.push_back({id, other, phase});
        if (states[id].decide(q))
            decided[id] = true;
    };
    for (int id = 0; id < active; ++id)
    {
        states[id].propose("batch");
        advance(id);
    }
    while (!messages.empty())
    {
        size_t i = rng() % messages.size();
        auto m = messages[i];
        messages[i] = messages.back();
        messages.pop_back();
        states[m.receiver].vote(m.phase, m.sender, "batch");
        states[m.receiver].vote(m.phase, m.sender, "batch"); // duplicate
        advance(m.receiver);
    }
    for (bool d : decided)
        assert(d == (active >= static_cast<int>(q)));
}

using Forget = stellar::ForgetITNormal<int, std::string>;
using Simple = stellar::SimpleITNormal<int, std::string>;

// Include proposals in the shuffled queue, allowing votes to arrive first.
void newNetwork(int n, int absent, unsigned seed, int mode)
{
    struct Message { int sender, receiver; unsigned phase; };
    int active = n - absent;
    size_t f = (n - 1) / 3, q = n - f;
    std::vector<Forget> forget(active);
    std::vector<Simple> simple(active);
    std::vector<bool> decided(active);
    std::vector<Message> messages;
    std::mt19937 rng(seed);
    for (int id = 0; id < active; ++id)
        messages.push_back({0, id, 0});
    while (!messages.empty())
    {
        size_t i = rng() % messages.size();
        auto m = messages[i];
        messages[i] = messages.back();
        messages.pop_back();
        auto id = m.receiver;
        if (m.phase == 0)
        {
            forget[id].propose("batch");
            simple[id].propose("batch");
        }
        else
        {
            for (int duplicate = 0; duplicate < 2; ++duplicate)
                if (mode == 0) forget[id].vote(m.phase, m.sender, "batch");
                else simple[id].vote(m.phase, m.sender, "batch");
        }
        auto outgoing = mode == 0 ? forget[id].advance(id, q) :
            simple[id].advance(id, 0, n, mode == 2, true);
        for (auto phase : outgoing)
            for (int other = 0; other < active; ++other)
                if (id != other) messages.push_back({id, other, phase});
        bool decision = mode == 0 ? forget[id].decide(q, f + 1) :
            simple[id].decide(q, 0, true);
        if (decision)
        {
            assert(!decided[id]);
            decided[id] = true;
        }
    }
    for (bool d : decided) assert(d == (active >= static_cast<int>(q)));
}

void newThresholds()
{
    Forget forget;
    forget.propose("A");
    forget.advance(0, 3);
    for (int i = 1; i <= 3; ++i) forget.vote(2, i, "A");
    assert(!forget.decide(3, 2)); // vote-2 quorum alone is not acceptable
    forget.vote(1, 1, "B");
    forget.vote(1, 1, "A"); // equivocation cannot replace first vote
    assert(!forget.decide(3, 2));
    forget.vote(1, 2, "A");
    assert(forget.decide(3, 2));
    assert(!forget.decide(3, 2));

    auto contains = [](std::vector<unsigned> const& v, unsigned p) {
        return std::find(v.begin(), v.end(), p) != v.end();
    };
    for (int n : {4, 7, 8, 16, 32, 48})
    {
        size_t f = (n - 1) / 3, q = n - f;
        size_t threshold = (n + 2 * f - 1) / 2;
        Simple fast;
        assert(fast.advance(0, 0, n, true, true).empty());
        assert(!fast.decide(q, 0, true));
        fast.propose("A");
        assert(!fast.propose("B"));
        fast.advance(0, 0, n, true, true); // leader echo does not count
        for (size_t i = 1; i < threshold; ++i)
        {
            fast.vote(Simple::Echo, i, "A");
            fast.vote(Simple::Echo, i, "A");
            fast.vote(Simple::Echo, i, "B");
            assert(!contains(fast.advance(0, 0, n, true, true), Simple::Commit));
        }
        fast.vote(Simple::Echo, threshold, "A");
        assert(!contains(fast.advance(0, 0, n, true, false), Simple::Commit));
        assert(contains(fast.advance(0, 0, n, true, true), Simple::Commit));
        for (size_t i = 1; i + 1 < q; ++i) fast.vote(Simple::Commit, i, "A");
        assert(!fast.decide(q, 0, true));
        fast.vote(Simple::Commit, q - 1, "A");
        assert(!fast.decide(q, 0, false));
        assert(fast.decide(q, 0, true));
        assert(!fast.decide(q, 0, true));
        // Optimistic decision must not stop the RBC auxiliary path.
        for (size_t i = 1; i <= (static_cast<size_t>(n) + f) / 2; ++i)
            fast.vote(Simple::Ack, i, "A");
        assert(contains(fast.advance(0, 0, n, true, true), Simple::Ready));

        Simple bracha;
        bracha.propose("A");
        bracha.advance(0, 0, n, false, true);
        for (size_t i = 1; i + 1 < q; ++i) bracha.vote(Simple::Echo, i, "A");
        assert(!contains(bracha.advance(0, 0, n, false, true), Simple::Ready));
        bracha.vote(Simple::Echo, q - 1, "A");
        auto outgoing = bracha.advance(0, 0, n, false, true);
        assert(contains(outgoing, Simple::Ready));
        assert(!contains(outgoing, Simple::Commit));
        for (size_t i = 1; i + 1 < q; ++i) bracha.vote(Simple::Ready, i, "A");
        assert(!contains(bracha.advance(0, 0, n, false, true), Simple::Commit));
        bracha.vote(Simple::Ready, q - 1, "A");
        assert(contains(bracha.advance(0, 0, n, false, true), Simple::Commit));

        Simple relay;
        relay.propose("A");
        for (size_t i = 1; i <= f; ++i) relay.vote(Simple::Ready, i, "A");
        assert(!contains(relay.advance(0, 0, n, false, true), Simple::Ready));
        relay.vote(Simple::Ready, f + 1, "A");
        assert(contains(relay.advance(0, 0, n, false, true), Simple::Ready));
    }
}

int main()
{
    State s;
    assert(s.advance(0, 3).empty());
    assert(!s.decide(3));
    assert(s.propose("A"));
    assert(!s.propose("B"));
    assert(s.advance(0, 3) == std::vector<unsigned>{1});
    for (unsigned phase = 1; phase <= 4; ++phase)
    {
        for (int k = 0; k < 10; ++k) s.vote(phase, 1, "A");
        s.vote(phase, 1, "B"); // equivocation cannot add another vote
        assert(s.advance(0, 3).empty());
        assert(!s.decide(3));
        s.vote(phase, 2, "A");
        if (phase < 4)
            assert(s.advance(0, 3) == std::vector<unsigned>{phase + 1});
    }
    assert(s.decide(3));
    assert(!s.decide(3));

    State buffered;
    for (unsigned p = 1; p <= 4; ++p)
        for (int i = 1; i <= 3; ++i) buffered.vote(p, i, "A");
    assert(!buffered.decide(3));
    buffered.propose("A");
    assert(buffered.advance(0, 3) == (std::vector<unsigned>{1,2,3,4}));
    assert(buffered.decide(3));
    for (int n : {4, 8, 16, 32, 48})
        for (unsigned seed = 0; seed < 10; ++seed)
        {
            network(n, 0, seed);
            network(n, (n - 1) / 3, seed);
            network(n, (n - 1) / 3 + 1, seed);
        }
    newThresholds();
    for (int mode = 0; mode < 3; ++mode)
        for (int n : {4, 7, 8, 16, 32, 48})
            for (unsigned seed = 0; seed < 10; ++seed)
                for (int absent : {0, (n - 1) / 3, (n - 1) / 3 + 1})
                    newNetwork(n, absent, seed, mode);
    std::cout << "Normal-path tests passed (690 network schedules)\n";
}
