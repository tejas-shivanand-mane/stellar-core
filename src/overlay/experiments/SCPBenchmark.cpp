// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "crypto/ShortHash.h"
#include "herder/Herder.h"
#include "herder/TransactionQueue.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxnEntry.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "transactions/TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"
#include <cstring>

namespace stellar
{
using namespace experiment;

struct SCPTxnStats
{
    int totalSubmitted = 0;
    int totalCommitted = 0;
    int totalOperations = 0;
    std::vector<int64_t> txLatencies;
    std::unordered_map<Hash, std::chrono::steady_clock::time_point> submitTimes;
    std::chrono::steady_clock::time_point testStart;
};

static SCPTxnStats g_scpTxnStats;

static std::unordered_map<std::string, uint64_t> g_accountSequences;
static bool g_accountInitialized = false;
static bool g_accountCreationSubmitted = false;

static std::vector<SecretKey> g_testAccounts;
static std::vector<std::string> g_testAccountKeys;
static bool g_multiAccountsInitialized = false;

// Return whether the account exists in this node's current ledger state.
bool
accountExistsInLedger(Application& app, PublicKey const& pk)
{
    try
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, pk);
        bool exists = (bool)acc;
        ltx.commit();
        return exists;
    }
    catch (...)
    {
        return false;
    }
}

// Create NODE_SEED directly in the local ledger for benchmark setup; this
// bypasses SCP.
void
submitAccountCreationTransaction(Application& app)
{
    if (g_accountCreationSubmitted)
        return;

    CLOG_INFO(Overlay, "[SCP TXN] Submitting account creation transaction...");

    //  A CREATE_ACCOUNT transaction would require an existing funded source
    //  account.
    //
    //
    //

    auto source = app.getConfig().NODE_SEED;
    auto pk = source.getPublicKey();

    // Check if already exists
    if (accountExistsInLedger(app, pk))
    {
        CLOG_INFO(Overlay, "[SCP TXN] Account already exists in ledger");
        g_accountCreationSubmitted = true;
        return;
    }

    // Benchmark-only fallback: create the account directly in the local ledger.
    //
    //
    try
    {
        std::string accountKey = KeyUtils::toStrKey<PublicKey>(pk);
        AccountID accountID;
        accountID.ed25519() = pk.ed25519();

        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, pk);

        if (!acc)
        {
            //
            LedgerEntry le;
            le.data.type(ACCOUNT);
            le.data.account().accountID = accountID;
            le.data.account().balance = 1000000000000LL; // 100,000 XLM
            le.data.account().seqNum = 1;
            le.data.account().numSubEntries = 0;
            le.data.account().flags = 0;
            le.data.account().homeDomain.clear();
            le.data.account().inflationDest.activate() = {};

            ltx.create(le);
            ltx.commit();

            CLOG_WARNING(
                Overlay,
                "[SCP TXN] Created account {} LOCALLY (not through consensus!)",
                accountKey);
            CLOG_WARNING(
                Overlay,
                "[SCP TXN] This account will NOT be visible to other nodes!");
            CLOG_WARNING(Overlay,
                         "[SCP TXN] For proper testing, pre-create accounts in "
                         "genesis or use CREATE_ACCOUNT tx");
        }

        g_accountCreationSubmitted = true;
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Failed to create account: {}", e.what());
    }
}

// Initialize sequence tracking once account exists
void
initializeSequenceTracking(Application& app)
{
    if (g_accountInitialized)
        return;

    auto source = app.getConfig().NODE_SEED;
    auto pk = source.getPublicKey();
    std::string accountKey = KeyUtils::toStrKey<PublicKey>(pk);

    try
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, pk);

        if (acc)
        {
            uint64_t currentSeq = acc.current().data.account().seqNum;
            g_accountSequences[accountKey] = currentSeq + 1;
            ltx.commit();

            CLOG_INFO(Overlay, "[SCP TXN] Initialized sequence tracking at {}",
                      currentSeq + 1);
            g_accountInitialized = true;
        }
        else
        {
            CLOG_WARNING(Overlay,
                         "[SCP TXN] Account doesn't exist yet in ledger");
        }
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Failed to initialize sequence: {}",
                   e.what());
    }
}

void
initializeMultipleAccounts(Application& app)
{
    if (g_multiAccountsInitialized)
        return;

    CLOG_INFO(Overlay, "[Multi-Account] Initializing {} test accounts",
              NUM_TEST_ACCOUNTS);

    g_testAccounts.clear();
    g_testAccountKeys.clear();

    for (int i = 0; i < NUM_TEST_ACCOUNTS; i++)
    {
        // Generate deterministic keys based on NODE_SEED + index
        auto baseSeed = app.getConfig().NODE_SEED;
        std::string seedStr = baseSeed.getStrKeyPublic() + std::to_string(i);
        Hash seedHash =
            sha256(std::vector<uint8_t>(seedStr.begin(), seedStr.end()));
        SecretKey accountKey = SecretKey::fromSeed(seedHash);

        g_testAccounts.push_back(accountKey);

        auto pk = accountKey.getPublicKey();
        std::string accountKeyStr = KeyUtils::toStrKey<PublicKey>(pk);
        g_testAccountKeys.push_back(accountKeyStr);

        // Check if account exists in ledger
        if (accountExistsInLedger(app, pk))
        {
            // Initialize sequence tracking
            try
            {
                LedgerTxn ltx(app.getLedgerTxnRoot());
                auto acc = stellar::loadAccount(ltx, pk);
                if (acc)
                {
                    uint64_t currentSeq = acc.current().data.account().seqNum;
                    g_accountSequences[accountKeyStr] = currentSeq + 1;
                    CLOG_INFO(
                        Overlay,
                        "[Multi-Account] Account {} initialized with seq={}", i,
                        currentSeq + 1);
                }
                ltx.commit();
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(Overlay,
                           "[Multi-Account] Failed to init account {}: {}", i,
                           e.what());
            }
        }
        else
        {
            // Benchmark-only fallback: create the account directly in the local
            // ledger.
            CLOG_WARNING(
                Overlay,
                "[Multi-Account] Account {} doesn't exist, creating locally",
                i);

            AccountID accountID;
            accountID.ed25519() = pk.ed25519();

            try
            {
                LedgerTxn ltx(app.getLedgerTxnRoot());

                LedgerEntry le;
                le.data.type(ACCOUNT);
                le.data.account().accountID = accountID;
                le.data.account().balance = 1000000000000LL;
                le.data.account().seqNum = 1;
                le.data.account().numSubEntries = 0;
                le.data.account().flags = 0;
                le.data.account().homeDomain.clear();
                le.data.account().inflationDest.activate() = {};

                ltx.create(le);
                ltx.commit();

                g_accountSequences[accountKeyStr] = 2;
                CLOG_INFO(Overlay, "[Multi-Account] Created account {} locally",
                          i);
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(Overlay,
                           "[Multi-Account] Failed to create account {}: {}", i,
                           e.what());
            }
        }
    }

    g_multiAccountsInitialized = true;
    CLOG_INFO(Overlay, "[Multi-Account] Initialization complete");
}

TransactionEnvelope
makeBatchedPaymentTx(Application& app, SecretKey const& source,
                     SequenceNumber seqNum, int numOps = 100)
{
    auto pk = source.getPublicKey();
    std::string accountKey = KeyUtils::toStrKey<PublicKey>(pk);

    //
    AccountID accountID;
    accountID.ed25519() = pk.ed25519();

    // Allocate the next locally tracked sequence number.
    if (!g_accountSequences.count(accountKey))
    {
        g_accountSequences[accountKey] = 0;
    }

    uint64_t nextSeq = g_accountSequences[accountKey]++;

    // Charge at least 100 stroops per operation.
    uint32_t baseFeePerOp = app.getLedgerManager().getLastTxFee();
    uint32_t fee = std::max<uint32_t>(baseFeePerOp * numOps, 100u * numOps);

    // Wrap AccountID into MuxedAccount
    MuxedAccount muxSource;
    muxSource.ed25519() = accountID.ed25519();

    MuxedAccount muxDest;
    muxDest.ed25519() = accountID.ed25519();

    // Build transaction with multiple operations
    Transaction tx;
    tx.sourceAccount = muxSource;
    tx.fee = fee;
    tx.seqNum = nextSeq;
    tx.memo.type(MEMO_NONE);

    // Add numOps payment operations
    for (int i = 0; i < numOps; i++)
    {
        PaymentOp pay;
        pay.destination = muxDest;
        pay.asset.type(ASSET_TYPE_NATIVE);
        pay.amount = 1; // One-stroop self-payment.

        Operation op;
        op.body.type(PAYMENT);
        op.body.paymentOp() = pay;

        tx.operations.push_back(op);
    }

    // Envelope (v1)
    TransactionEnvelope env;
    env.type(ENVELOPE_TYPE_TX);
    env.v1().tx = tx;

    // Sign
    TransactionSignaturePayload payload;
    payload.networkId = app.getNetworkID();
    payload.taggedTransaction.type(ENVELOPE_TYPE_TX);
    payload.taggedTransaction.tx() = tx;

    Hash txHash = sha256(xdr::xdr_to_opaque(payload));

    DecoratedSignature sig;
    auto const& pkBytes = pk.ed25519();
    std::memcpy(sig.hint.data(),
                pkBytes.data() + (pkBytes.size() - sig.hint.size()),
                sig.hint.size());
    sig.signature = source.sign(txHash);
    env.v1().signatures.push_back(sig);

    return env;
}

void
submitBatchedTransactionToSCP(Application& app, int accountIndex = 0,
                              int batchSize = 100)
{
    static std::vector<int> txCounters(NUM_TEST_ACCOUNTS, 0);

    // Validate account index
    if (accountIndex < 0 || accountIndex >= NUM_TEST_ACCOUNTS)
    {
        CLOG_ERROR(Overlay, "[SCP BATCH] Invalid account index: {}",
                   accountIndex);
        return;
    }

    if (!g_multiAccountsInitialized)
    {
        CLOG_ERROR(Overlay,
                   "[SCP BATCH] Multi-account system not initialized!");
        return;
    }

    try
    {
        auto source = g_testAccounts[accountIndex];
        auto env = makeBatchedPaymentTx(
            app, source, txCounters[accountIndex] + 1, batchSize);

        auto txFrame = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), env);

        if (txFrame)
        {
            Hash txHash = txFrame->getFullHash();

            //
            g_scpTxnStats.submitTimes[txHash] =
                std::chrono::steady_clock::now();

            auto result = app.getHerder().recvTransaction(txFrame, true);

            if (result.code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
            {
                g_scpTxnStats.totalSubmitted++;
                g_scpTxnStats.totalOperations += batchSize; //
                CLOG_DEBUG(Overlay,
                           "[SCP BATCH] Submitted batched tx from account {} "
                           "({} ops, hash={}) - PENDING",
                           accountIndex, batchSize, hexAbbrev(txHash));
                txCounters[accountIndex]++;
            }
            else if (result.code ==
                     TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE)
            {
                CLOG_DEBUG(Overlay,
                           "[SCP BATCH] Batched tx from account {} duplicate, "
                           "skipping",
                           accountIndex);
                txCounters[accountIndex]++;
            }
            else
            {
                CLOG_WARNING(Overlay,
                             "[SCP BATCH REJECT] account={} hash={} "
                             "resultCode={} batchSize={} "
                             "totalSubmittedTxs={} totalSubmittedOps={}",
                             accountIndex, hexAbbrev(txHash),
                             static_cast<int>(result.code), batchSize,
                             g_scpTxnStats.totalSubmitted,
                             g_scpTxnStats.totalOperations);
            }
        }
    }
    catch (std::exception& e)
    {
        CLOG_ERROR(Overlay, "[SCP BATCH] Exception for account {}: {}",
                   accountIndex, e.what());
    }
}

TransactionEnvelope
makeDummyPaymentTx(Application& app, SecretKey const& source,
                   SequenceNumber /*seqArg*/)
{
    auto pk = source.getPublicKey();
    std::string accountKey = KeyUtils::toStrKey<PublicKey>(pk);

    // Build AccountID from PublicKey
    AccountID accountID;
    accountID.ed25519() = pk.ed25519();

    // Get next sequence from tracker
    if (!g_accountSequences.count(accountKey))
    {
        // throw std::runtime_error("Account not initialized - sequence tracking
        // not ready");
        g_accountSequences[accountKey] = 0;
    }

    uint64_t nextSeq = g_accountSequences[accountKey]++;

    // Fee
    uint32_t baseFeePerOp = app.getLedgerManager().getLastTxFee();
    uint32_t fee = std::max<uint32_t>(baseFeePerOp, 100u);

    // Wrap AccountID into MuxedAccount
    MuxedAccount muxSource;
    muxSource.ed25519() = accountID.ed25519();

    MuxedAccount muxDest;
    muxDest.ed25519() = accountID.ed25519();

    // Build a simple self-payment
    Transaction tx;
    tx.sourceAccount = muxSource;
    tx.fee = fee;
    tx.seqNum = nextSeq;
    tx.memo.type(MEMO_NONE);

    PaymentOp pay;
    pay.destination = muxDest;
    pay.asset.type(ASSET_TYPE_NATIVE);
    pay.amount = 1; // 1 stroop

    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp() = pay;
    tx.operations.push_back(op);

    // Envelope (v1)
    TransactionEnvelope env;
    env.type(ENVELOPE_TYPE_TX);
    env.v1().tx = tx;

    // Sign
    TransactionSignaturePayload payload;
    payload.networkId = app.getNetworkID();
    payload.taggedTransaction.type(ENVELOPE_TYPE_TX);
    payload.taggedTransaction.tx() = tx;

    Hash txHash = sha256(xdr::xdr_to_opaque(payload));

    DecoratedSignature sig;
    auto const& pkBytes = pk.ed25519();
    std::memcpy(sig.hint.data(),
                pkBytes.data() + (pkBytes.size() - sig.hint.size()),
                sig.hint.size());
    sig.signature = source.sign(txHash);
    env.v1().signatures.push_back(sig);

    return env;
}

void
submitTransactionToSCP(Application& app, int accountIndex = 0)
{
    static std::vector<int> txCounters(NUM_TEST_ACCOUNTS, 0);

    // Validate account index
    if (accountIndex < 0 || accountIndex >= NUM_TEST_ACCOUNTS)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Invalid account index: {}",
                   accountIndex);
        return;
    }

    if (!g_multiAccountsInitialized)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Multi-account system not initialized!");
        return;
    }

    try
    {
        auto source = g_testAccounts[accountIndex];
        auto env =
            makeDummyPaymentTx(app, source, txCounters[accountIndex] + 1);

        auto txFrame = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), env);

        if (txFrame)
        {
            Hash txHash = txFrame->getFullHash();

            // Track submission time
            g_scpTxnStats.submitTimes[txHash] =
                std::chrono::steady_clock::now();

            auto result = app.getHerder().recvTransaction(txFrame, true);

            if (result.code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
            {
                g_scpTxnStats.totalSubmitted++;
                CLOG_INFO(Overlay,
                          "[SCP TXN] Submitted tx from account {} (tx #{}, "
                          "hash={}) - PENDING",
                          accountIndex, txCounters[accountIndex],
                          hexAbbrev(txHash));
                txCounters[accountIndex]++;
            }
            else if (result.code ==
                     TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE)
            {
                CLOG_DEBUG(Overlay,
                           "[SCP TXN] Tx from account {} duplicate, skipping",
                           accountIndex);
                txCounters[accountIndex]++;
            }
            else
            {
                CLOG_WARNING(Overlay,
                             "[SCP TXN] Tx from account {} (hash={}) rejected "
                             "- Result={}",
                             accountIndex, hexAbbrev(txHash),
                             static_cast<int>(result.code));
            }
        }
    }
    catch (std::exception& e)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Exception for account {}: {}",
                   accountIndex, e.what());
    }
}

TransactionEnvelope
createSCPTxFromProposal(Application& app, SecretKey const& source,
                        SequenceNumber seq, uint64_t view,
                        Hash const& blockHash, std::string const& data)
{
    // Build TransactionV0
    TransactionV0 tx;
    tx.sourceAccountEd25519 = source.getPublicKey().ed25519();
    tx.fee = 100;
    tx.seqNum = seq;

    // Use ManageData operation to store proposal data
    ManageDataOp dataOp;
    dataOp.dataName = "prop_" + std::to_string(view);

    // Serialize proposal: view|blockHash|data
    DataValue dataVal;
    std::string serialized =
        std::to_string(view) + "|" + binToHex(blockHash) + "|" + data;
    dataVal.assign(serialized.begin(), serialized.end());
    dataOp.dataValue.activate() = dataVal;

    Operation op;
    op.body.type(MANAGE_DATA);
    op.body.manageDataOp() = dataOp;
    tx.operations.push_back(op);

    tx.timeBounds.activate() = {};

    // Wrap in envelope
    TransactionEnvelope env(ENVELOPE_TYPE_TX_V0);
    env.v0().tx = tx;

    // Sign
    Hash txHash = sha256(xdr::xdr_to_opaque(env.v0().tx));
    DecoratedSignature sig;
    auto const& pubkey = source.getPublicKey().ed25519();
    std::memcpy(sig.hint.data(),
                pubkey.data() + (pubkey.size() - sig.hint.size()),
                sig.hint.size());
    sig.signature = source.sign(txHash);
    env.v0().signatures.push_back(sig);

    return env;
}

struct SCPStats
{
    int totalBatches = 0;
    std::vector<int64_t> batchLatencies;
    std::chrono::steady_clock::time_point testStart;
    std::chrono::steady_clock::time_point lastBatchTime;
    static const int BATCH_SIZE = 100; //
};

static SCPStats g_scpStats;

void
submitNextBatchOfTransactions(Application& app)
{
    static uint32_t lastLedger = 0;

    if (!ENABLE_SCP_TRACKING)
    {
        return;
    }

    // Only one node injects workload.
    if (!app.getConfig().SEND_CUSTOM_MESSAGE)
    {
        return;
    }

    uint32_t currentLedger = app.getLedgerManager().getLastClosedLedgerNum();

    // This function is called after externalization. Avoid duplicate
    // submissions for the same closed ledger.
    if (currentLedger <= lastLedger)
    {
        return;
    }

    auto submitTime = std::chrono::steady_clock::now();

    int submittedOps = 0;

    for (int round = 0; round < SCP_NATIVE_ROUNDS; round++)
    {
        for (int accountIdx = 0; accountIdx < NUM_TEST_ACCOUNTS; accountIdx++)
        {
            submitBatchedTransactionToSCP(app, accountIdx, SCP_OPS_PER_TX);
            submittedOps += SCP_OPS_PER_TX;
        }
    }

    CLOG_INFO(Overlay,
              "[SCP NATIVE SUBMIT] closedLedger={} nextLedger={} rounds={} "
              "opsPerTx={} accounts={} submittedOps={} totalSubmittedTxs={} "
              "totalSubmittedOps={} t_ms={}",
              currentLedger, currentLedger + 1, SCP_NATIVE_ROUNDS,
              SCP_OPS_PER_TX, NUM_TEST_ACCOUNTS, submittedOps,
              g_scpTxnStats.totalSubmitted, g_scpTxnStats.totalOperations,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  submitTime.time_since_epoch())
                  .count());

    lastLedger = currentLedger;
}

void
OverlayManagerImpl::tickSCPBenchmark()
{
    if (ENABLE_SCP_TRACKING)
    {

        static uint32_t lastLedger = 0;

        NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
        std::string shortID = KeyUtils::toShortString(selfID);
        CLOG_DEBUG(Overlay, "shortID, txn_count: {}, {}", shortID, txn_count);

        static bool accountCreatedLocally = false;
        if (!accountCreatedLocally)
        {
            submitAccountCreationTransaction(mApp);
            accountCreatedLocally = true;
        }

        if (!mApp.getConfig().SEND_CUSTOM_MESSAGE)
        {
            //
            return;
        }

        static bool initialized = false;
        static bool firstOps = false;

        static int initWaitTicks = 0;

        if (!initialized)
        {
            if (initWaitTicks == 0)
            {
                CLOG_INFO(Overlay, "========================================");
                CLOG_INFO(Overlay, "SCP PERFORMANCE TRACKING STARTED");
                CLOG_INFO(Overlay, "========================================");
                CLOG_WARNING(Overlay, "⚠️  IMPORTANT: This creates accounts "
                                      "LOCALLY, not through SCP!");
                CLOG_WARNING(Overlay, "⚠️  For proper testing, pre-create "
                                      "accounts in genesis ledger");
                CLOG_INFO(Overlay, "========================================");

                //
                g_scpTxnStats.testStart = std::chrono::steady_clock::now();

                submitAccountCreationTransaction(mApp);

                g_scpStats.testStart = std::chrono::steady_clock::now();
                g_scpStats.lastBatchTime = std::chrono::steady_clock::now();
            }

            // Wait five ticks for local account creation
            initWaitTicks++;

            if (initWaitTicks <= 5)
            {
                CLOG_INFO(Overlay,
                          "[SCP TXN] Waiting for local account creation... "
                          "(tick {}/5)",
                          initWaitTicks);
                return;
            }

            //
            if (!g_accountInitialized)
            {
                initializeSequenceTracking(mApp);
                if (!g_accountInitialized)
                {
                    CLOG_ERROR(
                        Overlay,
                        "[SCP TXN] Failed to initialize sequence tracking!");
                }
            }

            initializeMultipleAccounts(mApp);

            initialized = true;

            CLOG_INFO(Overlay, "[SCP TXN] Initialization complete, starting "
                               "transaction submission");

            uint32_t currentLedger =
                mApp.getLedgerManager().getLastClosedLedgerNum();
            if (currentLedger > lastLedger && !firstOps)

            {

                // Seed the first transaction batch after initialization.
                //
                for (int round = 0; round < SCP_NATIVE_ROUNDS; round++)
                {
                    for (int accountIdx = 0; accountIdx < NUM_TEST_ACCOUNTS;
                         accountIdx++)
                    {
                        submitBatchedTransactionToSCP(mApp, accountIdx,
                                                      SCP_OPS_PER_TX);
                    }
                }

                firstOps = true;
                lastLedger = currentLedger;
            }

            CLOG_INFO(Overlay, "[SCP TXN] Initial transaction batch submitted");
        }
    }

    //
    //
}

} // namespace stellar
