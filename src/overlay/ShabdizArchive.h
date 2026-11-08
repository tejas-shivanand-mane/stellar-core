#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

// Match SOCI's SQLite declarations when included alongside Database.h.
namespace sqlite_api
{
#include <sqlite3.h>
}

namespace stellar
{
// Per-run application archive, separate from retained consensus votes. Blocks
// and execution markers share one indexed database (plus SQLite's WAL/SHM).
// The background writer compresses blocks and commits up to 1000 blocks per
// transaction. Pending data is immediately fetchable; persistence is never a
// vote/ACK prerequisite. A bounded queue still applies backpressure if storage
// cannot keep up. No fsync/crash-restart durability is promised by this
// benchmark. Public operations have one caller; only the writer runs
// concurrently.
class ShabdizArchive
{
  public:
    struct Options
    {
        size_t batchBlocks = 1000;
        size_t maxPendingRecords = 4096;
        size_t maxPendingBytes = 64 * 1024 * 1024;
        std::chrono::milliseconds maxDelay{2000};
    };
    struct Stats
    {
        uint64_t transactions = 0, recordsWritten = 0, blocksWritten = 0;
        uint64_t rawBytes = 0, storedBytes = 0;
        uint64_t waitCount = 0, waitMicros = 0, maxWaitMicros = 0;
        size_t pendingRecords = 0, pendingBytes = 0;
        size_t peakRecords = 0, peakBytes = 0;
    };
    // Fault/slow-storage injection for tests, invoked on the writer thread.
    using BeforeWrite = std::function<void()>;

  private:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t FILTER_BITS = 1 << 23;
    static constexpr size_t RECENT_KEYS = 256;
    static constexpr size_t MAX_RECORD = 100000;
    struct CloseDB
    {
        void
        operator()(sqlite_api::sqlite3* db) const
        {
            sqlite_api::sqlite3_close_v2(db);
        }
    };
    struct Finalize
    {
        void
        operator()(sqlite_api::sqlite3_stmt* s) const
        {
            sqlite_api::sqlite3_finalize(s);
        }
    };
    using DB = std::unique_ptr<sqlite_api::sqlite3, CloseDB>;
    using Statement = std::unique_ptr<sqlite_api::sqlite3_stmt, Finalize>;
    Options mOptions;
    BeforeWrite mBeforeWrite;
    DB mWriter, mReader;
    std::mutex mMutex;
    std::condition_variable mChanged;
    std::map<std::string, std::shared_ptr<std::string const>> mPending;
    std::deque<std::string> mQueue;
    size_t mQueuedBlocks = 0;
    Clock::time_point mOldest;
    std::exception_ptr mFailure;
    bool mStopping = false, mFlush = false;
    Stats mStats;
    std::thread mWorker;
    std::vector<uint64_t> mFilter = std::vector<uint64_t>(FILTER_BITS / 64);
    std::set<std::string> mRecent;
    std::deque<std::string> mRecentOrder;

    static void
    require(sqlite_api::sqlite3* db, int rc)
    {
        if (rc != SQLITE_OK)
            throw std::runtime_error(std::string("Shabdiz archive: ") +
                                     sqlite_api::sqlite3_errmsg(db));
    }
    static DB
    open(std::filesystem::path const& path, int flags)
    {
        sqlite_api::sqlite3* raw = nullptr;
        int rc = sqlite_api::sqlite3_open_v2(path.string().c_str(), &raw, flags,
                                             nullptr);
        DB db(raw);
        require(raw, rc);
        require(raw, sqlite_api::sqlite3_busy_timeout(raw, 5000));
        return db;
    }
    static void
    sql(sqlite_api::sqlite3* db, char const* command)
    {
        require(db, sqlite_api::sqlite3_exec(db, command, nullptr, nullptr,
                                             nullptr));
    }
    static Statement
    prepare(sqlite_api::sqlite3* db, char const* command)
    {
        sqlite_api::sqlite3_stmt* raw = nullptr;
        int rc = sqlite_api::sqlite3_prepare_v2(db, command, -1, &raw, nullptr);
        Statement s(raw);
        require(db, rc);
        return s;
    }
    static void
    bind(sqlite_api::sqlite3* db, sqlite_api::sqlite3_stmt* s, int column,
         std::string const& bytes)
    {
        require(db,
                sqlite_api::sqlite3_bind_blob(
                    s, column, bytes.data(), static_cast<int>(bytes.size()),
                    reinterpret_cast<sqlite_api::sqlite3_destructor_type>(-1)));
    }
    static bool
    isBlock(std::string const& key)
    {
        return key.compare(0, 2, "b-") == 0;
    }
    // The production keys are a kind and a SHA-256 hash: store 33 bytes rather
    // than 66 hex characters. Tag arbitrary test keys separately to avoid
    // aliases.
    static std::string
    packedKey(std::string const& key)
    {
        if (key.size() == 66 && key[1] == '-' &&
            (key[0] == 'b' || key[0] == 'e'))
        {
            std::string result(1, key[0] == 'b' ? '\1' : '\2');
            auto nibble = [](char c) -> int {
                return c >= '0' && c <= '9'   ? c - '0'
                       : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                              : -1;
            };
            for (size_t i = 2; i < key.size(); i += 2)
            {
                int a = nibble(key[i]), b = nibble(key[i + 1]);
                if (a < 0 || b < 0)
                    return std::string(1, '\0') + key;
                result.push_back(static_cast<char>(16 * a + b));
            }
            return result;
        }
        return std::string(1, '\0') + key;
    }
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
    bool
    fullBatch() const
    {
        return mQueuedBlocks >= mOptions.batchBlocks ||
               mQueue.size() >= 2 * mOptions.batchBlocks;
    }
    void
    run()
    {
        try
        {
            auto insert = prepare(
                mWriter.get(),
                "INSERT INTO records(key, codec, size, data) VALUES(?,?,?,?)");
            for (;;)
            {
                std::vector<
                    std::pair<std::string, std::shared_ptr<std::string const>>>
                    batch;
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mChanged.wait(lock,
                                  [&] { return mStopping || !mQueue.empty(); });
                    if (mQueue.empty())
                        return;
                    mChanged.wait_until(lock, mOldest + mOptions.maxDelay, [&] {
                        return mStopping || mFlush || fullBatch();
                    });
                    size_t blocks = 0;
                    while (!mQueue.empty() && blocks < mOptions.batchBlocks &&
                           batch.size() < 2 * mOptions.batchBlocks)
                    {
                        auto key = std::move(mQueue.front());
                        mQueue.pop_front();
                        if (isBlock(key))
                        {
                            ++blocks;
                            --mQueuedBlocks;
                        }
                        batch.emplace_back(key, mPending.at(key));
                    }
                }
                if (mBeforeWrite)
                    mBeforeWrite();
                uint64_t rawBytes = 0, storedBytes = 0, blocks = 0;
                sql(mWriter.get(), "BEGIN");
                for (auto const& item : batch)
                {
                    auto const& data = *item.second;
                    std::string compressed;
                    bool useCompression = false;
                    if (!data.empty())
                    {
                        uLongf size = compressBound(data.size());
                        compressed.resize(size);
                        int rc = compress2(
                            reinterpret_cast<Bytef*>(&compressed[0]), &size,
                            reinterpret_cast<Bytef const*>(data.data()),
                            data.size(), Z_BEST_SPEED);
                        if (rc != Z_OK)
                            throw std::runtime_error(
                                "Shabdiz archive compression failed");
                        compressed.resize(size);
                        useCompression = compressed.size() < data.size();
                    }
                    auto const& encoded = useCompression ? compressed : data;
                    auto key = packedKey(item.first);
                    require(mWriter.get(),
                            sqlite_api::sqlite3_reset(insert.get()));
                    bind(mWriter.get(), insert.get(), 1, key);
                    require(mWriter.get(),
                            sqlite_api::sqlite3_bind_int(insert.get(), 2,
                                                         useCompression));
                    require(mWriter.get(), sqlite_api::sqlite3_bind_int(
                                               insert.get(), 3, data.size()));
                    bind(mWriter.get(), insert.get(), 4, encoded);
                    int rc = sqlite_api::sqlite3_step(insert.get());
                    if (rc != SQLITE_DONE)
                        require(mWriter.get(), rc);
                    rawBytes += data.size();
                    storedBytes += encoded.size();
                    blocks += isBlock(item.first);
                }
                sql(mWriter.get(), "COMMIT");
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    for (auto const& item : batch)
                    {
                        mStats.pendingBytes -=
                            item.first.size() + item.second->size();
                        mPending.erase(item.first);
                    }
                    mStats.pendingRecords = mPending.size();
                    ++mStats.transactions;
                    mStats.recordsWritten += batch.size();
                    mStats.blocksWritten += blocks;
                    mStats.rawBytes += rawBytes;
                    mStats.storedBytes += storedBytes;
                    if (mPending.empty())
                        mFlush = false;
                }
                mChanged.notify_all();
            }
        }
        catch (...)
        {
            sqlite_api::sqlite3_exec(mWriter.get(), "ROLLBACK", nullptr,
                                     nullptr, nullptr);
            std::lock_guard<std::mutex> lock(mMutex);
            mFailure = std::current_exception();
            mChanged.notify_all();
        }
    }

  public:
    explicit ShabdizArchive(std::filesystem::path root)
        : ShabdizArchive(std::move(root), Options{})
    {
    }
    ShabdizArchive(std::filesystem::path root, Options options,
                   BeforeWrite beforeWrite = {})
        : mOptions(options), mBeforeWrite(std::move(beforeWrite))
    {
        if (!options.batchBlocks || options.batchBlocks > SIZE_MAX / 2 ||
            !options.maxPendingRecords || !options.maxPendingBytes ||
            options.maxDelay.count() <= 0 || std::filesystem::exists(root))
            throw std::invalid_argument(
                "Shabdiz archive requires a new directory and positive limits");
        std::filesystem::create_directories(root);
        auto file = root / "archive.sqlite";
        mWriter = open(file, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_NOMUTEX);
        sql(mWriter.get(),
            "PRAGMA journal_mode=WAL; PRAGMA synchronous=OFF;"
            "PRAGMA cache_size=-2048; PRAGMA mmap_size=0;"
            "PRAGMA wal_autocheckpoint=1024; PRAGMA "
            "journal_size_limit=16777216;"
            "CREATE TABLE records(key BLOB PRIMARY KEY, codec INTEGER NOT NULL,"
            "size INTEGER NOT NULL, data BLOB NOT NULL);");
        mReader = open(file, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX);
        sql(mReader.get(), "PRAGMA cache_size=-256; PRAGMA mmap_size=0;");
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
    Stats
    stats()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        checkLocked();
        return mStats;
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
        auto s = prepare(mReader.get(), "SELECT 1 FROM records WHERE key=?");
        bind(mReader.get(), s.get(), 1, packedKey(key));
        int rc = sqlite_api::sqlite3_step(s.get());
        if (rc != SQLITE_ROW && rc != SQLITE_DONE)
            require(mReader.get(), rc);
        return rc == SQLITE_ROW;
    }
    void
    put(std::string const& key, std::string data)
    {
        size_t bytes = key.size() + data.size();
        if (data.size() > MAX_RECORD || key.size() > 1024 ||
            bytes > mOptions.maxPendingBytes)
            throw std::invalid_argument("Oversized Shabdiz archive record");
        if (contains(key))
            return;
        auto value = std::make_shared<std::string const>(std::move(data));
        {
            std::unique_lock<std::mutex> lock(mMutex);
            auto hasSpace = [&] {
                return mPending.size() < mOptions.maxPendingRecords &&
                       bytes <= mOptions.maxPendingBytes - mStats.pendingBytes;
            };
            if (!hasSpace())
            {
                auto start = Clock::now();
                mFlush = true; // capacity pressure must flush partial batches
                mChanged.notify_all();
                mChanged.wait(lock, [&] { return mFailure || hasSpace(); });
                auto waited =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - start)
                        .count();
                ++mStats.waitCount;
                mStats.waitMicros += waited;
                mStats.maxWaitMicros =
                    std::max(mStats.maxWaitMicros, uint64_t(waited));
            }
            checkLocked();
            mPending.emplace(key, std::move(value));
            if (mQueue.empty())
                mOldest = Clock::now();
            mQueue.push_back(key);
            mQueuedBlocks += isBlock(key);
            mStats.pendingRecords = mPending.size();
            mStats.pendingBytes += bytes;
            mStats.peakRecords =
                std::max(mStats.peakRecords, mStats.pendingRecords);
            mStats.peakBytes = std::max(mStats.peakBytes, mStats.pendingBytes);
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
        auto s = prepare(mReader.get(),
                         "SELECT codec,size,data FROM records WHERE key=?");
        bind(mReader.get(), s.get(), 1, packedKey(key));
        int rc = sqlite_api::sqlite3_step(s.get());
        if (rc == SQLITE_DONE)
            return {};
        if (rc != SQLITE_ROW)
            require(mReader.get(), rc);
        int codec = sqlite_api::sqlite3_column_int(s.get(), 0);
        int size = sqlite_api::sqlite3_column_int(s.get(), 1);
        int stored = sqlite_api::sqlite3_column_bytes(s.get(), 2);
        auto bytes = static_cast<char const*>(
            sqlite_api::sqlite3_column_blob(s.get(), 2));
        if (size < 0 || size > int(MAX_RECORD) || stored > int(MAX_RECORD))
            throw std::runtime_error("Invalid Shabdiz archive length");
        if (codec == 0 && stored == size)
            return size ? std::string(bytes, size) : std::string{};
        if (codec != 1 || size == 0)
            throw std::runtime_error("Invalid Shabdiz archive encoding");
        std::string result(size, '\0');
        uLongf length = size;
        if (uncompress(reinterpret_cast<Bytef*>(&result[0]), &length,
                       reinterpret_cast<Bytef const*>(bytes), stored) != Z_OK ||
            length != static_cast<uLongf>(size))
            throw std::runtime_error(
                "Corrupt compressed Shabdiz archive record");
        return result;
    }
    void
    flush()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mFlush = true;
        mChanged.notify_all();
        mChanged.wait(lock, [&] { return mFailure || mPending.empty(); });
        checkLocked();
        mFlush = false;
    }
};
} // namespace stellar
