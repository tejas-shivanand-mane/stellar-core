#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace stellar
{
// Basic TetraBFT view zero, or Forget-IT view one (SAFE guard).
// Transport, membership validation, payload validation and instance numbering
// are the caller's responsibility. No recovery or chained multi-shot protocol.
template <typename Node, typename Digest, unsigned Phases = 4> class BasicNormalPath
{
    std::optional<Digest> mProposal;
    std::array<std::map<Node, Digest>, Phases> mVotes;
    std::array<bool, Phases> mSent{};
    bool mDecided = false;

  public:
    bool propose(Digest const& digest)
    {
        if (mProposal)
            return *mProposal == digest;
        mProposal = digest;
        return true;
    }

    void vote(unsigned phase, Node const& sender, Digest const& digest)
    {
        if (phase < 1 || phase > Phases)
            throw std::invalid_argument("Invalid normal-path vote phase");
        // At most one vote per sender/phase, including equivocation.
        mVotes[phase - 1].emplace(sender, digest);
    }

    std::vector<unsigned> advance(Node const& self, size_t quorum)
    {
        std::vector<unsigned> outgoing;
        if (!mProposal || mDecided)
            return outgoing;
        auto count = [&](unsigned phase) {
            size_t n = 0;
            for (auto const& v : mVotes[phase - 1])
                n += v.second == *mProposal;
            return n;
        };
        for (unsigned phase = 1; phase <= Phases; ++phase)
        {
            if (phase > 1 && count(phase - 1) < quorum)
                break;
            if (!mSent[phase - 1])
            {
                mSent[phase - 1] = true;
                vote(phase, self, *mProposal);
                outgoing.push_back(phase);
            }
        }
        return outgoing;
    }

    bool decide(size_t quorum, size_t acceptance = 0)
    {
        if (!mProposal || mDecided)
            return false;
        // Forget-IT Algorithm 1, line 23: non-bottom vote-2 messages
        // become acceptable only after f+1 matching vote-1 messages.
        size_t first = 0;
        for (auto const& v : mVotes[0])
            first += v.second == *mProposal;
        if (first < acceptance)
            return false;
        size_t count = 0;
        for (auto const& v : mVotes[Phases - 1])
            count += v.second == *mProposal;
        if (count < quorum)
            return false;
        mDecided = true;
        return true;
    }

    Digest const& digest() const { return mProposal.value(); }
};

template <typename Node, typename Digest>
using TetraBFTNormal = BasicNormalPath<Node, Digest, 4>;
template <typename Node, typename Digest>
using ForgetITNormal = BasicNormalPath<Node, Digest, 2>;

// Simple-IT Fig. 2, with a safe consecutive parent provided by the caller.
// Bracha RBC: proposal/echo/ready followed by consensus commit.
// Opt-RBC: Fig. 1 of arXiv:2505.02761v4; only nonleader votes count
// toward echo/vote/ack thresholds. No timeout/disable or speculation.
template <typename Node, typename Digest> class SimpleITNormal
{
    std::optional<Digest> mProposal;
    std::array<std::map<Node, Digest>, 4> mVotes;
    std::set<Node> mCommits;
    std::array<bool, 5> mSent{};
    bool mDelivered = false;
    bool mDecided = false;

    size_t count(unsigned phase, Node const& leader, bool excludeLeader) const
    {
        size_t result = 0;
        for (auto const& v : mVotes[phase - 1])
            result += (!excludeLeader || !(v.first == leader)) && v.second == *mProposal;
        return result;
    }

  public:
    enum Phase : unsigned { Echo = 1, Vote = 2, Ack = 3, Ready = 4, Commit = 5 };
    bool propose(Digest const& digest)
    {
        if (mProposal)
            return *mProposal == digest;
        mProposal = digest;
        return true;
    }
    void vote(unsigned phase, Node const& sender, Digest const& digest)
    {
        if (phase < 1 || phase > 5)
            throw std::invalid_argument("Invalid Simple-IT phase");
        if (phase == Commit)
        {
            commit(sender);
            return;
        }
        mVotes[phase - 1].emplace(sender, digest);
    }
    // Simple-IT Figure 2: <commit, round>. The caller selects this round's
    // state; no value is attached. Early commits wait for local RBC delivery.
    void commit(Node const& sender)
    {
        mCommits.insert(sender);
    }
    std::vector<unsigned> advance(Node const& self, Node const& leader,
                                  size_t n, bool optimistic, bool safeParent)
    {
        std::vector<unsigned> result;
        if (!mProposal)
            return result;
        size_t f = (n - 1) / 3;
        size_t q = n - f;
        auto send = [&](unsigned phase) {
            if (!mSent[phase - 1])
            {
                mSent[phase - 1] = true;
                vote(phase, self, *mProposal);
                result.push_back(phase);
            }
        };
        send(Echo);
        if (optimistic)
        {
            // ceil(n/2), ceil((n+f-1)/2), ceil((n+2f-2)/2).
            size_t echo = count(Echo, leader, true);
            if (echo >= (n + 1) / 2)
                send(Vote);
            if (echo >= (n + f) / 2 || count(Vote, leader, true) >= (n + f) / 2)
                send(Ack);
            if (count(Ack, leader, true) >= (n + f) / 2 ||
                count(Ready, leader, false) >= f + 1)
                send(Ready);
            if (echo >= (n + 2 * f - 1) / 2)
                mDelivered = true;
        }
        else if (count(Echo, leader, false) >= q ||
                 count(Ready, leader, false) >= f + 1)
            send(Ready);
        if (count(Ready, leader, false) >= q)
            mDelivered = true;
        if (mDelivered && safeParent)
            send(Commit);
        return result;
    }
    bool decide(size_t quorum, Node const& /*leader*/, bool safeParent)
    {
        if (mDecided || !mDelivered || !safeParent ||
            mCommits.size() < quorum)
            return false;
        mDecided = true;
        return true;
    }
    Digest const& digest() const { return mProposal.value(); }
};
}
