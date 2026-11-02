#include "overlay/TetraBFTNormal.h"
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
    std::cout << "TetraBFT normal-path tests passed (150 network schedules)\n";
}
