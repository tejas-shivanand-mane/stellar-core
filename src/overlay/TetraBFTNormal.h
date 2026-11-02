#pragma once

#include <array>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace stellar
{
// Basic TetraBFT, Section 3.2, view zero only. One instance per batch.
// Transport, membership validation, payload validation and instance numbering
// are the caller's responsibility. No recovery or chained multi-shot protocol.
template <typename Node, typename Digest> class TetraBFTNormal
{
    std::optional<Digest> mProposal;
    std::array<std::map<Node, Digest>, 4> mVotes;
    std::array<bool, 4> mSent{};
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
        if (phase < 1 || phase > 4)
            throw std::invalid_argument("TetraBFT vote phase");
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
        for (unsigned phase = 1; phase <= 4; ++phase)
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

    bool decide(size_t quorum)
    {
        if (!mProposal || mDecided)
            return false;
        size_t count = 0;
        for (auto const& v : mVotes[3])
            count += v.second == *mProposal;
        if (count < quorum)
            return false;
        mDecided = true;
        return true;
    }

    Digest const& digest() const { return mProposal.value(); }
};
}
