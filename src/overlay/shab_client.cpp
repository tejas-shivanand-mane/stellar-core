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
#include <cmath>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <endian.h>
#include <unistd.h>

// ---- Config ----
static int    MAX_IN_FLIGHT = 10;
static int    BATCH_SIZE    = 100;
static int    TOTAL_BATCHES = 10000;


static int DURATION_SEC = 60; // 0 = use total_batches, >0 = run for N seconds


// ---- YCSB ----
static double ZIPFIAN_ALPHA = 0.99;
static int    ZIPFIAN_N     = 1000000;

static uint64_t zipfianNext()
{
    static double zetaN = 0, zeta2 = 0;
    static bool init = false;
    if (!init) {
        for (int i = 1; i <= ZIPFIAN_N; i++)
            zetaN += 1.0 / pow(i, ZIPFIAN_ALPHA);
        zeta2 = 1.0 + 1.0 / pow(2, ZIPFIAN_ALPHA);
        init = true;
    }
    double u  = (double)rand() / RAND_MAX;
    double uz = u * zetaN;
    if (uz < 1.0) return 1;
    if (uz < 1.0 + pow(0.5, ZIPFIAN_ALPHA)) return 2;
    return (uint64_t)(ZIPFIAN_N * pow(zeta2 / zetaN * u,
                                      1.0 / (1.0 - ZIPFIAN_ALPHA)));
}

static std::string generateOp()
{
    std::string key = "user" + std::to_string(zipfianNext());
    double r = (double)rand() / RAND_MAX;
    return r < 0.5 ? "READ " + key
                   : "UPDATE " + key + " val" + std::to_string(rand());
}

// ---- Serialization ----
// Format: batchId|id:payload:ts:sender|id:payload:ts:sender|...
static std::string serializeBatch(uint64_t batchId,
                                   uint64_t startTxnId)
{
    std::string result = std::to_string(batchId);
    uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (int i = 0; i < BATCH_SIZE; ++i)
    {
        result += "|";
        result += std::to_string(startTxnId + i) + ":"
               + generateOp() + ":"
               + std::to_string(ts) + ":client";
    }
    return result;
}

// ---- Stats ----
struct Stats {
    std::vector<int64_t> latUs;

    void record(int64_t l) { latUs.push_back(l); }

    void report(double elapsed)
    {
        if (latUs.empty()) return;
        std::sort(latUs.begin(), latUs.end());
        int n = latUs.size();

        double mean = std::accumulate(latUs.begin(),
            latUs.end(), 0LL) / (double)n / 1000.0;
        double p50  = latUs[n * 0.50] / 1000.0;
        double p99  = latUs[n * 0.99] / 1000.0;
        double p999 = latUs[(int)(n * 0.999)] / 1000.0;
        double tput = (double)n * BATCH_SIZE / elapsed;

        printf("=== Results ===\n");
        printf("Elapsed:        %.2f s\n",   elapsed);
        printf("Throughput:     %.0f ops/s\n", tput);
        printf("Latency mean:   %.2f ms\n",  mean);
        printf("Latency p50:    %.2f ms\n",  p50);
        printf("Latency p99:    %.2f ms\n",  p99);
        printf("Latency p99.9:  %.2f ms\n",  p999);
        printf("Batches:        %d\n",        n);
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
        ackThread.join();

        double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count() / 1000.0;

        stats.report(elapsed);
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
























