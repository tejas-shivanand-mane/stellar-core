#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace stellar
{
// Application block archive, separate from the paper's retained voting state.
// The experiment has no crash/restart recovery contract. A queued block is
// immediately available to chain fetch; disk completion is NOT a voting/ACK
// prerequisite. Bound pending bytes through the wire size and record count,
// and apply backpressure if the disk cannot keep up (never discard ancestry).
// Public methods are called on the application thread; only the writer runs
// in the background. There is no timer or per-block fsync.
class ShabdizArchive
{
  public:
    using Writer =
        std::function<void(std::filesystem::path const&, std::string const&)>;

  private:
    static constexpr size_t FILTER_BITS = 1 << 23; // fixed 1 MiB
    static constexpr size_t RECENT_KEYS = 256;
    std::filesystem::path mRoot;
    size_t mLimit;
    Writer mWrite;
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::map<std::string, std::shared_ptr<std::string const>> mPending;
    std::deque<std::string> mQueue;
    std::exception_ptr mFailure;
    bool mStopping = false;
    std::thread mWorker;
    std::vector<uint64_t> mFilter = std::vector<uint64_t>(FILTER_BITS / 64);
    std::set<std::string> mRecent;
    std::deque<std::string> mRecentOrder;

    // A negative answer proves absence in this new per-run archive. False
    // positives fall back to the filesystem; they NEVER drop a request.
    bool
    filter(std::string const& key, bool insert)
    {
        uint64_t h = std::hash<std::string>{}(key);
        uint64_t step = ((h >> 32) ^ (h << 13)) | 1;
        bool found = true;
        for (int i = 0; i < 4; ++i, h += step)
        {
            auto bit = h & (FILTER_BITS - 1);
            auto mask = uint64_t(1) << (bit % 64);
            found &= (mFilter[bit / 64] & mask) != 0;
            if (insert)
                mFilter[bit / 64] |= mask;
        }
        return found;
    }
    void
    recent(std::string const& key)
    {
        if (mRecent.insert(key).second)
            mRecentOrder.push_back(key);
        while (mRecent.size() > RECENT_KEYS)
        {
            mRecent.erase(mRecentOrder.front());
            mRecentOrder.pop_front();
        }
    }
    void
    checkLocked() const
    {
        if (mFailure)
            std::rethrow_exception(mFailure);
    }
    void
    run()
    {
        try
        {
            for (;;)
            {
                std::string key;
                std::shared_ptr<std::string const> data;
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mChanged.wait(lock,
                                  [&] { return mStopping || !mQueue.empty(); });
                    if (mQueue.empty())
                        return;
                    key = std::move(mQueue.front());
                    mQueue.pop_front();
                    data = mPending.at(key);
                }
                mWrite(mRoot / key, *data);
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    mPending.erase(key);
                }
                mChanged.notify_all();
            }
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mFailure = std::current_exception();
            mChanged.notify_all();
        }
    }

  public:
    explicit ShabdizArchive(std::filesystem::path root, size_t limit = 256,
                            Writer writer = {})
        : mRoot(std::move(root)), mLimit(limit), mWrite(std::move(writer))
    {
        if (!mLimit || std::filesystem::exists(mRoot))
            throw std::invalid_argument("Shabdiz archive requires a new "
                                        "directory and a positive queue limit");
        std::filesystem::create_directories(mRoot);
        if (!mWrite)
            mWrite = [](auto const& path, auto const& bytes) {
                std::ofstream out(path, std::ios::binary);
                out.write(bytes.data(), bytes.size());
                out.close();
                if (!out)
                    throw std::runtime_error("Cannot write Shabdiz archive");
            };
        mWorker = std::thread([this] { run(); });
    }
    ~ShabdizArchive()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mStopping = true;
        }
        mChanged.notify_all();
        mWorker.join();
    }
    void
    check()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        checkLocked();
    }
    bool
    contains(std::string const& key)
    {
        check();
        if (mRecent.count(key))
            return true;
        if (!filter(key, false))
            return false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mPending.count(key))
                return true;
        }
        return std::filesystem::exists(mRoot / key);
    }
    void
    put(std::string const& key, std::string data)
    {
        if (data.size() > 100000)
            throw std::invalid_argument("Oversized Shabdiz archive record");
        if (contains(key))
            return;
        auto bytes = std::make_shared<std::string const>(std::move(data));
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mChanged.wait(lock,
                          [&] { return mFailure || mPending.size() < mLimit; });
            checkLocked();
            mPending.emplace(key, std::move(bytes));
            mQueue.push_back(key);
        }
        filter(key, true);
        recent(key);
        mChanged.notify_all();
    }
    std::optional<std::string>
    get(std::string const& key)
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            checkLocked();
            auto it = mPending.find(key);
            if (it != mPending.end())
                return *it->second;
        }
        if (!filter(key, false))
            return {};
        std::ifstream in(mRoot / key, std::ios::binary);
        if (!in)
        {
            if (!std::filesystem::exists(mRoot / key))
                return {};
            throw std::runtime_error("Cannot read Shabdiz archive");
        }
        return std::string(std::istreambuf_iterator<char>(in), {});
    }
    void
    flush()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mChanged.wait(lock, [&] { return mFailure || mPending.empty(); });
        checkLocked();
    }
};
} // namespace stellar
