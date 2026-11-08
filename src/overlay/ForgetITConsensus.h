#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace stellar
{
// Forget-IT, August 2026, Algorithm 1. One Byzantine-agreement instance.
// Values are compared directly: hashes are not information-theoretic evidence.
// The caller supplies authenticated membership, clocks in milliseconds, and
// reliable delivery (or retransmission of alive()). Durable storage is a caller
// responsibility; Persistent contains exactly the retained protocol state.
template <class Node> class ForgetITConsensus
{
  public:
    enum Kind : unsigned
    {
        Prepare = 1,
        Vote1,
        Vote2,
        Echo1,
        Echo2,
        Echo3
    };
    struct Value
    {
        enum Tag : unsigned
        {
            Bottom = 0,
            Safe = 1,
            Data = 2
        };
        unsigned tag = Bottom;
        std::string data;
        static Value
        safe()
        {
            return {Safe, {}};
        }
        static Value
        value(std::string const& s)
        {
            return {Data, s};
        }
        bool
        operator==(Value const& b) const
        {
            return tag == b.tag && data == b.data;
        }
        bool
        operator!=(Value const& b) const
        {
            return !(*this == b);
        }
        bool
        operator<(Value const& b) const
        {
            return std::tie(tag, data) < std::tie(b.tag, b.data);
        }
        bool
        valid() const
        {
            return tag <= Data && (tag == Data || data.empty());
        }
    };
    struct Message
    {
        unsigned kind;
        uint64_t view;
        Value value;
        Value guard;
        bool
        operator==(Message const& b) const
        {
            return kind == b.kind && view == b.view && value == b.value &&
                   guard == b.guard;
        }
    };
    struct Memory
    {
        Value guard, proposal, lock, potential;
        std::optional<bool> alert;
        bool changing = false;
        bool passive = false;
        std::optional<Message> vote1, vote2;
        std::array<std::vector<Value>, 3> echoes;
    };
    struct Persistent
    {
        std::string input;
        uint64_t view = 1;
        std::map<uint64_t, Memory> views;
        std::optional<std::string> decision;
    };

  private:
    struct Received
    {
        std::optional<Value> proposal;
        std::map<Node, Value> vote1;
        std::map<Node, std::vector<Message>> pending2;
        std::map<Node, Message> vote2;
        std::array<std::map<Node, std::set<Value>>, 3> echoes;
    };
    Node mSelf;
    std::vector<Node> mLeaders;
    size_t mF, mQ;
    uint64_t mDelta, mEntered = 0;
    Persistent mState;
    std::map<uint64_t, Received> mReceived;
    std::vector<Message> mOutgoing;
    bool mChanged = false;
    std::set<uint64_t> mPrepareAlive;

    size_t
    count(std::map<Node, Value> const& votes, Value const& value) const
    {
        size_t n = 0;
        for (auto const& v : votes)
            n += v.second == value;
        return n;
    }
    void
    record(Node const& sender, Message const& msg)
    {
        auto& r = mReceived[msg.view];
        if (msg.kind == Prepare)
        {
            if (!r.proposal)
                r.proposal = msg.value;
        }
        else if (msg.kind == Vote1)
            r.vote1.emplace(sender, msg.value);
        else if (msg.kind == Vote2)
        {
            auto& pending = r.pending2[sender];
            if (std::find(pending.begin(), pending.end(), msg) == pending.end())
                pending.push_back(msg);
        }
        else
            r.echoes[msg.kind - Echo1][sender].insert(msg.value);
    }
    void
    send(Message const& msg)
    {
        mOutgoing.push_back(msg);
        record(mSelf, msg);
        mChanged = true;
    }
    void
    voteTwo(uint64_t view, Value const& lock, Value const& guard)
    {
        auto& s = mState.views[view];
        if (!s.vote2)
        {
            s.vote2 = Message{Vote2, view, lock, guard};
            send(*s.vote2);
        }
    }
    void
    ensureChange(uint64_t view, Value const& guard)
    {
        if (!view)
            return;
        auto& s = mState.views[view];
        if (!s.changing)
        {
            s.changing = true;
            mChanged = true;
        }
        voteTwo(view, {}, guard); // lines 98-100: bottom, not a fabricated lock
    }
    void
    echo(unsigned phase, uint64_t view, Value value)
    {
        auto& sent = mState.views[view].echoes[phase];
        if (!sent.empty())
        {
            if (sent.size() == 2 || sent[0].tag == Value::Safe ||
                sent[0] == value)
                return;
            value = Value::safe();
        }
        sent.push_back(value);
        send({Echo1 + phase, view, value, {}});
    }
    std::vector<Value>
    certificates(unsigned phase, uint64_t view, size_t q) const
    {
        auto found = mReceived.find(view);
        if (found == mReceived.end())
            return {};
        auto const& votes = found->second.echoes[phase];
        std::set<Value> candidates{Value::safe()};
        for (auto const& p : votes)
            candidates.insert(p.second.begin(), p.second.end());
        std::vector<Value> result;
        for (auto const& value : candidates)
        {
            size_t n = 0;
            for (auto const& p : votes)
                n += p.second.count(value) || (value.tag == Value::Data &&
                                               p.second.count(Value::safe()));
            if (n >= q)
                result.push_back(value);
        }
        return result;
    }
    void
    enter(uint64_t view, Value const& guard, bool passive, uint64_t now)
    {
        mState.view = view;
        auto& s = mState.views[view];
        s.guard = guard;
        s.passive = passive;
        mEntered = now;
        if (view > 2)
            ensureChange(view - 2, guard);
        if (view > 1)
            ensureChange(view - 1, guard);
        if (passive)
            ensureChange(view, guard);
        else if (mSelf == leader(view))
        {
            s.proposal =
                guard.tag == Value::Safe ? Value::value(mState.input) : guard;
            mPrepareAlive.insert(view);
            send({Prepare, view, s.proposal, {}});
        }
        while (!mPrepareAlive.empty() && *mPrepareAlive.begin() < view &&
               view - *mPrepareAlive.begin() > 2)
            mPrepareAlive.erase(mPrepareAlive.begin());
        // Only current and preceding two views are persistent (line 18).
        while (!mState.views.empty() && mState.views.begin()->first < view &&
               view - mState.views.begin()->first > 2)
            mState.views.erase(mState.views.begin());
        while (!mReceived.empty() && mReceived.begin()->first < view &&
               view - mReceived.begin()->first > 2)
            mReceived.erase(mReceived.begin());
        // Abandon unsent messages from retired views too (line 19).
        mOutgoing.erase(std::remove_if(mOutgoing.begin(), mOutgoing.end(),
                                       [&](Message const& m) {
                                           return m.view < view &&
                                                  view - m.view > 2;
                                       }),
                        mOutgoing.end());
        mChanged = true;
    }
    void
    process(uint64_t now)
    {
        do
        {
            mChanged = false;
            auto view = mState.view;
            auto& current = mState.views.at(view);
            auto& received = mReceived[view];
            // Acceptance is separate from receipt: reconsider vote-2 on vote-1.
            for (auto& vr : mReceived)
                for (auto const& p : vr.second.pending2)
                    if (!vr.second.vote2.count(p.first))
                        for (auto const& msg : p.second)
                            if (msg.value.tag == Value::Bottom ||
                                count(vr.second.vote1, msg.value) >= mF + 1)
                            {
                                vr.second.vote2.emplace(p.first, msg);
                                mChanged = true;
                                break;
                            }
            if (!current.passive && !current.changing && received.proposal &&
                !current.vote1 &&
                (current.guard.tag == Value::Safe ||
                 current.guard == *received.proposal))
            {
                current.vote1 = Message{Vote1, view, *received.proposal, {}};
                send(*current.vote1);
            }
            if (!current.changing && !current.vote2)
                for (auto const& p : received.vote1)
                    if (count(received.vote1, p.second) >= mQ)
                    {
                        current.lock = p.second;
                        voteTwo(view, current.lock, current.guard);
                        break;
                    }
            if (!mState.decision)
            {
                std::map<Value, size_t> counts;
                for (auto const& p : received.vote2)
                    if (p.second.value.tag == Value::Data &&
                        ++counts[p.second.value] >= mQ)
                        mState.decision = p.second.value.data;
            }
            if (!current.passive && now - mEntered >= 3 * mDelta)
                voteTwo(view, current.lock, current.guard);
            if (now - mEntered >= 4 * mDelta && !current.changing)
            {
                current.changing = true;
                mChanged = true;
            }
            if (view > 1 && current.guard.tag != Value::Safe)
                for (auto const& v : certificates(1, view - 1, mF + 1))
                    if (v != current.guard)
                    {
                        current.guard = Value::safe();
                        mChanged = true;
                        break;
                    }
            for (auto& vm : mState.views)
            {
                auto v = vm.first;
                auto& s = vm.second;
                auto& r = mReceived[v];
                std::map<Value, size_t> locks, guards;
                for (auto const& p : r.vote2)
                {
                    ++locks[p.second.value];
                    ++guards[p.second.guard];
                }
                auto bottom = locks[Value{}];
                if (!s.alert)
                {
                    if (bottom >= mQ)
                    {
                        s.alert = false;
                        mChanged = true;
                    }
                    else
                        for (auto const& l : locks)
                            if (l.first.tag == Value::Data &&
                                bottom + l.second >= mQ)
                            {
                                s.alert = true;
                                s.potential = l.first;
                                mChanged = true;
                                break;
                            }
                }
                if (s.alert && *s.alert &&
                    r.vote2.size() - locks[s.potential] >= mQ)
                {
                    s.alert = false;
                    mChanged = true;
                }
                if (s.changing && s.alert)
                {
                    for (auto const& l : locks)
                        if (l.first.tag == Value::Data && l.second &&
                            (!*s.alert || l.first == s.potential))
                            echo(0, v, l.first);
                    if (!*s.alert)
                    {
                        size_t maxGuard = 0;
                        for (auto const& g : guards)
                        {
                            maxGuard = std::max(maxGuard, g.second);
                            if (g.second >= mF + 1)
                                echo(0, v, g.first);
                        }
                        if (r.vote2.size() - maxGuard >= mF + 1)
                            echo(0, v, Value::safe());
                    }
                }
                for (unsigned phase = 0; phase < 2; ++phase)
                    for (auto const& value : certificates(phase, v, mQ))
                        echo(phase + 1, v, value);
            }
            // Choose one enabled entry rule; rebuild references after entering.
            bool entered = false;
            for (auto const& vr : mReceived)
            {
                auto v = vr.first;
                if (v < view)
                    continue;
                auto cert3 =
                    certificates(2, v, mSelf == leader(v + 1) ? mF + 1 : mQ);
                if (!cert3.empty())
                {
                    enter(v + 1, cert3.front(), false, now);
                    entered = true;
                    break;
                }
                if (v > view && vr.second.proposal)
                {
                    auto cert2 = certificates(1, v - 1, mF + 1);
                    if (!cert2.empty())
                    {
                        enter(v, cert2.front(), false, now);
                        entered = true;
                        break;
                    }
                }
                auto cert1 = certificates(0, v, mF + 1);
                if (!cert1.empty())
                {
                    if (v > view)
                    {
                        enter(v, cert1.front(), true, now);
                        entered = true;
                        break;
                    }
                    ensureChange(v, cert1.front());
                }
            }
            if (entered)
                continue;
        } while (mChanged);
    }

  public:
    ForgetITConsensus(Node self, std::vector<Node> leaders, std::string input,
                      uint64_t now, uint64_t delta)
        : mSelf(self)
        , mLeaders(std::move(leaders))
        , mF((mLeaders.size() - 1) / 3)
        , mQ(mLeaders.size() - mF)
        , mDelta(delta)
    {
        if (mLeaders.size() < 4 || mLeaders.size() != 3 * mF + 1 || !delta ||
            delta > UINT64_MAX / 4 ||
            std::find(mLeaders.begin(), mLeaders.end(), self) ==
                mLeaders.end() ||
            std::set<Node>(mLeaders.begin(), mLeaders.end()).size() !=
                mLeaders.size())
            throw std::invalid_argument(
                "Invalid Forget-IT membership or delta");
        mState.input = std::move(input);
        enter(1, Value::safe(), false, now);
        process(now);
    }
    Node const&
    leader(uint64_t view) const
    {
        return mLeaders.at((view - 1) % mLeaders.size());
    }
    Persistent const&
    state() const
    {
        return mState;
    }
    void
    receive(Node const& sender, Message const& msg, uint64_t now)
    {
        if (std::find(mLeaders.begin(), mLeaders.end(), sender) ==
                mLeaders.end() ||
            !msg.view || msg.view == UINT64_MAX || msg.kind < Prepare ||
            msg.kind > Echo3 || !msg.value.valid() || !msg.guard.valid() ||
            (msg.view < mState.view && mState.view - msg.view > 2))
            return;
        if (msg.kind == Prepare && !(sender == leader(msg.view)))
            return;
        if ((msg.kind == Prepare || msg.kind == Vote1) &&
            msg.value.tag != Value::Data)
            return;
        if (msg.kind == Vote2 &&
            (msg.value.tag == Value::Safe || msg.guard.tag == Value::Bottom))
            return;
        if (msg.kind >= Echo1 && msg.value.tag == Value::Bottom)
            return;
        if (msg.kind != Vote2 && msg.guard.tag != Value::Bottom)
            return;
        record(sender, msg);
        process(now);
    }
    void
    tick(uint64_t now)
    {
        process(now);
    }
    std::vector<Message>
    takeOutgoing()
    {
        auto out = std::move(mOutgoing);
        mOutgoing.clear();
        return out;
    }
    std::vector<Message>
    alive() const
    {
        std::vector<Message> out;
        for (auto const& vm : mState.views)
        {
            auto v = vm.first;
            auto const& s = vm.second;
            if (mPrepareAlive.count(v))
                out.push_back({Prepare, v, s.proposal, {}});
            if (s.vote1)
                out.push_back(*s.vote1);
            if (s.vote2)
                out.push_back(*s.vote2);
            for (unsigned p = 0; p < 3; ++p)
                for (auto const& val : s.echoes[p])
                    out.push_back({Echo1 + p, v, val, {}});
        }
        return out;
    }
    // Algorithm 1 lines 51-56. The caller must atomically persist state before
    // sending messages if crash/restart durability is required.
    void
    recover(Persistent saved, uint64_t now)
    {
        mState = std::move(saved);
        mReceived.clear();
        mOutgoing.clear();
        mPrepareAlive.clear();
        mEntered = now;
        for (auto& vm : mState.views)
            vm.second.changing = true;
        auto& s = mState.views.at(mState.view);
        if (!s.vote2)
            s.vote2 = Message{Vote2, mState.view, s.lock, s.guard};
        for (auto const& vm : mState.views)
        {
            auto const& mem = vm.second;
            if (mem.vote1)
                send(*mem.vote1);
            if (mem.vote2)
                send(*mem.vote2);
            for (unsigned p = 0; p < 3; ++p)
                for (auto const& val : mem.echoes[p])
                    send({Echo1 + p, vm.first, val, {}});
        }
        process(now);
    }
};
}
