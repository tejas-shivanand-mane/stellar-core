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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <endian.h>
#include <unistd.h>

#include "overlay/CustomYCSBWorkload.h"
#include "overlay/CustomProtocolTypes.h"

// ---- Config ----
static int    MAX_IN_FLIGHT = 10;
static int    BATCH_SIZE    = 100;
static int    TOTAL_BATCHES = 10000;


static int DURATION_SEC = 60; // 0 = use total_batches, >0 = run for N seconds


// ---- Serialization ----
// Format: batchId|id:payload:ts:sender|id:payload:ts:sender|...
static std::string
serializeBatch(uint64_t batchId, uint64_t startTxnId)
{
    TransactionBatch batch;

    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (int i = 0; i < BATCH_SIZE; ++i)
    {
        CustomTransaction txn;
        txn.txnId = startTxnId + i;
        txn.payload = generateYCSBOp();
        txn.timestamp = ts;
        txn.sender = "client";

        batch.transactions.push_back(std::move(txn));
    }

    return std::to_string(batchId) + "|" + batch.serialize();
}

// ---- Stats ----
struct Stats {
    struct Sample {
        std::chrono::steady_clock::time_point ackTime;
        int64_t latUs;
    };

    std::vector<Sample> samples;

    void record(int64_t l) {
        samples.push_back({std::chrono::steady_clock::now(), l});
    }

    void report(double elapsed)
    {
        if (samples.empty()) return;

        std::vector<int64_t> latUs;
        latUs.reserve(samples.size());

        for (auto const& s : samples)
            latUs.push_back(s.latUs);

        std::sort(latUs.begin(), latUs.end());
        int n = latUs.size();

        double mean = std::accumulate(latUs.begin(), latUs.end(), 0LL) / (double)n / 1000.0;
        double p50  = latUs[n * 0.50] / 1000.0;
        double p99  = latUs[n * 0.99] / 1000.0;
        double p999 = latUs[std::min((int)(n * 0.999), n - 1)] / 1000.0;
        double tput = (double)n * BATCH_SIZE / elapsed;

        printf("=== Overall Results ===\n");
        printf("Elapsed:        %.2f s\n", elapsed);
        printf("Throughput:     %.0f ops/s\n", tput);
        printf("Latency mean:   %.2f ms\n", mean);
        printf("Latency p50:    %.2f ms\n", p50);
        printf("Latency p99:    %.2f ms\n", p99);
        printf("Latency p99.9:  %.2f ms\n", p999);
        printf("Batches:        %d\n", n);
    }

    void reportLastWindow(double windowSec = 60.0)
    {
        if (samples.empty()) return;

        auto endTime = samples.back().ackTime;
        auto startTime = endTime - std::chrono::milliseconds((int)(windowSec * 1000));

        std::vector<int64_t> windowLatUs;
        for (auto const& s : samples)
        {
            if (s.ackTime >= startTime)
                windowLatUs.push_back(s.latUs);
        }

        if (windowLatUs.empty())
        {
            printf("=== Last %.0f Seconds Results ===\n", windowSec);
            printf("No committed batches in this window.\n");
            return;
        }

        std::sort(windowLatUs.begin(), windowLatUs.end());
        int n = windowLatUs.size();

        double mean = std::accumulate(windowLatUs.begin(), windowLatUs.end(), 0LL) / (double)n / 1000.0;
        double p50  = windowLatUs[n * 0.50] / 1000.0;
        double p99  = windowLatUs[std::min((int)(n * 0.99), n - 1)] / 1000.0;
        double p999 = windowLatUs[std::min((int)(n * 0.999), n - 1)] / 1000.0;

        double actualWindowSec = windowSec;
        if (samples.size() > 1)
        {
            auto first = samples.front().ackTime;
            double totalObservedSec =
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - first).count() / 1000.0;
            actualWindowSec = std::min(windowSec, std::max(0.001, totalObservedSec));
        }

        double tput = (double)n * BATCH_SIZE / actualWindowSec;

        printf("=== Last %.0f Seconds Results ===\n", actualWindowSec);
        printf("Throughput:     %.0f ops/s\n", tput);
        printf("Latency mean:   %.2f ms\n", mean);
        printf("Latency p50:    %.2f ms\n", p50);
        printf("Latency p99:    %.2f ms\n", p99);
        printf("Latency p99.9:  %.2f ms\n", p999);
        printf("Batches:        %d\n", n);
    }
};

// ---- Sliding window client ----
struct Client {
    int fd;

    std::mutex              mu;
    std::condition_variable cv;
    std::map<uint64_t,
        std::chrono::steady_clock::time_point> inFlight;

    Stats    stats;
    uint64_t nextBatchId = 0;
    uint64_t nextTxnId   = 0;
    int      sent        = 0;
    int      committed   = 0;
    bool     done        = false;

    void sendBatch()
    {
        uint64_t batchId = nextBatchId++;
        std::string data = serializeBatch(batchId, nextTxnId);
        nextTxnId += BATCH_SIZE;

        uint32_t lenNet = htonl((uint32_t)data.size());
        if (send(fd, &lenNet, 4, MSG_NOSIGNAL) < 0 ||
            send(fd, data.c_str(), data.size(), MSG_NOSIGNAL) < 0)
        {
            std::cerr << "Send failed\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mu);
            inFlight[batchId] = std::chrono::steady_clock::now();
        }
        sent++;
    }

    void onAck(uint64_t batchId)
    {
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mu);
            auto it = inFlight.find(batchId);
            if (it != inFlight.end())
            {
                int64_t latUs = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second).count();

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

        // ACK receiver thread
        std::thread ackThread([this]() {
            while (!done)
            {
                uint64_t ackNet = 0;
                int n = recv(fd, &ackNet, 8, MSG_WAITALL);
                if (n <= 0) break;
                onAck(be64toh(ackNet));
            }
        });

        // Send loop — either total_batches or duration based
        auto shouldKeepSending = [&]() {
            if (DURATION_SEC > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - startTime).count();
                return elapsed < DURATION_SEC;
            }
            return sent < TOTAL_BATCHES;
        };

        while (shouldKeepSending())
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [this]() {
                return (int)inFlight.size() < MAX_IN_FLIGHT;
            });
            lock.unlock();

            sendBatch();

            if (sent % 200 == 0)
            {
                auto now = std::chrono::steady_clock::now();
                static auto last = startTime;
                double dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() / 1000.0;
                printf("sent=%d committed=%d in_flight=%zu  (last 200 batches took %.2fs)\n",
                    sent, committed, inFlight.size(), dt);
                last = now;
            }
        }

        // Wait for all ACKs
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [this]() {
                return inFlight.empty();
            });
        }

        done = true;

        // Wake the ACK receiver if it is blocked in recv(..., MSG_WAITALL).
        shutdown(fd, SHUT_RDWR);

        if (ackThread.joinable())
            ackThread.join();

        double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count() / 1000.0;

        stats.report(elapsed);
        stats.reportLastWindow(60.0);
    }
};

int main(int argc, char* argv[])
{
    if (argc < 6) {
        fprintf(stderr,
            "Usage: client <leader_ip> <port> "
            "<max_in_flight> <total_batches> <batch_size>\n"
            "Example: ./client 10.128.0.5 12000 10 5000 100\n");
        return 1;
    }



    std::string leaderIp = argv[1];
    int port             = std::stoi(argv[2]);
    MAX_IN_FLIGHT        = std::stoi(argv[3]);
    TOTAL_BATCHES        = std::stoi(argv[4]);
    BATCH_SIZE           = std::stoi(argv[5]);

    if (argc >= 7)
    {
        DURATION_SEC = std::stoi(argv[6]);

    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, leaderIp.c_str(), &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to connect to %s:%d\n",
                leaderIp.c_str(), port);
        return 1;
    }

    printf("Connected to %s:%d | max_in_flight=%d "
           "total_batches=%d batch_size=%d\n",
           leaderIp.c_str(), port, MAX_IN_FLIGHT,
           TOTAL_BATCHES, BATCH_SIZE);

    Client client;
    client.fd = fd;
    client.run();

    close(fd);
    return 0;
}
























