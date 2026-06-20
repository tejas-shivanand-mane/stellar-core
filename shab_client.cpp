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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <endian.h>
#include <unistd.h>

#include "overlay/CustomYCSBWorkload.h"
#include "overlay/CustomProtocolTypes.h"

// ---- Config ----
static int MAX_IN_FLIGHT = 400;
static int SERVER_BATCH_SIZE_HINT = 100;
static int TOTAL_REQUESTS = 10000;

static int DURATION_SEC = 0;      // 0 = use TOTAL_REQUESTS, >0 = run for N seconds
static int SEND_INTERVAL_US = 0;  // microseconds between consecutive requests

// ---- Serialization ----
static std::string
serializeRequest(uint64_t requestId, uint64_t txnId)
{
    uint64_t ts =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    CustomTransaction txn;
    txn.txnId = txnId;
    txn.payload = generateYCSBOp();
    txn.timestamp = ts;
    txn.sender = "client";

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

// ---- Sliding window client ----
struct Client
{
    int fd;

    std::mutex              mu;
    std::condition_variable cv;

    std::map<uint64_t,
        std::chrono::steady_clock::time_point> inFlight;

    Stats stats;

    uint64_t nextRequestId = 0;
    uint64_t nextTxnId     = 0;

    int sent      = 0;
    int committed = 0;

    std::atomic<bool> done{false};

    void sendRequest()
    {
        uint64_t requestId = nextRequestId++;

        std::string data = serializeRequest(requestId, nextTxnId);
        nextTxnId += 1;

        uint32_t lenNet = htonl((uint32_t)data.size());

        if (send(fd, &lenNet, 4, MSG_NOSIGNAL) < 0 ||
            send(fd, data.c_str(), data.size(), MSG_NOSIGNAL) < 0)
        {
            // std::cerr << "Send failed\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mu);
            inFlight[requestId] = std::chrono::steady_clock::now();
        }

        sent++;
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

                stats.record(latUs);

                inFlight.erase(it);
                committed++;
            }
        }

        cv.notify_one();
    }

    void run()
    {
        auto startTime = std::chrono::steady_clock::now();

        // ACK receiver thread.
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

        // Per-second client-side stats thread.
        std::thread statsThread([this, startTime]() {
            int secIndex = 1;

            while (!done.load())
            {
                std::this_thread::sleep_until(
                    startTime + std::chrono::seconds(secIndex));

                if (done.load())
                {
                    break;
                }

                stats.reportSecondWindow(startTime, secIndex);

                secIndex++;
            }
        });

        // Send loop: fixed number of requests or duration-based.
        auto shouldKeepSending = [&]() {
            if (DURATION_SEC > 0)
            {
                auto elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - startTime).count();

                return elapsed < DURATION_SEC;
            }

            return sent < TOTAL_REQUESTS;
        };

        while (shouldKeepSending())
        {
            std::unique_lock<std::mutex> lock(mu);

            cv.wait(lock, [this]() {
                return (int)inFlight.size() < MAX_IN_FLIGHT;
            });

            lock.unlock();

            sendRequest();

            sleepBetweenRequests();
        }

        // Wait for all ACKs.
        {
            std::unique_lock<std::mutex> lock(mu);

            cv.wait(lock, [this]() {
                return inFlight.empty();
            });
        }

        done.store(true);

        // Wake the ACK receiver if it is blocked in recv(..., MSG_WAITALL).
        shutdown(fd, SHUT_RDWR);

        if (ackThread.joinable())
        {
            ackThread.join();
        }

        if (statsThread.joinable())
        {
            statsThread.join();
        }

        double elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count() / 1000.0;

        stats.report(elapsed);
        stats.reportLastWindow(60.0);
    }
};

int main(int argc, char* argv[])
{
    if (argc < 6)
    {
        fprintf(stderr,
            "Usage: client <leader_ip> <port> "
            "<max_in_flight> <total_requests> <server_batch_size_hint> "
            "[duration_sec] [send_interval_us]\n"
            "Example fixed requests: ./client 10.128.0.5 12000 500 100000 100 0 50\n"
            "Example duration run:   ./client 10.128.0.5 12000 500 100000 100 60 50\n");

        return 1;
    }

    std::string leaderIp = argv[1];

    int port = std::stoi(argv[2]);

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

    if (MAX_IN_FLIGHT < SERVER_BATCH_SIZE_HINT)
    {
        fprintf(stderr,
                "WARNING: max_in_flight=%d is smaller than server_batch_size_hint=%d.\n"
                "This can deadlock because the server may wait for a full batch before ACKing.\n"
                "Use max_in_flight >= server_batch_size_hint.\n",
                MAX_IN_FLIGHT,
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

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, leaderIp.c_str(), &addr.sin_addr) != 1)
    {
        fprintf(stderr, "Invalid IP address: %s\n", leaderIp.c_str());
        close(fd);
        return 1;
    }

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "Failed to connect to %s:%d\n",
                leaderIp.c_str(), port);

        close(fd);
        return 1;
    }

    printf("Connected to %s:%d | max_in_flight=%d "
           "total_requests=%d server_batch_size_hint=%d "
           "duration_sec=%d send_interval_us=%d\n",
           leaderIp.c_str(),
           port,
           MAX_IN_FLIGHT,
           TOTAL_REQUESTS,
           SERVER_BATCH_SIZE_HINT,
           DURATION_SEC,
           SEND_INTERVAL_US);

    Client client;
    client.fd = fd;
    client.run();

    close(fd);

    return 0;
}