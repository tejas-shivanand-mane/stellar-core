#include "overlay/CustomProtocolTypes.h"
#include "overlay/ShabdizArchive.h"
#include "overlay/ShabdizBatch.h"
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <random>
#include <sstream>
#include <unistd.h>

using namespace stellar;
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

void
writeRecord(std::filesystem::path const& p, std::string const& s)
{
    std::ofstream out(p, std::ios::binary);
    out.write(s.data(), s.size());
    out.close();
    if (!out)
        throw std::runtime_error("test write failed");
}

void
archive(std::filesystem::path const& root)
{
    std::promise<void> writing, release;
    auto gate = release.get_future().share();
    auto main = std::this_thread::get_id();
    {
        ShabdizArchive store(root / "pending", 1,
                             [&](auto const& p, auto const& s) {
                                 assert(std::this_thread::get_id() != main);
                                 if (p.filename() == "b-first")
                                 {
                                     writing.set_value();
                                     gate.wait();
                                 }
                                 writeRecord(p, s);
                             });
        store.put("b-first",
                  "block bytes"); // must return while writer is blocked
        writing.get_future().wait();
        assert(!std::filesystem::exists(root / "pending/b-first"));
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
    }
    {
        ShabdizArchive store(root / "evicted", 8);
        for (int i = 0; i < 2000; ++i)
            store.put("b-" + std::to_string(i), "data" + std::to_string(i));
        store.flush();
        // Fetch/dedup still work after eviction from the bounded recent cache.
        for (int i = 0; i < 2000; ++i)
        {
            auto key = "b-" + std::to_string(i);
            assert(store.contains(key) &&
                   store.get(key) == "data" + std::to_string(i));
            store.put(key, "must not overwrite");
        }
        for (int i = 2000; i < 12000; ++i)
            assert(!store.contains("b-" + std::to_string(i)));
        assert(!store.get("not-present"));
    }
    {
        ShabdizArchive store(root / "failure", 2, [](auto const&, auto const&) {
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
        assert(
            failed); // failures must surface; never silently discard ancestry
    }
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
    archive(root);
    std::filesystem::remove_all(root);
    std::cout << "Shabdiz parser equivalence, pending fetch, replay dedup, "
                 "bounded archive and error propagation passed\n";
    if (argc == 2 && std::string(argv[1]) == "--bench")
        benchmark();
}
