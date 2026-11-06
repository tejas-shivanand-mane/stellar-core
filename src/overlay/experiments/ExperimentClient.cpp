// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "main/Config.h"
#include "overlay/CustomYCSBWorkload.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include <arpa/inet.h>
#include <atomic>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <thread>

namespace stellar
{
using namespace experiment;

static int g_clientListenerFd = -1;
static std::vector<int> g_clientFds;
static std::mutex g_clientFdsMutex;

static std::atomic<bool> g_propPending{false};
static std::mutex g_clientRequestMutex;
static std::queue<PendingClientRequest> g_clientRequestQueue;

void
OverlayManagerImpl::startClientListener(int port)
{
    g_clientListenerFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_clientListenerFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_clientListenerFd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        CLOG_ERROR(Overlay, "Client listener bind failed on port {}", port);
        return;
    }
    listen(g_clientListenerFd, 128);
    g_clientListenerActive = true;

    CLOG_INFO(Overlay, "Client listener started on port {}", port);

    std::thread([this, port]() {
        while (true)
        {
            int clientFd = accept(g_clientListenerFd, nullptr, nullptr);
            if (clientFd < 0)
                break;

            int noDelay = 1;
            if (setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &noDelay,
                           sizeof(noDelay)) < 0)
            {
                CLOG_ERROR(Overlay, "Failed to enable TCP_NODELAY fd={}",
                           clientFd);
                close(clientFd);
                continue;
            }

            CLOG_INFO(Overlay, "Client connected fd={}", clientFd);

            {
                std::lock_guard<std::mutex> lock(g_clientFdsMutex);
                g_clientFds.push_back(clientFd);
            }

            // One thread per client connection
            std::thread([this, clientFd]() {
                while (true)
                {
                    // Read 4-byte length prefix
                    uint32_t lenNet = 0;
                    int n = recv(clientFd, &lenNet, 4, MSG_WAITALL);
                    if (n <= 0)
                        break;

                    uint32_t len = ntohl(lenNet);
                    if (len == 0 || len > 1024 * 1024)
                        break;

                    //
                    std::string data(len, '\0');
                    n = recv(clientFd, &data[0], len, MSG_WAITALL);
                    if (n <= 0)
                        break;

                    // Parse: requestId|singleTxn
                    size_t sep = data.find('|');
                    if (sep == std::string::npos)
                    {
                        CLOG_WARNING(
                            Overlay,
                            "[CLIENT REJECT] missing request separator");
                        continue;
                    }

                    uint64_t requestId = 0;
                    try
                    {
                        requestId = std::stoull(data.substr(0, sep));
                    }
                    catch (...)
                    {
                        CLOG_WARNING(Overlay,
                                     "[CLIENT REJECT] malformed requestId");
                        continue;
                    }

                    std::string txnData = data.substr(sep + 1);

                    CustomTransaction txn;
                    try
                    {
                        txn = CustomTransaction::deserialize(txnData);
                    }
                    catch (...)
                    {
                        CLOG_WARNING(Overlay,
                                     "[CLIENT REJECT] malformed transaction "
                                     "for requestId={}",
                                     requestId);
                        continue;
                    }

                    // Fast path for read-only requests.
                    // READ requests are ACKed immediately for all custom
                    // protocols.
                    {
                        std::istringstream ss(txn.payload);
                        std::string op;
                        ss >> op;

                        if (op == "READ")
                        {
                            uint64_t ackNet = htobe64(requestId);

                            ssize_t sent =
                                send(clientFd, &ackNet, 8, MSG_NOSIGNAL);
                            if (sent != 8)
                            {
                                CLOG_WARNING(Overlay,
                                             "[CLIENT READ ACK FAILED] "
                                             "requestId={} fd={}",
                                             requestId, clientFd);
                                break;
                            }

                            CLOG_DEBUG(Overlay,
                                       "[CLIENT READ FAST ACK] requestId={} "
                                       "payload='{}'",
                                       requestId, txn.payload);

                            continue;
                        }
                    }

                    size_t queueSize = 0;
                    {
                        PendingClientRequest pending;
                        pending.requestId = requestId;
                        pending.clientFd = clientFd;
                        pending.txn = std::move(txn);

                        std::lock_guard<std::mutex> lock(g_clientRequestMutex);
                        g_clientRequestQueue.push(std::move(pending));
                        queueSize = g_clientRequestQueue.size();
                    }

                    CLOG_DEBUG(Overlay,
                               "[CLIENT REQUEST QUEUE] queued requestId={} "
                               "queueSize={} serverBatchSize={}",
                               requestId, queueSize, SERVER_BATCH_SIZE);

                    // Trigger prop() only when enough individual requests have
                    // accumulated to form one server-side block.
                    if (queueSize >= SERVER_BATCH_SIZE)
                    {
                        bool expected = false;
                        if (g_propPending.compare_exchange_strong(expected,
                                                                  true))
                        {
                            mApp.postOnMainThread(
                                [this]() {
                                    prop();
                                    g_propPending.store(false);
                                },
                                "client-triggered server-batch prop");
                        }
                    }
                }

                CLOG_INFO(Overlay, "Client disconnected fd={}", clientFd);
                close(clientFd);

                std::lock_guard<std::mutex> lock(g_clientFdsMutex);
                g_clientFds.erase(std::remove(g_clientFds.begin(),
                                              g_clientFds.end(), clientFd),
                                  g_clientFds.end());
            }).detach();
        }
    }).detach();
}

namespace experiment
{
void
ackClientBatchesForBlock(uint64_t view, Hash const& blockHash)
{
    BlockKey key{view, blockHash};

    auto it = g_blockClientAcks.find(key);
    if (it == g_blockClientAcks.end())
    {
        return;
    }

    for (auto const& ack : it->second)
    {
        uint64_t ackNet = htobe64(ack.batchId);
        send(ack.clientFd, &ackNet, 8, MSG_NOSIGNAL);

        CLOG_DEBUG(Overlay, "[CLIENT ACK] batchId={} fd={} block={} view={}",
                   ack.batchId, ack.clientFd, hexAbbrev(blockHash), view);
    }

    g_blockClientAcks.erase(it);
}

size_t
getClientRequestQueueSize()
{
    std::lock_guard<std::mutex> lock(g_clientRequestMutex);
    return g_clientRequestQueue.size();
}

bool
tryBuildServerBatch(TransactionBatch& batch, std::vector<ClientAck>& clientAcks)
{
    std::lock_guard<std::mutex> lock(g_clientRequestMutex);

    if (g_clientRequestQueue.size() < SERVER_BATCH_SIZE)
    {
        return false;
    }

    batch.transactions.clear();
    clientAcks.clear();

    for (size_t i = 0; i < SERVER_BATCH_SIZE; ++i)
    {
        auto pending = std::move(g_clientRequestQueue.front());
        g_clientRequestQueue.pop();

        batch.transactions.push_back(std::move(pending.txn));
        clientAcks.push_back(ClientAck{pending.requestId, pending.clientFd});
    }

    return true;
}

TransactionBatch
makeSyntheticYCSBBatch(Application& app, std::string const& senderShortID,
                       uint64_t startTxnId, size_t batchSize)
{
    TransactionBatch batch;

    uint64_t currentTime = VirtualClock::to_time_t(app.getClock().system_now());

    for (size_t i = 0; i < batchSize; ++i)
    {
        CustomTransaction txn;
        txn.txnId = startTxnId + i;
        txn.payload = generateYCSBOp();
        txn.timestamp = currentTime;
        txn.sender = senderShortID;

        batch.transactions.push_back(txn);
    }

    return batch;
}

}

} // namespace stellar
