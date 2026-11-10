#include "overlay/CustomProtocolTypes.h"
#include "overlay/ShabdizArchive.h"
#include "overlay/ShabdizBatch.h"
#include <cassert>
#include <chrono>
#include <future>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <unistd.h>

using namespace stellar;
using namespace sqlite_api;
using Clock = std::chrono::steady_clock;

bool
legacyValidate(std::string const& data, size_t count)
{
    try
    {
        if (data.empty() || data.size() > 90000)
            return false;
        auto batch = TransactionBatch::deserialize(data);
        if (batch.transactions.size() != count || batch.serialize() != data)
            return false;
        for (auto const& txn : batch.transactions)
        {
            std::istringstream in(txn.payload);
            std::string op, key, value, extra;
            if (!(in >> op >> key))
                return false;
            if (op != "READ" &&
                ((op != "UPDATE" && op != "RMW") || !(in >> value)))
                return false;
            if (in >> extra)
                return false;
        }
        return true;
    }
    catch (std::exception const&)
    {
        return false;
    }
}

std::string
batchData()
{
    TransactionBatch batch;
    for (size_t i = 0; i < 100; ++i)
        batch.transactions.push_back({i,
                                      (i % 3 == 0   ? "READ key"
                                       : i % 3 == 1 ? "UPDATE key value"
                                                    : "RMW key value") +
                                          std::to_string(i),
                                      123456789 + i, "client-0"});
    return batch.serialize();
}

void
parser()
{
    auto data = batchData();
    auto parsed = parseShabdizBatch(data, 100);
    assert(parsed && parsed->size() == 100 && !(*parsed)[0].write);
    assert((*parsed)[1].write && (*parsed)[1].key == "key" &&
           (*parsed)[1].value == "value1");
    assert((*parsed)[2].write && (*parsed)[2].value == "value2");
    for (auto const& s :
         {"0:READ k:0:", "18446744073709551615:RMW k v:18446744073709551615:c",
          "1: UPDATE\tk\tv \n:5:c", "01:READ k:0:c", "-1:READ k:0:c",
          "+1:READ k:0:c", "1:READ k:18446744073709551616:c", "1:READ k:0:c|",
          "1:READ k:0:c:extra", "1:DELETE k:0:c", "1:UPDATE k:0:c",
          "1:READ k v:0:c", "1:READ:0:c", "||"})
        assert(bool(parseShabdizBatch(s, 1)) == legacyValidate(s, 1));
    std::mt19937 rng(42);
    // Differential mutations cover malformed framing, numeric fields and ops.
    for (int i = 0; i < 10000; ++i)
    {
        auto s = data;
        size_t pos = rng() % s.size();
        if (i % 3 == 0)
            s.erase(pos, 1);
        else if (i % 3 == 1)
            s.insert(pos, 1, char(rng() % 128));
        else
            s[pos] = char(rng() % 128);
        assert(bool(parseShabdizBatch(s, 100)) == legacyValidate(s, 100));
    }
}

std::string
archiveKey(char kind, unsigned i)
{
    std::ostringstream out;
    out << kind << '-' << std::hex << std::setfill('0') << std::setw(64) << i;
    return out.str();
}

size_t
fileCount(std::filesystem::path const& root)
{
    size_t count = 0;
    for (auto const& file : std::filesystem::directory_iterator(root))
        count += file.is_regular_file();
    return count;
}

void
archive(std::filesystem::path const& root)
{
    using Options = ShabdizArchive::Options;
    std::promise<void> writing, release;
    auto gate = release.get_future().share();
    auto main = std::this_thread::get_id();
    {
        Options options;
        options.batchBlocks = 1;
        options.maxPendingRecords = 1;
        bool first = true;
        ShabdizArchive store(root / "pending", options, [&] {
            assert(std::this_thread::get_id() != main);
            if (first)
            {
                first = false;
                writing.set_value();
                gate.wait();
            }
        });
        store.put("b-first", "block bytes");
        writing.get_future().wait();
        // Data is fetchable before its SQLite transaction even starts.
        assert(store.contains("b-first") &&
               store.get("b-first") == "block bytes");
        auto next =
            std::async(std::launch::async, [&] { store.put("e-batch", {}); });
        assert(next.wait_for(std::chrono::milliseconds(30)) ==
               std::future_status::timeout);
        release.set_value();
        next.get();
        store.flush();
        assert(store.contains("e-batch") && store.get("e-batch") == "");
        assert(store.get("b-first") == "block bytes");
        auto stats = store.stats();
        assert(stats.waitCount == 1 && stats.waitMicros >= 20000);
        assert(stats.peakRecords == 1 && stats.pendingRecords == 0);
    }
    uint64_t rawBytes = 0;
    {
        // Full 1000-block batches, actual production hash-key layout, and
        // lossless payload retrieval after pending/recent-cache eviction.
        Options options;
        options.maxDelay = std::chrono::minutes(1);
        ShabdizArchive store(root / "packed", options);
        auto data = batchData();
        for (unsigned i = 0; i < 3000; ++i)
        {
            store.put(archiveKey('b', i), data + std::to_string(i));
            store.put(archiveKey('e', i), {});
        }
        store.flush();
        auto stats = store.stats();
        assert(stats.blocksWritten == 3000 && stats.recordsWritten == 6000);
        assert(stats.transactions >= 3 && stats.transactions <= 6);
        assert(stats.peakBytes <= options.maxPendingBytes);
        assert(stats.peakRecords <= options.maxPendingRecords);
        rawBytes = stats.rawBytes;
        assert(stats.storedBytes < rawBytes / 2);
        for (unsigned i = 0; i < 3000; ++i)
        {
            auto key = archiveKey('b', i);
            assert(store.contains(key) &&
                   store.get(key) == data + std::to_string(i));
            assert(store.contains(archiveKey('e', i)) &&
                   store.get(archiveKey('e', i)) == "");
            store.put(key, "must not overwrite");
        }
        for (unsigned i = 3000; i < 12000; ++i)
            assert(!store.contains(archiveKey('b', i)));
        assert(!store.get("not-present"));
        assert(store.stats().recordsWritten == 6000);
        assert(fileCount(root / "packed") <= 3);
        std::cout << "Packed fixture: 3000 blocks + 3000 markers, transactions="
                  << stats.transactions << " raw_payload_bytes=" << rawBytes
                  << " compressed_payload_bytes=" << stats.storedBytes << "\n";
    }
    uint64_t diskBytes = 0;
    for (auto const& f : std::filesystem::directory_iterator(root / "packed"))
        diskBytes += f.file_size();
    assert(fileCount(root / "packed") == 1 && diskBytes < rawBytes / 2);
    std::cout << "Closed archive: files=1 database_bytes=" << diskBytes << "\n";
    {
        Options options;
        options.maxDelay = std::chrono::milliseconds(20);
        ShabdizArchive store(root / "timer", options);
        std::mt19937 rng(7);
        std::string noise(90000, '\0');
        for (auto& c : noise)
            c = static_cast<char>(rng());
        store.put("b-noise", noise);
        // Partial batches flush on age, without explicit flush or more work.
        auto end = Clock::now() + std::chrono::seconds(3);
        while (!store.stats().recordsWritten && Clock::now() < end)
            std::this_thread::yield();
        assert(store.stats().recordsWritten == 1);
        assert(store.get("b-noise") == noise);
        assert(store.stats().rawBytes ==
               store.stats().storedBytes); // raw fallback
        // Different key encodings cannot alias compact hash keys.
        store.put(archiveKey('e', 42), {});
        store.put(std::string(1, '\2') + std::string(32, '\0'), "binary key");
        store.flush();
        assert(store.get(std::string(1, '\2') + std::string(32, '\0')) ==
               "binary key");
    }
    {
        Options options;
        options.maxPendingBytes = 100;
        options.maxDelay = std::chrono::minutes(1);
        ShabdizArchive store(root / "byte-limit", options);
        store.put("b-one", std::string(80, 'x'));
        // Capacity must wake the writer even below the 1000-block threshold.
        store.put("b-two", std::string(80, 'y'));
        store.flush();
        assert(store.stats().peakBytes <= 100 && store.stats().waitCount >= 1);
        assert(store.get("b-one") == std::string(80, 'x'));
        bool rejected = false;
        try
        {
            store.put("b-too-large", std::string(100, 'z'));
        }
        catch (std::invalid_argument const&)
        {
            rejected = true;
        }
        assert(rejected);
    }
    {
        Options options;
        options.maxDelay = std::chrono::minutes(1);
        ShabdizArchive store(root / "shutdown", options);
        store.put("b-last", "partial final batch");
        // Destructor must drain a partial batch immediately.
    }
    sqlite3* db = nullptr;
    assert(sqlite3_open_v2((root / "shutdown/archive.sqlite").c_str(), &db,
                           SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM records", -1, &stmt,
                              nullptr) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW &&
           sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    {
        ShabdizArchive store(root / "failure", Options{}, [] {
            throw std::runtime_error("injected disk failure");
        });
        store.put("b-block", "bytes");
        bool failed = false;
        try
        {
            store.flush();
        }
        catch (std::runtime_error const&)
        {
            failed = true;
        }
        assert(failed); // never silently discard ancestry
        failed = false;
        try
        {
            store.get("b-block");
        }
        catch (std::runtime_error const&)
        {
            failed = true;
        }
        assert(failed);
    }
}

void
memoryArchive(std::filesystem::path const& root)
{
    ShabdizArchive::Options options;
    options.memoryOnly = true;
    auto path = root / "memory-only";
    {
        ShabdizArchive store(path, options, [] {
            throw std::runtime_error("Memory-only storage must not start a writer");
        });
        auto data = batchData();
        // Exceed both the recent-key cache and disk pending-queue capacity.
        // Old blocks and execution markers must remain fetchable in RAM.
        for (unsigned i = 0; i < 5000; ++i)
        {
            store.put(archiveKey('b', i), data + std::to_string(i));
            store.put(archiveKey('e', i), {});
        }
        for (unsigned i = 0; i < 5000; ++i)
        {
            auto key = archiveKey('b', i);
            store.put(key, "must not overwrite");
            assert(store.contains(key));
            assert(store.get(key) == data + std::to_string(i));
            assert(store.contains(archiveKey('e', i)));
            assert(store.get(archiveKey('e', i)) == "");
        }
        store.put(std::string(1, '\2') + std::string(32, '\0'), "binary key");
        assert(store.get(std::string(1, '\2') + std::string(32, '\0')) ==
               "binary key");
        assert(!store.contains("absent") && !store.get("absent"));
        store.flush();
        store.check();
        assert(store.stats().transactions == 0);
        assert(store.stats().recordsWritten == 0);
        assert(store.stats().pendingRecords == 0);
        assert(!std::filesystem::exists(path));
    }
    assert(!std::filesystem::exists(path));
    std::cout << "Memory-only archive: 10001 records, no files or writer\n";
}

void
benchmark()
{
    auto data = batchData();
    constexpr int repeats = 10000;
    size_t count = 0;
    auto begin = Clock::now();
    for (int i = 0; i < repeats; ++i)
        count += legacyValidate(data, 100);
    auto middle = Clock::now();
    for (int i = 0; i < repeats; ++i)
        count += bool(parseShabdizBatch(data, 100));
    auto end = Clock::now();
    assert(count == 2 * repeats);
    std::cout
        << "Parser only, 100 txns/batch, " << repeats << " batches: previous="
        << std::chrono::duration<double, std::milli>(middle - begin).count()
        << " ms optimized="
        << std::chrono::duration<double, std::milli>(end - middle).count()
        << " ms\n";
}

int
main(int argc, char** argv)
{
    auto root = std::filesystem::temp_directory_path() /
                ("shabdiz-runtime-test-" + std::to_string(getpid()) + "-" +
                 std::to_string(Clock::now().time_since_epoch().count()));
    parser();
    memoryArchive(root);
    archive(root);
    std::filesystem::remove_all(root);
    std::cout << "Shabdiz parser equivalence, pending fetch, replay dedup, "
                 "packed/compressed archive, byte/count backpressure, partial "
                 "flush and error propagation passed\n";
    if (argc == 2 && std::string(argv[1]) == "--bench")
        benchmark();
}

