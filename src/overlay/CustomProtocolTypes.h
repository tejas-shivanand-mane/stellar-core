#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

struct CustomTransaction
{
    uint64_t txnId;
    std::string payload;
    uint64_t timestamp;
    std::string sender;

    std::string serialize() const
    {
        return std::to_string(txnId) + ":" +
               payload + ":" +
               std::to_string(timestamp) + ":" +
               sender;
    }

    static CustomTransaction deserialize(std::string const& data)
    {
        CustomTransaction txn;
        std::istringstream ss(data);
        std::string token;

        std::getline(ss, token, ':');
        txn.txnId = std::stoull(token);

        std::getline(ss, txn.payload, ':');

        std::getline(ss, token, ':');
        txn.timestamp = std::stoull(token);

        std::getline(ss, txn.sender, ':');

        return txn;
    }
};

struct TransactionBatch
{
    std::vector<CustomTransaction> transactions;

    std::string serialize() const
    {
        std::string result;
        for (size_t i = 0; i < transactions.size(); ++i)
        {
            result += transactions[i].serialize();
            if (i < transactions.size() - 1)
            {
                result += "|";
            }
        }
        return result;
    }

    static TransactionBatch deserialize(std::string const& data)
    {
        TransactionBatch batch;
        std::istringstream ss(data);
        std::string txnData;

        while (std::getline(ss, txnData, '|'))
        {
            if (!txnData.empty())
            {
                batch.transactions.push_back(
                    CustomTransaction::deserialize(txnData));
            }
        }

        return batch;
    }
};

struct PendingClientBatch
{
    uint64_t batchId;
    int clientFd;
    TransactionBatch batch;
};

struct PendingClientRequest
{
    uint64_t requestId;
    int clientFd;
    CustomTransaction txn;
};