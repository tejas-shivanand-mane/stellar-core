#include <iostream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <atomic>
#include <memory>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <endian.h>
#include <unistd.h>

#include "overlay/CustomYCSBWorkload.h"
#include "overlay/CustomProtocolTypes.h"

// ---- Config ----
static int MAX_IN_FLIGHT = 400;          // per client thread
static int SERVER_BATCH_SIZE_HINT = 100;
static int TOTAL_REQUESTS = 10000;       // total across all client threads
static int DURATION_SEC = 0;             // 0 = use TOTAL_REQUESTS, >0 = run for N seconds
static int SEND_INTERVAL_US = 0;         // per-thread sleep between requests
static int CLIENT_THREADS = 1;

// If generateYCSBOp() uses shared/global RNG internally, protect it.
// If you later confirm it is thread-safe and this becomes a bottleneck,
// you can remove this mutex.
static std::mutex g_workloadMutex;

// ---- Serialization ----
static std::string
serializeRequest(uint64_t requestId, uint64_t txnId, int workerId)
{
    uint64_t ts =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    CustomTransaction txn;
    txn.txnId = txnId;

    {
        std::lock_guard<std::mutex> lock(g_workloadMutex);
        txn.payload = generateYCSBOp();
    }

    txn.timestamp = ts;
    txn.sender = "client-" + std::to_string(workerId);

    return std::to_string(requestId) + "|" + txn.serialize();
}

static void
sleepBetweenRequests()
{
    if (SEND_INTERVAL_US > 0)
    {
        std::this_thread::sleep_for(
            std::chrono::microseconds(SEND_INTERVAL_US));
    }
}

static bool
sendAll(int fd, const void* data, size_t len)
{
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);

        if (n <= 0)
        {
            return false;
        }

        sent += (size_t)n;
    }

    return true;
}

// ---- Stats ----
struct Stats
{
    struct Sample
    {
        std::chrono::steady_clock::time_point ackTime;
        int64_t latUs;
    };

    std::mutex mu;
    std::vector<Sample> samples;

    void record(int64_t latencyUs)
    {
        std::lock_guard<std::mutex> lock(mu);
        samples.push_back({std::chrono::steady_clock::now(), latencyUs});
    }

    void reportSecondWindow(
        std::chrono::steady_clock::time_point runStart,
        int secIndex)
    {
        auto windowStart = runStart + std::chrono::seconds(secIndex - 1);
        auto windowEnd   = runStart + std::chrono::seconds(secIndex);

        std::vector<int64_t> windowLatUs;

        {
            std::lock_guard<std::mutex> lock(mu);

            for (auto const& s : samples)
            {
                if (s.ackTime >= windowStart && s.ackTime < windowEnd)
                {
                    windowLatUs.push_back(s.latUs);
                }
            }
        }

        if (windowLatUs.empty())
        {
            printf("[CLIENT_SEC] sec=%d requests=0 throughput=0 "
                   "avg_latency_ms=nan p50_latency_ms=nan "
                   "p99_latency_ms=nan p999_latency_ms=nan\n",
                   secIndex);
            fflush(stdout);
            return;
        }

        std::sort(windowLatUs.begin(), windowLatUs.end());

        int n = (int)windowLatUs.size();

        double meanMs =
            std::accumulate(windowLatUs.begin(), windowLatUs.end(), 0LL)
            / (double)n / 1000.0;

        double p50Ms =
            windowLatUs[n / 2] / 1000.0;

        double p99Ms =
            windowLatUs[std::min((int)(n * 0.99), n - 1)] / 1000.0;

        double p999Ms =
            windowLatUs[std::min((int)(n * 0.999), n - 1)] / 1000.0;

        double throughput = (double)n; // one-second window

        printf("[CLIENT_SEC] sec=%d requests=%d throughput=%.0f "
               "avg_latency_ms=%.3f p50_latency_ms=%.3f "
               "p99_latency_ms=%.3f p999_latency_ms=%.3f\n",
               secIndex,
               n,
               throughput,
               meanMs,
               p50Ms,
               p99Ms,
               p999Ms);

        fflush(stdout);
    }

    void report(double elapsed)
    {
        std::vector<int64_t> latUs;

        {
            std::lock_guard<std::mutex> lock(mu);

            if (samples.empty())
            {
                printf("=== Overall Results ===\n");
                printf("No ACKed requests.\n");
                return;
            }

            latUs.reserve(samples.size());

            for (auto const& s : samples)
            {
                latUs.push_back(s.latUs);
            }
        }

        std::sort(latUs.begin(), latUs.end());

        int n = (int)latUs.size();

        double mean =
            std::accumulate(latUs.begin(), latUs.end(), 0LL)
            / (double)n / 1000.0;

        double p50 =
            latUs[n / 2] / 1000.0;

        double p99 =
            latUs[std::min((int)(n * 0.99), n - 1)] / 1000.0;

        double p999 =
            latUs[std::min((int)(n * 0.999), n - 1)] / 1000.0;

        double tput = (double)n / std::max(elapsed, 1e-9);

        printf("=== Overall Results ===\n");
        printf("Elapsed:        %.2f s\n", elapsed);
        printf("Throughput:     %.0f ops/s\n", tput);
        printf("Requests:       %d\n", n);
        printf("Latency mean:   %.2f ms\n", mean);
        printf("Latency p50:    %.2f ms\n", p50);
        printf("Latency p99:    %.2f ms\n", p99);
        printf("Latency p99.9:  %.2f ms\n", p999);
    }

    void reportLastWindow(double windowSec = 60.0)
    {
        std::vector<Sample> localSamples;

        {
            std::lock_guard<std::mutex> lock(mu);

            if (samples.empty())
            {
                printf("=== Last %.0f Seconds Results ===\n", windowSec);
                printf("No ACKed requests.\n");
                return;
            }

            localSamples = samples;
        }

        auto endTime = localSamples.back().ackTime;
        auto startTime = endTime - std::chrono::milliseconds((int)(windowSec * 1000));

        std::vector<int64_t> windowLatUs;

        for (auto const& s : localSamples)
        {
            if (s.ackTime >= startTime)
            {
                windowLatUs.push_back(s.latUs);
            }
        }

        if (windowLatUs.empty())
        {
            printf("=== Last %.0f Seconds Results ===\n", windowSec);
            printf("No committed requests in this window.\n");
            return;
        }

        std::sort(windowLatUs.begin(), windowLatUs.end());

        int n = (int)windowLatUs.size();

        double mean =
            std::accumulate(windowLatUs.begin(), windowLatUs.end(), 0LL)
            / (double)n / 1000.0;

        double p50 =
            windowLatUs[n / 2] / 1000.0;

        double p99 =
            windowLatUs[std::min((int)(n * 0.99), n - 1)] / 1000.0;

        double p999 =
            windowLatUs[std::min((int)(n * 0.999), n - 1)] / 1000.0;

        double actualWindowSec = windowSec;

        if (localSamples.size() > 1)
        {
            auto first = localSamples.front().ackTime;

            double totalObservedSec =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - first).count() / 1000.0;

            actualWindowSec = std::min(windowSec, std::max(0.001, totalObservedSec));
        }

        double tput = (double)n / std::max(actualWindowSec, 1e-9);

        printf("=== Last %.0f Seconds Results ===\n", actualWindowSec);
        printf("Throughput:     %.0f ops/s\n", tput);
        printf("Latency mean:   %.2f ms\n", mean);
        printf("Latency p50:    %.2f ms\n", p50);
        printf("Latency p99:    %.2f ms\n", p99);
        printf("Latency p99.9:  %.2f ms\n", p999);
        printf("Requests:       %d\n", n);
    }
};

struct SharedState
{
    std::string leaderIp;
    int port;

    std::chrono::steady_clock::time_point startTime;

    Stats stats;

    std::atomic<uint64_t> nextRequestId{0};
    std::atomic<uint64_t> totalSent{0};
    std::atomic<uint64_t> totalAcked{0};

    std::atomic<bool> stopRequested{false};
};

struct ClientWorker
{
    int workerId;
    int fd = -1;

    SharedState* shared = nullptr;

    std::mutex mu;
    std::condition_variable cv;

    std::map<uint64_t, std::chrono::steady_clock::time_point> inFlight;

    std::atomic<bool> done{false};

    bool connectToLeader()
    {
        fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0)
        {
            perror("socket");
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(shared->port);

        if (inet_pton(AF_INET, shared->leaderIp.c_str(), &addr.sin_addr) != 1)
        {
            fprintf(stderr, "Worker %d: invalid IP address: %s\n",
                    workerId, shared->leaderIp.c_str());
            close(fd);
            fd = -1;
            return false;
        }

        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
        {
            fprintf(stderr, "Worker %d: failed to connect to %s:%d\n",
                    workerId, shared->leaderIp.c_str(), shared->port);
            close(fd);
            fd = -1;
            return false;
        }

        return true;
    }

    bool reserveRequest(uint64_t& requestId)
    {
        if (DURATION_SEC > 0)
        {
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - shared->startTime).count();

            if (elapsed >= DURATION_SEC)
            {
                return false;
            }

            requestId = shared->nextRequestId.fetch_add(1);
            return true;
        }

        requestId = shared->nextRequestId.fetch_add(1);

        if (requestId >= (uint64_t)TOTAL_REQUESTS)
        {
            return false;
        }

        return true;
    }

    bool sendRequest(uint64_t requestId)
    {
        uint64_t txnId = requestId;

        std::string data = serializeRequest(requestId, txnId, workerId);
        uint32_t lenNet = htonl((uint32_t)data.size());

        auto sendTime = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mu);
            inFlight[requestId] = sendTime;
        }

        bool ok =
            sendAll(fd, &lenNet, 4) &&
            sendAll(fd, data.data(), data.size());

        if (!ok)
        {
            {
                std::lock_guard<std::mutex> lock(mu);
                inFlight.erase(requestId);
            }

            shared->stopRequested.store(true);
            cv.notify_one();
            return false;
        }

        shared->totalSent.fetch_add(1);

        return true;
    }

    void onAck(uint64_t requestId)
    {
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mu);

            auto it = inFlight.find(requestId);

            if (it != inFlight.end())
            {
                int64_t latUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - it->second).count();

                shared->stats.record(latUs);
                shared->totalAcked.fetch_add(1);

                inFlight.erase(it);
            }
        }

        cv.notify_one();
    }

    void run()
    {
        if (!connectToLeader())
        {
            shared->stopRequested.store(true);
            return;
        }

        std::thread ackThread([this]() {
            while (!done.load())
            {
                uint64_t ackNet = 0;

                int n = recv(fd, &ackNet, 8, MSG_WAITALL);

                if (n <= 0)
                {
                    break;
                }

                onAck(be64toh(ackNet));
            }
        });

        while (!shared->stopRequested.load())
        {
            uint64_t requestId = 0;

            if (!reserveRequest(requestId))
            {
                break;
            }

            {
                std::unique_lock<std::mutex> lock(mu);

                cv.wait(lock, [this]() {
                    return shared->stopRequested.load() ||
                           (int)inFlight.size() < MAX_IN_FLIGHT;
                });

                if (shared->stopRequested.load())
                {
                    break;
                }
            }

            if (!sendRequest(requestId))
            {
                break;
            }

            sleepBetweenRequests();
        }

        // Wait for all ACKs for this worker.
        {
            std::unique_lock<std::mutex> lock(mu);

            cv.wait(lock, [this]() {
                return inFlight.empty() || shared->stopRequested.load();
            });
        }

        done.store(true);

        if (fd >= 0)
        {
            shutdown(fd, SHUT_RDWR);
        }

        if (ackThread.joinable())
        {
            ackThread.join();
        }

        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }
    }
};

int main(int argc, char* argv[])
{
    if (argc < 6)
    {
        fprintf(stderr,
            "Usage: client <leader_ip> <port> "
            "<max_in_flight_per_thread> <total_requests> <server_batch_size_hint> "
            "[duration_sec] [send_interval_us] [client_threads]\n"
            "Example fixed requests: ./client 10.128.0.5 12000 1000 100000 100 0 0 8\n"
            "Example duration run:   ./client 10.128.0.5 12000 1000 100000 100 60 0 8\n");

        return 1;
    }

    SharedState shared;

    shared.leaderIp = argv[1];
    shared.port     = std::stoi(argv[2]);

    MAX_IN_FLIGHT          = std::stoi(argv[3]);
    TOTAL_REQUESTS         = std::stoi(argv[4]);
    SERVER_BATCH_SIZE_HINT = std::stoi(argv[5]);

    if (argc >= 7)
    {
        DURATION_SEC = std::stoi(argv[6]);
    }

    if (argc >= 8)
    {
        SEND_INTERVAL_US = std::stoi(argv[7]);
    }

    if (argc >= 9)
    {
        CLIENT_THREADS = std::stoi(argv[8]);
    }

    if (CLIENT_THREADS <= 0)
    {
        fprintf(stderr, "ERROR: client_threads must be > 0.\n");
        return 1;
    }

    int totalMaxInFlight = MAX_IN_FLIGHT * CLIENT_THREADS;

    if (totalMaxInFlight < SERVER_BATCH_SIZE_HINT)
    {
        fprintf(stderr,
                "WARNING: aggregate max_in_flight=%d is smaller than "
                "server_batch_size_hint=%d.\n"
                "This can deadlock because the server may wait for a full batch before ACKing.\n",
                totalMaxInFlight,
                SERVER_BATCH_SIZE_HINT);
    }

    if (DURATION_SEC == 0 &&
        SERVER_BATCH_SIZE_HINT > 0 &&
        TOTAL_REQUESTS % SERVER_BATCH_SIZE_HINT != 0)
    {
        fprintf(stderr,
                "WARNING: total_requests=%d is not a multiple of server_batch_size_hint=%d.\n"
                "The final partial server batch may not be ACKed if the server only proposes full batches.\n",
                TOTAL_REQUESTS,
                SERVER_BATCH_SIZE_HINT);
    }

    printf("Connected workload config | leader=%s:%d "
           "client_threads=%d max_in_flight_per_thread=%d "
           "aggregate_max_in_flight=%d total_requests=%d "
           "server_batch_size_hint=%d duration_sec=%d send_interval_us=%d\n",
           shared.leaderIp.c_str(),
           shared.port,
           CLIENT_THREADS,
           MAX_IN_FLIGHT,
           totalMaxInFlight,
           TOTAL_REQUESTS,
           SERVER_BATCH_SIZE_HINT,
           DURATION_SEC,
           SEND_INTERVAL_US);

    shared.startTime = std::chrono::steady_clock::now();

    std::thread statsThread([&shared]() {
        int secIndex = 1;

        while (!shared.stopRequested.load())
        {
            std::this_thread::sleep_until(
                shared.startTime + std::chrono::seconds(secIndex));

            if (shared.stopRequested.load())
            {
                break;
            }

            shared.stats.reportSecondWindow(shared.startTime, secIndex);
            secIndex++;
        }
    });

    std::vector<std::unique_ptr<ClientWorker>> workers;
    std::vector<std::thread> workerThreads;

    workers.reserve(CLIENT_THREADS);
    workerThreads.reserve(CLIENT_THREADS);

    for (int i = 0; i < CLIENT_THREADS; i++)
    {
        auto worker = std::make_unique<ClientWorker>();
        worker->workerId = i;
        worker->shared = &shared;

        workers.push_back(std::move(worker));
    }

    for (int i = 0; i < CLIENT_THREADS; i++)
    {
        workerThreads.emplace_back([&, i]() {
            workers[i]->run();
        });
    }

    for (auto& t : workerThreads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    shared.stopRequested.store(true);

    if (statsThread.joinable())
    {
        statsThread.join();
    }

    double elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - shared.startTime).count() / 1000.0;

    printf("=== Multi-thread Client Summary ===\n");
    printf("Client threads:      %d\n", CLIENT_THREADS);
    printf("Total sent:          %lu\n", (unsigned long)shared.totalSent.load());
    printf("Total ACKed:         %lu\n", (unsigned long)shared.totalAcked.load());

    shared.stats.report(elapsed);
    shared.stats.reportLastWindow(60.0);

    return 0;
}