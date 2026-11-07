#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace stellar
{
// Shabdiz, Algorithm 1 and section 10.2 (November 2026 paper).
// No transport or application globals: other benchmark protocols do not use
// this state. Block storage is external, separate from retained consensus
// state.
template <typename Node, typename Hash> class ShabdizConsensus
{
  public:
    using View = uint64_t;
    struct Pair
    {
        View view = 0;
        Hash hash{};
        bool
        operator==(Pair const& b) const
        {
            return view == b.view && hash == b.hash;
        }
        bool
        operator<(Pair const& b) const
        {
            return std::tie(view, hash) < std::tie(b.view, b.hash);
        }
    };
    struct Block
    {
        View view = 0;
        Hash hash{}, parent{};
        std::string data;
    };
    enum class Kind : uint32_t
    {
        FastProposal,
        SlowProposal,
        Prepare,
        Commit,
        Collect,
        Send,
        Echo,
        Ready,
        Committed,
        Wish,
        GetBlock,
        BlockData
    };
    struct Message
    {
        Kind kind = Kind::Prepare;
        View view = 0; // consensus/collection view, not target view
        Pair value;
        Node origin{}; // SEND origin, authenticated at SEND receipt
        std::vector<Node> dependencies; // empty or one process identity
        std::vector<Node> selected;     // slow proposal's P
        std::optional<Block> block;
    };
    struct Outgoing
    {
        Message message;
        std::optional<Node> to; // empty means direct broadcast to fixed members
    };
    using ReadBlock = std::function<std::optional<Block>(Hash const&)>;
    using WriteBlock = std::function<void(Block const&)>;
    using Digest = std::function<Hash(View, Hash const&, std::string const&)>;
    using TakeData = std::function<std::optional<std::string>(Hash const&)>;
    using ValidateData = std::function<bool(std::string const&)>;

  private:
    using Dependency = std::optional<Node>;
    using ReadyVotes = std::map<Node, std::set<Dependency>>;
    struct OriginState
    {
        bool echoed = false, echoDelivered = false, amplified = false;
        std::map<Pair, std::set<Node>> echoes;
        std::map<Pair, ReadyVotes> readies, pending;
    };
    struct ViewState
    {
        std::optional<Hash> prepareSent;
        bool fastProposed = false, slowProposed = false;
        bool collectSent = false, sendSent = false;
        bool forceSlow = false;
        std::vector<Message> proposals;
        std::map<Node, Hash> prepares, commits;
        std::map<Node, OriginState> origins;
        std::map<Node, Pair> collection;
        std::optional<std::pair<Pair, std::vector<Node>>> slowPlan;
    };
    Node mSelf;
    std::vector<Node> mMembers;
    size_t mF, mQuorum;
    Hash mGenesis;
    View mView = 0, mOpen = 0, mConfirmed = 0, mHighestWish = 0;
    uint64_t mTimerGeneration = 0;
    Pair mPrepared, mCommitted;
    std::set<Pair> mPS;
    std::map<View, ViewState> mViews;
    std::map<Node, Pair> mReports;
    std::map<Node, View> mWishes;
    std::map<View, std::vector<std::pair<Node, Message>>> mFuture;
    std::deque<std::pair<Node, Message>> mInbox;
    std::set<Hash> mMissing;
    std::vector<Outgoing> mOutgoing;
    std::vector<Block> mExecuted;
    ReadBlock mRead;
    WriteBlock mWrite;
    Digest mDigest;
    TakeData mTakeData;
    ValidateData mValidate;
    bool mRunning = false, mStarted = false, mReportsDirty = false;
    bool mKeepPrepareHistory = false;
    size_t mForceSlow = 0;

    bool
    member(Node const& n) const
    {
        return std::binary_search(mMembers.begin(), mMembers.end(), n);
    }
    bool
    retained(View v) const
    {
        return v <= mView && (mView == 0 || v >= mView - 1);
    }
    Message
    message(Kind kind, View view, Pair const& value = {}) const
    {
        Message m;
        m.kind = kind;
        m.view = view;
        m.value = value;
        m.origin = mSelf;
        return m;
    }
    void
    emit(Message const& m, std::optional<Node> to = {})
    {
        mOutgoing.push_back({m, to});
        if (!to || *to == mSelf)
            mInbox.emplace_back(mSelf, m);
    }
    std::optional<Block>
    block(Hash const& h)
    {
        if (h == mGenesis)
            return Block{0, mGenesis, mGenesis, {}};
        auto b = mRead(h);
        if (!b && mMissing.insert(h).second)
            emit(message(Kind::GetBlock, 0, Pair{0, h}));
        return b;
    }
    bool
    validBlock(Message const& m)
    {
        if (!m.block)
            return true;
        auto const& b = *m.block;
        if (!b.view || b.hash == mGenesis || b.hash == b.parent ||
            !(m.value == Pair{b.view, b.hash}) || !mValidate(b.data) ||
            mDigest(b.view, b.parent, b.data) != b.hash)
            return false;
        mWrite(b);
        mMissing.erase(b.hash);
        mReportsDirty = true;
        return true;
    }
    bool
    targetBlock(Pair const& p)
    {
        auto b = block(p.hash);
        if (!b || b->view != p.view)
            return false;
        if (!p.view)
            return p.hash == mGenesis;
        auto parent = block(b->parent);
        return parent && parent->view < b->view;
    }
    size_t
    votes(std::map<Node, Hash> const& v, Hash const& hash) const
    {
        return std::count_if(v.begin(), v.end(),
                             [&](auto const& x) { return x.second == hash; });
    }
    bool
    dependencyMet(ViewState const& s, Pair const& p, Dependency const& d) const
    {
        if (!d)
            return true;
        auto it = s.collection.find(*d);
        return it != s.collection.end() && it->second.view > p.view;
    }
    // Choose a quorum with a unique block at the maximum view. No enumeration
    // of all quorum subsets. Freeze the choice when first valid, even if the
    // workload has not yet supplied a batch (the proof uses this selection
    // time).
    std::optional<std::pair<Pair, std::vector<Node>>>
    select(ViewState const& s) const
    {
        std::set<Pair> candidates;
        for (auto const& x : s.collection)
            candidates.insert(x.second);
        for (auto c = candidates.rbegin(); c != candidates.rend(); ++c)
        {
            std::vector<Node> selected;
            for (auto const& x : s.collection)
                if (x.second == *c)
                    selected.push_back(x.first);
            for (auto const& x : s.collection)
                if (x.second.view < c->view)
                    selected.push_back(x.first);
            if (selected.size() >= mQuorum)
            {
                selected.resize(mQuorum);
                return std::make_pair(*c, selected);
            }
        }
        return {};
    }
    std::optional<Pair>
    maximum(ViewState const& s, std::vector<Node> const& selected) const
    {
        if (selected.size() != mQuorum ||
            std::set<Node>(selected.begin(), selected.end()).size() != mQuorum)
            return {};
        Pair best{0, mGenesis};
        for (auto const& n : selected)
        {
            auto it = s.collection.find(n);
            if (it == s.collection.end())
                return {};
            if (it->second.view > best.view)
                best = it->second;
        }
        for (auto const& n : selected)
        {
            auto const& p = s.collection.at(n);
            if (p.view == best.view && !(p.hash == best.hash))
                return {};
        }
        return best;
    }
    bool
    supported(ViewState const& s, std::vector<Node> const& selected,
              Pair const& best) const
    {
        for (auto const& n : selected)
        {
            if (!(s.collection.at(n) == best))
                continue;
            auto oi = s.origins.find(n);
            if (oi == s.origins.end())
                continue;
            auto ri = oi->second.readies.find(best);
            if (ri == oi->second.readies.end())
                continue;
            size_t unconditional = 0;
            for (auto const& r : ri->second)
                unconditional += r.second.count(Dependency{}) != 0;
            if (unconditional >= mF + 1)
                return true;
        }
        return false;
    }
    void
    enter(View v)
    {
        if (v <= mView)
            return;
        mView = v;
        mOpen = v - 1;
        mMissing.clear(); // rederive outstanding fetches from retained roots
        mReportsDirty = true;
        auto& state = mViews[v];
        state.forceSlow = mForceSlow > 0;
        if (mForceSlow)
            --mForceSlow;
        for (auto it = mViews.begin(); it != mViews.end();)
            if (it->first < v - 1)
                it = mViews.erase(it);
            else
                ++it;
        for (auto it = mFuture.begin(); it != mFuture.end();)
        {
            if (it->first > v)
                break;
            if (it->first >= v - 1)
                for (auto& pending : it->second)
                    mInbox.push_back(std::move(pending));
            it = mFuture.erase(it);
        }
    }
    void
    requestNext()
    {
        if (mView == std::numeric_limits<View>::max())
            throw std::overflow_error("Shabdiz view exhausted");
        mConfirmed = mView;
        ++mTimerGeneration;
        enter(mView + 1);
    }
    void
    processWish(Node const& sender, View wish)
    {
        auto& old = mWishes[sender];
        old = std::max(old, wish);
        std::vector<View> ws;
        for (auto const& w : mWishes)
            ws.push_back(w.second);
        std::sort(ws.rbegin(), ws.rend());
        if (ws.size() >= mF + 1 && ws[mF] > mHighestWish)
        {
            mHighestWish = ws[mF];
            emit(message(Kind::Wish, mHighestWish));
        }
        if (ws.size() >= mQuorum && ws[mQuorum - 1] > mConfirmed)
        {
            mConfirmed = ws[mQuorum - 1];
            ++mTimerGeneration;
            enter(mConfirmed);
        }
    }
    void
    process(Node const& sender, Message const& m)
    {
        if (!member(sender) || m.dependencies.size() > 1 ||
            (!m.dependencies.empty() && !member(m.dependencies.front())))
            return;
        if (m.kind == Kind::GetBlock)
        {
            auto b = mRead(m.value.hash);
            if (b)
            {
                auto reply =
                    message(Kind::BlockData, 0, Pair{b->view, b->hash});
                reply.block = *b;
                emit(reply, sender);
            }
            return;
        }
        if (m.kind == Kind::BlockData)
        {
            // Only import requested blocks; unsolicited data is not a vote.
            if (mMissing.count(m.value.hash))
                validBlock(m);
            return;
        }
        if (m.kind == Kind::Wish)
        {
            processWish(sender, m.view);
            return;
        }
        if (m.kind == Kind::Committed)
        {
            if (m.value.view > mReports[sender].view)
            {
                mReports[sender] = m.value;
                mReportsDirty = true;
            }
            return;
        }
        if (m.view > mView)
        {
            // Deduplicate retransmissions before the view becomes active.
            auto& pending = mFuture[m.view];
            auto same = [&](auto const& x) {
                return x.first == sender && x.second.kind == m.kind &&
                       x.second.value == m.value &&
                       x.second.origin == m.origin &&
                       x.second.dependencies == m.dependencies &&
                       x.second.selected == m.selected;
            };
            if (std::none_of(pending.begin(), pending.end(), same))
                pending.emplace_back(sender, m);
            return;
        }
        if (!retained(m.view) || !m.view)
            return;
        auto& s = mViews[m.view];
        switch (m.kind)
        {
        case Kind::FastProposal:
        case Kind::SlowProposal:
            if (!(sender == leader(m.view)) || m.value.view != m.view ||
                !m.block || (m.kind == Kind::FastProposal && m.view != mView) ||
                (m.kind == Kind::SlowProposal &&
                 m.selected.size() != mQuorum) ||
                std::any_of(s.proposals.begin(), s.proposals.end(),
                            [&](auto const& p) { return p.kind == m.kind; }) ||
                !validBlock(m))
                return;
            // A correct leader sends one proposal. Keep both path variants to
            // permit a pending slow proposal while its prerequisites arrive.
            s.proposals.push_back(m);
            break;
        case Kind::Prepare:
        case Kind::Commit:
            if (m.value.view != m.view)
                return;
            (m.kind == Kind::Prepare ? s.prepares : s.commits)
                .emplace(sender, m.value.hash);
            break;
        case Kind::Collect:
            if (!(sender == leader(m.view)) || m.view != mView || s.sendSent)
                return;
            mOpen = mView; // close BEFORE taking the prepared snapshot
            s.sendSent = true;
            emit(message(Kind::Send, m.view, mPrepared));
            break;
        case Kind::Send:
        {
            if (!(m.origin == sender))
                return;
            auto& o = s.origins[sender];
            if (o.echoed)
                return;
            o.echoed = true;
            auto echo = message(Kind::Echo, m.view, m.value);
            echo.origin = sender;
            emit(echo);
            break;
        }
        case Kind::Echo:
            if (member(m.origin))
                s.origins[m.origin].echoes[m.value].insert(sender);
            break;
        case Kind::Ready:
            if (member(m.origin))
            {
                Dependency d;
                if (!m.dependencies.empty())
                    d = m.dependencies.front();
                s.origins[m.origin].pending[m.value][sender].insert(d);
            }
            break;
        default:
            break;
        }
    }
    bool
    collectionRules(View v, ViewState& s)
    {
        bool changed = false;
        for (auto& entry : s.origins)
        {
            auto const& origin = entry.first;
            auto& o = entry.second;
            for (auto const& e : o.echoes)
            {
                if (!o.echoDelivered && e.second.size() >= mQuorum)
                {
                    o.echoDelivered = true;
                    changed = true;
                    auto r = message(Kind::Ready, v, e.first);
                    r.origin = origin;
                    if (mPS.count(e.first))
                        emit(r);
                    else if (e.first.view < mPrepared.view)
                    {
                        r.dependencies.push_back(mSelf);
                        emit(r);
                    }
                }
            }
            for (auto pi = o.pending.begin(); pi != o.pending.end();)
            {
                for (auto si = pi->second.begin(); si != pi->second.end();)
                {
                    for (auto di = si->second.begin(); di != si->second.end();)
                    {
                        if (dependencyMet(s, pi->first, *di))
                        {
                            changed |= o.readies[pi->first][si->first]
                                           .insert(*di)
                                           .second;
                            di = si->second.erase(di);
                        }
                        else
                            ++di;
                    }
                    if (si->second.empty())
                        si = pi->second.erase(si);
                    else
                        ++si;
                }
                if (pi->second.empty())
                    pi = o.pending.erase(pi);
                else
                    ++pi;
            }
            for (auto const& r : o.readies)
            {
                if (!o.amplified && r.second.size() >= mF + 1)
                {
                    o.amplified = true;
                    changed = true;
                    auto reply = message(Kind::Ready, v, r.first);
                    reply.origin = origin;
                    std::set<Node> dependencies;
                    for (auto const& sender : r.second)
                        for (auto const& d : sender.second)
                            if (d)
                                dependencies.insert(*d);
                    if (!dependencies.empty())
                        reply.dependencies.push_back(*dependencies.begin());
                    emit(reply);
                }
                if (r.second.size() >= mQuorum && !s.collection.count(origin))
                {
                    s.collection.emplace(origin, r.first);
                    changed = true;
                    // Select at the FIRST valid collection, before processing
                    // another delivery or waiting for client work.
                    if (mSelf == leader(v) && !s.slowPlan)
                        s.slowPlan = select(s);
                }
            }
        }
        return changed;
    }
    bool
    proposalRules(View v, ViewState& s)
    {
        bool changed = false;
        if (mSelf == leader(v) && v == mView && !s.fastProposed &&
            !s.collectSent)
        {
            bool fast = mPrepared.view == v - 1 &&
                        (v <= 1 || mCommitted.view >= v - 2) && !s.forceSlow;
            if (!fast)
            {
                s.collectSent = true;
                changed = true;
                emit(message(Kind::Collect, v));
            }
            else if (auto data = mTakeData(mPrepared.hash))
            {
                Block b{v, mDigest(v, mPrepared.hash, *data), mPrepared.hash,
                        *data};
                auto p = message(Kind::FastProposal, v, Pair{v, b.hash});
                p.block = b;
                s.fastProposed = true;
                changed = true;
                emit(p);
            }
        }
        if (mSelf == leader(v) && s.collectSent && !s.slowProposed &&
            s.slowPlan && s.slowPlan->first.view < v &&
            targetBlock(s.slowPlan->first))
        {
            if (auto data = mTakeData(s.slowPlan->first.hash))
            {
                Block b{v, mDigest(v, s.slowPlan->first.hash, *data),
                        s.slowPlan->first.hash, *data};
                auto p = message(Kind::SlowProposal, v, Pair{v, b.hash});
                p.block = b;
                p.selected = s.slowPlan->second;
                s.slowProposed = true;
                changed = true;
                emit(p);
            }
        }
        for (auto const& p : s.proposals)
        {
            if (s.prepareSent || !targetBlock(p.value))
                continue;
            auto const& b = *p.block;
            bool acceptable = false;
            if (p.kind == Kind::FastProposal)
                acceptable = v == mView && mPrepared.view == v - 1 &&
                             b.parent == mPrepared.hash;
            else if (auto best = maximum(s, p.selected))
                acceptable = best->view < v && b.parent == best->hash &&
                             supported(s, p.selected, *best);
            if (acceptable)
            {
                s.prepareSent = b.hash;
                mPS.insert(p.value);
                changed = true;
                emit(message(Kind::Prepare, v, p.value));
            }
        }
        return changed;
    }
    bool
    commitChain(Pair const& target)
    {
        if (target.view <= mCommitted.view)
            return false;
        std::vector<Block> chain;
        auto b = block(target.hash);
        if (!b || b->view != target.view)
            return false;
        while (b->hash != mCommitted.hash)
        {
            if (b->view <= mCommitted.view)
                return false; // not an extension
            chain.push_back(*b);
            auto parent = block(b->parent);
            if (!parent || parent->view >= b->view)
                return false;
            b = std::move(parent);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            mExecuted.push_back(*it);
        mCommitted = target;
        emit(message(Kind::Committed, 0, mCommitted));
        mReportsDirty = true;
        return true;
    }
    bool
    votingRules(View v, ViewState& s)
    {
        bool changed = false;
        std::set<Hash> candidates;
        for (auto const& x : s.prepares)
            candidates.insert(x.second);
        for (auto const& x : s.commits)
            candidates.insert(x.second);
        for (auto const& h : candidates)
        {
            Pair p{v, h};
            if (mOpen <= v && mPrepared.view < v &&
                (votes(s.prepares, h) >= mQuorum ||
                 votes(s.commits, h) >= mF + 1) &&
                targetBlock(p))
            {
                mPrepared = p;
                changed = true;
                if (!mKeepPrepareHistory)
                    for (auto it = mPS.begin();
                         it != mPS.end() && it->view < v;)
                        it = mPS.erase(it);
                emit(message(Kind::Commit, v, p));
            }
            if (votes(s.commits, h) >= mQuorum)
                changed |= commitChain(p);
        }
        return changed;
    }
    bool
    catchUp()
    {
        if (!mReportsDirty)
            return false;
        mReportsDirty = false;
        std::map<Pair, size_t> ancestors;
        for (auto const& report : mReports)
        {
            if (report.second.view <= mCommitted.view)
                continue;
            auto b = block(report.second.hash);
            if (!b || b->view != report.second.view)
                continue;
            while (b && b->view > mCommitted.view)
            {
                ++ancestors[Pair{b->view, b->hash}];
                auto parent = block(b->parent);
                if (!parent || parent->view >= b->view)
                    break;
                b = std::move(parent);
            }
        }
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
            if (it->second >= mF + 1 && commitChain(it->first))
                return true;
        return false;
    }
    void
    drive()
    {
        if (mRunning || !mStarted)
            return;
        mRunning = true;
        try
        {
            for (;;)
            {
                while (!mInbox.empty())
                {
                    auto incoming = std::move(mInbox.front());
                    mInbox.pop_front();
                    process(incoming.first, incoming.second);
                }
                bool changed = false;
                for (auto& view : mViews)
                {
                    if (!view.first)
                        continue; // genesis is already prepared
                    changed |= collectionRules(view.first, view.second);
                    changed |= proposalRules(view.first, view.second);
                    changed |= votingRules(view.first, view.second);
                }
                changed |= catchUp();
                if (mPrepared.view == mView &&
                    (mView == 0 || mCommitted.view >= mView - 1))
                {
                    requestNext();
                    changed = true;
                }
                if (!changed && mInbox.empty())
                    break;
            }
        }
        catch (...)
        {
            mRunning = false;
            throw;
        }
        mRunning = false;
    }

  public:
    ShabdizConsensus(Node self, std::vector<Node> members, Hash genesis,
                     ReadBlock read, WriteBlock write, Digest digest,
                     TakeData takeData, ValidateData validate,
                     bool keepPrepareHistory = false)
        : mSelf(self)
        , mMembers(std::move(members))
        , mGenesis(genesis)
        , mPrepared{0, genesis}
        , mCommitted{0, genesis}
        , mRead(std::move(read))
        , mWrite(std::move(write))
        , mDigest(std::move(digest))
        , mTakeData(std::move(takeData))
        , mValidate(std::move(validate))
        , mKeepPrepareHistory(keepPrepareHistory)
    {
        std::sort(mMembers.begin(), mMembers.end());
        if (mMembers.size() < 4 ||
            std::adjacent_find(mMembers.begin(), mMembers.end()) !=
                mMembers.end() ||
            !member(mSelf))
            throw std::invalid_argument("Invalid Shabdiz membership");
        mF = (mMembers.size() - 1) / 3;
        // n-f equals 2f+1 for the paper's n=3f+1. For larger memberships,
        // it preserves quorum intersection >= f+1 and availability with f
        // absent.
        mQuorum = mMembers.size() - mF;
        mPS.insert(mPrepared);
        for (auto const& n : mMembers)
            mReports[n] = mCommitted;
        mViews[0].prepareSent = genesis;
    }
    Node const&
    leader(View v) const
    {
        return mMembers[(v ? v - 1 : 0) % mMembers.size()];
    }
    void
    start()
    {
        mStarted = true;
        drive();
    }
    void
    receive(Node const& sender, Message const& m)
    {
        if (member(sender))
        {
            mInbox.emplace_back(sender, m);
            drive();
        }
    }
    void
    workAvailable()
    {
        drive();
    }
    void
    timeout()
    {
        if (mConfirmed == std::numeric_limits<View>::max())
            return;
        mHighestWish = std::max(mHighestWish, mConfirmed + 1);
        emit(message(Kind::Wish, mHighestWish));
        drive();
    }
    // Retry only durable reports and unavailable blocks, not old consensus
    // votes. WISH retransmission after timer expiry is driven by the adapter.
    void
    retransmit()
    {
        if (mCommitted.view)
            emit(message(Kind::Committed, 0, mCommitted));
        for (auto const& h : mMissing)
            emit(message(Kind::GetBlock, 0, Pair{0, h}));
        drive();
    }
    void
    forceSlowViews(size_t count)
    {
        mForceSlow = count;
    }
    std::vector<Outgoing>
    takeOutgoing()
    {
        auto out = std::move(mOutgoing);
        mOutgoing.clear();
        return out;
    }
    std::vector<Block>
    takeExecuted()
    {
        auto out = std::move(mExecuted);
        mExecuted.clear();
        return out;
    }
    View
    view() const
    {
        return mView;
    }
    View
    oldestOpenView() const
    {
        return mOpen;
    }
    View
    confirmedView() const
    {
        return mConfirmed;
    }
    uint64_t
    timerGeneration() const
    {
        return mTimerGeneration;
    }
    Pair
    prepared() const
    {
        return mPrepared;
    }
    Pair
    committed() const
    {
        return mCommitted;
    }
    size_t
    quorum() const
    {
        return mQuorum;
    }
    size_t
    retainedViews() const
    {
        return mViews.size();
    }
    size_t
    prepareHistorySize() const
    {
        return mPS.size();
    }
};
} // namespace stellar
