#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace stellar
{
// IT-HS Part One (September 20, 2021), not the OPODIS protocol.
// https://decentralizedthoughts.github.io/2021-09-20-information-theoretic-hotstuff-it-hs-part-one/
// The caller supplies authenticated membership, synchronized clock readings,
// and a common epoch (start of view 1). View v begins at epoch+(v-1)*10*Delta.
// Locks are justified by n-f ACCEPT messages, following the overview and
// safety proof; the main-protocol pseudocode's ECHO reference is inconsistent.
// Initial view-zero input locks need no evidence: no prior decision exists.
// Keep all historical ACCEPT evidence until this one-shot instance terminates.
template <class Node> class ITHSBlogConsensus
{
  public:
    enum Kind : unsigned
    {
        Suggest = 1,
        Propose,
        Echo,
        Accept,
        Lock,
        Commit
    };
    struct Message
    {
        unsigned kind;
        uint64_t view; // COMMIT has no view: canonical wire representation is 0
        uint64_t lockView;
        std::string value;
    };
    struct Outgoing
    {
        Message message;
        std::optional<Node>
            recipient; // SUGGEST is unicast, other messages broadcast
    };
    struct Locked
    {
        uint64_t view = 0;
        std::string value;
    };

  private:
    struct Round
    {
        std::map<Node, Locked> suggestions;
        std::optional<Locked> proposal;
        std::array<std::map<Node, std::string>, 3> votes;
        std::array<std::optional<std::string>, 3> sent;
        bool proposed = false;
    };
    Node mSelf;
    std::vector<Node> mLeaders;
    size_t mF, mQ;
    uint64_t mEpoch, mDelta, mView = 0;
    Locked mLock;
    std::map<uint64_t, Round> mRounds;
    std::map<Node, std::string> mCommits;
    std::optional<std::string> mCommitSent, mDecision;
    std::vector<Outgoing> mOutgoing;
    bool mChanged = false;

    size_t
    count(std::map<Node, std::string> const& votes,
          std::string const& value) const
    {
        size_t n = 0;
        for (auto const& v : votes)
            n += v.second == value;
        return n;
    }
    bool
    justified(Locked const& lock) const
    {
        if (lock.view == 0)
            return true; // explicit initial-input bootstrap
        auto it = mRounds.find(lock.view);
        return it != mRounds.end() &&
               count(it->second.votes[1], lock.value) >= mQ;
    }
    void
    record(Node const& sender, Message const& msg)
    {
        if (msg.kind == Commit)
        {
            mCommits.emplace(sender, msg.value);
            return;
        }
        auto& round = mRounds[msg.view];
        if (msg.kind == Suggest)
            round.suggestions.emplace(sender, Locked{msg.lockView, msg.value});
        else if (msg.kind == Propose)
        {
            if (!round.proposal)
                round.proposal = Locked{msg.lockView, msg.value};
        }
        else
            round.votes[msg.kind - Echo].emplace(sender, msg.value);
    }
    void
    send(Message msg, std::optional<Node> recipient = {})
    {
        if (!recipient || *recipient == mSelf)
            record(mSelf, msg);
        if (!recipient || !(*recipient == mSelf))
            mOutgoing.push_back({std::move(msg), recipient});
        mChanged = true;
    }
    void
    vote(unsigned kind, uint64_t view, std::string const& value)
    {
        auto& sent = mRounds[view].sent[kind - Echo];
        if (sent)
            return;
        sent = value;
        send({kind, view, 0, value});
    }
    void
    commit(std::string const& value)
    {
        if (mCommitSent)
            return;
        mCommitSent = value;
        send({Commit, 0, 0, value});
    }
    void
    process(uint64_t now)
    {
        if (mDecision || now < mEpoch)
            return;
        auto scheduled = 1 + (now - mEpoch) / (10 * mDelta);
        if (scheduled > mView)
        {
            mView = scheduled;
            send({Suggest, mView, mLock.view, mLock.value}, leader(mView));
        }
        do
        {
            mChanged = false;
            // Background ACCEPT boosting remains active for past/future views.
            for (auto& r : mRounds)
                if (!r.second.sent[1])
                    for (auto const& v : r.second.votes[1])
                        if (count(r.second.votes[1], v.second) >= mF + 1)
                        {
                            vote(Accept, r.first, v.second);
                            break;
                        }
            if (!mCommitSent)
                for (auto const& v : mCommits)
                    if (count(mCommits, v.second) >= mF + 1)
                    {
                        commit(v.second);
                        break;
                    }
            for (auto const& v : mCommits)
                if (count(mCommits, v.second) >= mQ)
                {
                    mDecision = v.second;
                    break;
                }
            if (mDecision)
                break;
            auto& round = mRounds[mView];
            uint64_t start = mEpoch + (mView - 1) * (10 * mDelta);
            if (mSelf == leader(mView) && !round.proposed &&
                now - start >= 2 * mDelta)
            {
                size_t valid = 0;
                std::optional<Locked> best;
                for (auto const& s : round.suggestions)
                    if (justified(s.second))
                    {
                        ++valid;
                        if (!best || s.second.view > best->view ||
                            (s.second.view == best->view &&
                             s.second.value < best->value))
                            best = s.second;
                    }
                if (valid >= mQ)
                {
                    round.proposed = true;
                    send({Propose, mView, best->view, best->value});
                }
            }
            if (round.proposal && round.proposal->view >= mLock.view &&
                justified(*round.proposal))
                vote(Echo, mView, round.proposal->value);
            if (!round.sent[1])
                for (auto const& v : round.votes[0])
                    if (count(round.votes[0], v.second) >= mQ)
                    {
                        vote(Accept, mView, v.second);
                        break;
                    }
            if (!round.sent[2])
                for (auto const& v : round.votes[1])
                    if (count(round.votes[1], v.second) >= mQ)
                    {
                        mLock = {mView, v.second};
                        vote(Lock, mView, v.second);
                        break;
                    }
            if (!mCommitSent)
                for (auto const& v : round.votes[2])
                    if (count(round.votes[2], v.second) >= mQ)
                    {
                        commit(v.second);
                        break;
                    }
        } while (mChanged);
    }

  public:
    ITHSBlogConsensus(Node self, std::vector<Node> leaders, std::string input,
                      uint64_t epoch, uint64_t now, uint64_t delta)
        : mSelf(self)
        , mLeaders(std::move(leaders))
        , mF((mLeaders.size() - 1) / 3)
        , mQ(mLeaders.size() - mF)
        , mEpoch(epoch)
        , mDelta(delta)
        , mLock{0, std::move(input)}
    {
        if (mLeaders.size() < 4 || !delta || delta > UINT64_MAX / 10 ||
            std::find(mLeaders.begin(), mLeaders.end(), self) ==
                mLeaders.end() ||
            std::set<Node>(mLeaders.begin(), mLeaders.end()).size() !=
                mLeaders.size())
            throw std::invalid_argument(
                "Invalid IT-HS blog membership or Delta");
        process(now);
    }
    Node const&
    leader(uint64_t view) const
    {
        return mLeaders.at((view - 1) % mLeaders.size());
    }
    uint64_t
    view() const
    {
        return mView;
    }
    uint64_t
    epoch() const
    {
        return mEpoch;
    }
    Locked const&
    lock() const
    {
        return mLock;
    }
    std::optional<std::string> const&
    decision() const
    {
        return mDecision;
    }
    void
    tick(uint64_t now)
    {
        process(now);
    }
    void
    receive(Node const& sender, Message const& msg, uint64_t now)
    {
        if (mDecision ||
            std::find(mLeaders.begin(), mLeaders.end(), sender) ==
                mLeaders.end() ||
            msg.kind < Suggest || msg.kind > Commit)
            return;
        if (msg.kind == Commit)
        {
            if (msg.view || msg.lockView)
                return;
        }
        else
        {
            if (!msg.view)
                return;
            if (msg.kind == Suggest || msg.kind == Propose)
            {
                if (msg.lockView >= msg.view)
                    return;
            }
            else if (msg.lockView)
                return;
            if (msg.kind == Suggest && !(mSelf == leader(msg.view)))
                return;
            if (msg.kind == Propose && !(sender == leader(msg.view)))
                return;
        }
        record(sender, msg);
        process(now);
    }
    std::vector<Outgoing>
    takeOutgoing()
    {
        auto out = std::move(mOutgoing);
        mOutgoing.clear();
        return out;
    }
    std::optional<uint64_t>
    nextDeadline(uint64_t now) const
    {
        if (mDecision)
            return {};
        if (now < mEpoch)
            return mEpoch;
        auto view = 1 + (now - mEpoch) / (10 * mDelta);
        if (view > mView)
            return now; // another instance must not cancel this due transition
        auto start = mEpoch + (view - 1) * (10 * mDelta);
        if (mSelf == leader(view) && now - start < 2 * mDelta)
            return start + 2 * mDelta;
        if (start > UINT64_MAX - 10 * mDelta)
            return {};
        return start + 10 * mDelta;
    }
};
}
