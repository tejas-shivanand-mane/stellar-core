// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/OverlayManagerImpl.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "crypto/ShortHash.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "lib/util/finally.h"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerBareAddress.h"
#include "overlay/PeerManager.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/SurveyDataManager.h"
#include "overlay/TCPPeer.h"
#include "overlay/TxDemandsManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/Thread.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/counter.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

#include <algorithm>
#include "OverlayManagerImpl.h"

#include <fstream>
#include <string>

size_t
getRSS_MB()
{
    std::ifstream status("/proc/self/status");
    std::string line;

    while (std::getline(status, line))
    {
        if (line.rfind("VmRSS:", 0) == 0)
        {
            // Format: VmRSS:   123456 kB
            size_t kb = std::stoul(line.substr(6));
            return kb / 1024;
        }
    }
    return 0;
}



// ---- Forced COLLECT window control ----
static std::chrono::steady_clock::time_point pbftStartTime;
static bool pbftStartTimeSet = false;

static bool collectWindowActive = false;
static uint64_t collectWindowStartView = 0;

static uint64_t collectAttempts = 0;
static constexpr uint64_t MAX_COLLECT_ATTEMPTS = 10;


constexpr int FORCE_COLLECT_AFTER_SEC = 3000;

static bool collectWindowArmed = false;
static uint64_t lastCollectSentView = UINT64_MAX;

static bool force_collect = false;

struct CustomTransaction
{
    uint64_t txnId;           // Unique transaction ID
    std::string payload;      // Transaction data/payload
    uint64_t timestamp;       // When transaction was created
    std::string sender;       // Who created it (could be node ID)
    
    // Serialize for transmission
    std::string serialize() const
    {
        return std::to_string(txnId) + ":" + 
               payload + ":" + 
               std::to_string(timestamp) + ":" + 
               sender;
    }
    
    // Deserialize from string
    static CustomTransaction deserialize(const std::string& data)
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
    
    // Serialize all transactions
    std::string serialize() const
    {
        std::string result;
        for (size_t i = 0; i < transactions.size(); ++i)
        {
            result += transactions[i].serialize();
            if (i < transactions.size() - 1)
                result += "|";  // Separator between transactions
        }
        return result;
    }
    
    // Deserialize batch
    static TransactionBatch deserialize(const std::string& data)
    {
        TransactionBatch batch;
        std::istringstream ss(data);
        std::string txnData;
        
        while (std::getline(ss, txnData, '|'))
        {
            batch.transactions.push_back(CustomTransaction::deserialize(txnData));
        }
        
        return batch;
    }
};



struct SCPTxnStats {
    int totalSubmitted = 0;
    int totalCommitted = 0;
    int totalOperations = 0;  // ADD THIS - track total operations
    std::vector<int64_t> txLatencies;
    std::unordered_map<Hash, std::chrono::steady_clock::time_point> submitTimes;
    std::chrono::steady_clock::time_point testStart;
};


static SCPTxnStats g_scpTxnStats;

static std::unordered_map<std::string, uint64_t> g_accountSequences;
static bool g_accountInitialized = false;
static bool g_accountCreationSubmitted = false;



static const int NUM_TEST_ACCOUNTS = 50;
static std::vector<SecretKey> g_testAccounts;
static std::vector<std::string> g_testAccountKeys;
static bool g_multiAccountsInitialized = false;

// Check if account exists in the ledger (visible to all nodes)
bool accountExistsInLedger(Application& app, PublicKey const& pk)
{
    try
    {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, pk);
        bool exists = (bool)acc;  // Convert to bool instead of comparing to nullptr
        ltx.commit();
        return exists;
    }
    catch (...)
    {
        return false;
    }
}

// Create account through a CREATE_ACCOUNT transaction (goes through SCP)
void submitAccountCreationTransaction(Application& app)
{
    if (g_accountCreationSubmitted)
        return;
    
    CLOG_INFO(Overlay, "[SCP TXN] Submitting account creation transaction...");
    
    // You need a "root" account that already exists with funds
    // In Stellar testnet/local, this is usually a pre-funded account
    // For this example, we'll assume NODE_SEED itself has funds initially
    // If not, you'll need to use a genesis account or create accounts in genesis ledger
    
    auto source = app.getConfig().NODE_SEED;
    auto pk = source.getPublicKey();
    
    // Check if already exists
    if (accountExistsInLedger(app, pk))
    {
        CLOG_INFO(Overlay, "[SCP TXN] Account already exists in ledger");
        g_accountCreationSubmitted = true;
        return;
    }
    
    // For now, create it directly (this is the problem!)
    // In production, you'd need to use a CREATE_ACCOUNT operation
    // from an account that already has funds
    try
    {
        std::string accountKey = KeyUtils::toStrKey<PublicKey>(pk);
        AccountID accountID;
        accountID.ed25519() = pk.ed25519();
        
        LedgerTxn ltx(app.getLedgerTxnRoot());
        auto acc = stellar::loadAccount(ltx, pk);
        
        if (!acc)
        {
            // Create the account directly (NOT through SCP - this is the issue!)
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
            
            CLOG_WARNING(Overlay, "[SCP TXN] Created account {} LOCALLY (not through consensus!)", 
                     accountKey);
            CLOG_WARNING(Overlay, "[SCP TXN] This account will NOT be visible to other nodes!");
            CLOG_WARNING(Overlay, "[SCP TXN] For proper testing, pre-create accounts in genesis or use CREATE_ACCOUNT tx");
        }
        
        g_accountCreationSubmitted = true;
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Failed to create account: {}", e.what());
    }
}

// Initialize sequence tracking once account exists
void initializeSequenceTracking(Application& app)
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
            
            CLOG_INFO(Overlay, "[SCP TXN] Initialized sequence tracking at {}", currentSeq + 1);
            g_accountInitialized = true;
        }
        else
        {
            CLOG_WARNING(Overlay, "[SCP TXN] Account doesn't exist yet in ledger");
        }
    }
    catch (std::exception const& e)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Failed to initialize sequence: {}", e.what());
    }
}


void initializeMultipleAccounts(Application& app)
{
    if (g_multiAccountsInitialized)
        return;
    
    CLOG_INFO(Overlay, "[Multi-Account] Initializing {} test accounts", NUM_TEST_ACCOUNTS);
    
    g_testAccounts.clear();
    g_testAccountKeys.clear();
    
    for (int i = 0; i < NUM_TEST_ACCOUNTS; i++)
    {
        // Generate deterministic keys based on NODE_SEED + index
        auto baseSeed = app.getConfig().NODE_SEED;
        std::string seedStr = baseSeed.getStrKeyPublic() + std::to_string(i);
        Hash seedHash = sha256(std::vector<uint8_t>(seedStr.begin(), seedStr.end()));
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
                    CLOG_INFO(Overlay, "[Multi-Account] Account {} initialized with seq={}", 
                             i, currentSeq + 1);
                }
                ltx.commit();
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(Overlay, "[Multi-Account] Failed to init account {}: {}", i, e.what());
            }
        }
        else
        {
            // Create account locally (same as your existing approach)
            CLOG_WARNING(Overlay, "[Multi-Account] Account {} doesn't exist, creating locally", i);
            
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
                CLOG_INFO(Overlay, "[Multi-Account] Created account {} locally", i);
            }
            catch (std::exception const& e)
            {
                CLOG_ERROR(Overlay, "[Multi-Account] Failed to create account {}: {}", i, e.what());
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
    
    // Build AccountID from PublicKey
    AccountID accountID;
    accountID.ed25519() = pk.ed25519();

    // Get next sequence from tracker
    if (!g_accountSequences.count(accountKey))
    {
        g_accountSequences[accountKey] = 0;
    }
    
    uint64_t nextSeq = g_accountSequences[accountKey]++;

    // Fee: base fee × number of operations
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
        pay.amount = 1; // 1 stroop per operation

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


void submitBatchedTransactionToSCP(Application& app, int accountIndex = 0, int batchSize = 100)
{
    static std::vector<int> txCounters(NUM_TEST_ACCOUNTS, 0);
    
    // Validate account index
    if (accountIndex < 0 || accountIndex >= NUM_TEST_ACCOUNTS)
    {
        CLOG_ERROR(Overlay, "[SCP BATCH] Invalid account index: {}", accountIndex);
        return;
    }
    
    if (!g_multiAccountsInitialized)
    {
        CLOG_ERROR(Overlay, "[SCP BATCH] Multi-account system not initialized!");
        return;
    }
    
    try
    {
        auto source = g_testAccounts[accountIndex];
        auto env = makeBatchedPaymentTx(app, source, txCounters[accountIndex] + 1, batchSize);
        
        auto txFrame = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), env);
        
        if (txFrame)
        {
            Hash txHash = txFrame->getFullHash();
            
            // Track submission time
            g_scpTxnStats.submitTimes[txHash] = std::chrono::steady_clock::now();
            
            auto result = app.getHerder().recvTransaction(txFrame, true);
            
            if (result.code == TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
            {
                g_scpTxnStats.totalSubmitted++;
                g_scpTxnStats.totalOperations += batchSize;  // Track operations
                CLOG_DEBUG(Overlay, "[SCP BATCH] Submitted batched tx from account {} ({} ops, hash={}) - PENDING", 
                          accountIndex, batchSize, hexAbbrev(txHash));
                txCounters[accountIndex]++;
            }
            else if (result.code == TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE)
            {
                CLOG_DEBUG(Overlay, "[SCP BATCH] Batched tx from account {} duplicate, skipping", accountIndex);
                txCounters[accountIndex]++;
            }
            else
            {
                CLOG_WARNING(Overlay, "[SCP BATCH] Batched tx from account {} (hash={}) rejected - Result={}", 
                            accountIndex, hexAbbrev(txHash), static_cast<int>(result.code));
            }
        }
    }
    catch (std::exception& e)
    {
        CLOG_ERROR(Overlay, "[SCP BATCH] Exception for account {}: {}", accountIndex, e.what());
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
        // throw std::runtime_error("Account not initialized - sequence tracking not ready");
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
    tx.fee           = fee;
    tx.seqNum        = nextSeq;
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

void submitTransactionToSCP(Application& app, int accountIndex = 0)
{
    static std::vector<int> txCounters(NUM_TEST_ACCOUNTS, 0);
    
    // Validate account index
    if (accountIndex < 0 || accountIndex >= NUM_TEST_ACCOUNTS)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Invalid account index: {}", accountIndex);
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
        auto env = makeDummyPaymentTx(app, source, txCounters[accountIndex] + 1);
        
        auto txFrame = TransactionFrameBase::makeTransactionFromWire(
            app.getNetworkID(), env);
        
        if (txFrame)
        {
            Hash txHash = txFrame->getFullHash();
            
            // Track submission time
            g_scpTxnStats.submitTimes[txHash] = std::chrono::steady_clock::now();
            
            auto result = app.getHerder().recvTransaction(txFrame, true);
            
            if (result.code == TransactionQueue::AddResultCode::ADD_STATUS_PENDING)
            {
                g_scpTxnStats.totalSubmitted++;
                CLOG_INFO(Overlay, "[SCP TXN] Submitted tx from account {} (tx #{}, hash={}) - PENDING", 
                          accountIndex, txCounters[accountIndex], hexAbbrev(txHash));
                txCounters[accountIndex]++;
            }
            else if (result.code == TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE)
            {
                CLOG_DEBUG(Overlay, "[SCP TXN] Tx from account {} duplicate, skipping", accountIndex);
                txCounters[accountIndex]++;
            }
            else
            {
                CLOG_WARNING(Overlay, "[SCP TXN] Tx from account {} (hash={}) rejected - Result={}", 
                            accountIndex, hexAbbrev(txHash), static_cast<int>(result.code));
            }
        }
    }
    catch (std::exception& e)
    {
        CLOG_ERROR(Overlay, "[SCP TXN] Exception for account {}: {}", accountIndex, e.what());
    }
}








static Hash
makeBlock(Hash const& prev, int txnCount)
{
    std::string input = std::to_string(txnCount) + binToHex(prev);
    return sha256(input);
}



// Key type for (view, blockHash) used in echoes/readies
struct ViewBlockKey {
    uint64_t view;
    Hash block;

    bool operator==(ViewBlockKey const& other) const noexcept {
        return view == other.view && block == other.block;
    }
};

struct ViewBlockKeyHash {
    size_t operator()(ViewBlockKey const& k) const noexcept {
        size_t h1 = std::hash<uint64_t>()(k.view);

        // simple hash combine for the 32-byte Hash
        size_t h2 = 0;
        for (auto b : k.block) {
            h2 = (h2 * 131) ^ b;
        }

        return h1 ^ (h2 << 1);
    }
};





namespace stellar
{

using namespace soci;
using namespace std;

constexpr std::chrono::seconds PEER_IP_RESOLVE_DELAY(600);
constexpr std::chrono::seconds PEER_IP_RESOLVE_RETRY_DELAY(10);
constexpr std::chrono::seconds OUT_OF_SYNC_RECONNECT_DELAY(60);
constexpr uint32_t INITIAL_PEER_FLOOD_READING_CAPACITY_BYTES{300000};
constexpr uint32_t INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES{100000};






// Track per-(view, blockHash) state
struct BlockKey {
    uint64_t view;
    Hash blockHash;

    bool operator==(BlockKey const& other) const noexcept {
        return view == other.view && blockHash == other.blockHash;
    }
};

struct BlockKeyHash {
    size_t operator()(BlockKey const& k) const noexcept {
        size_t h1 = std::hash<uint64_t>()(k.view);
        size_t h2 = std::hash<std::string>()(binToHex(k.blockHash));
        return h1 ^ (h2 << 1);
    }
};

// NodeID hashing helpers
struct NodeIDHash {
    size_t operator()(NodeID const& n) const noexcept {
        return std::hash<std::string>()(KeyUtils::toStrKey(n));
    }
};
struct NodeIDEq {
    bool operator()(NodeID const& a, NodeID const& b) const noexcept {
        return a == b;
    }
};


static std::pair<uint64_t, Hash>
maxPreparedFromCollection(
    std::unordered_map<NodeID,
                       std::pair<uint64_t, Hash>,
                       NodeIDHash,
                       NodeIDEq> const& collection)
{
    uint64_t bestView = 0;
    Hash bestBlock{};

    for (auto const& [node, vb] : collection) {
        auto const& [v, b] = vb;
        if (v > bestView) {
            bestView = v;
            bestBlock = b;
        }
    }
    return {bestView, bestBlock};
}






// Per-block consensus state
struct TxnState {
    bool preparedSent = false;
    uint64_t commitView = 0;     // last view we sent a commit
    uint64_t preparedView = 0;
    uint64_t committedView = 0;
    uint64_t executedView = 0;

    bool proposalSentForView = false;


    Hash preparedBlock;
    Hash committedBlock;

    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> prepareVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> commitVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> executeVoters;


    // ====== For collection / Bracha-like broadcast ======

    // Which (vp,bp) I already echoed
    std::unordered_set<ViewBlockKey, ViewBlockKeyHash> eSent;

    // Which (vp,bp) I already readied
    std::unordered_set<ViewBlockKey, ViewBlockKeyHash> rSent;

    // Echoes received: (vp,bp) -> set of nodes who echoed
    std::unordered_map<ViewBlockKey,
                       std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       ViewBlockKeyHash> echoes;

    // Readies received: (vp,bp) -> set of nodes who readied
    std::unordered_map<ViewBlockKey,
                       std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       ViewBlockKeyHash> readies;

    // Collection: origin process p′ -> (vp,bp) it reported
    std::unordered_map<NodeID,
                       std::pair<uint64_t, Hash>,
                       NodeIDHash,
                       NodeIDEq> collection;

    // ====== NEW for conditional ready ======

    // Deferred CondReady votes: (vp,bp) -> set of nodes
    std::unordered_map<ViewBlockKey,
                       std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                       ViewBlockKeyHash> pendingCondReady;
};


static std::unordered_map<BlockKey, TxnState, BlockKeyHash> g_txn;
static std::unordered_set<BlockKey, BlockKeyHash> g_ps;

// Global tracking
static uint64_t currentView = 1;         
static uint64_t latestCommittedView = 0;
static Hash latestCommittedBlock = Hash();
static int txn_count = 0;
static int pbft_start = 0;
// static int forceCollectRound = 0;


void cleanupOldTxnStates()
{
    static const int MAX_HISTORY = 100;

    for (auto it = g_txn.begin(); it != g_txn.end(); )
    {
        // BlockKey has a .view (uint64_t) field, right?
        if (it->first.view + MAX_HISTORY < latestCommittedView)
        {
            it = g_txn.erase(it);
        }
        else
        {
            ++it;
        }
    }


    for (auto it = g_ps.begin(); it != g_ps.end(); )
    {
        if (it->view + MAX_HISTORY < latestCommittedView)
        {
            it = g_ps.erase(it);
        }
        else
        {
            ++it;
        }
    }


}














TransactionEnvelope createSCPTxFromProposal(
    Application& app,
    SecretKey const& source,
    SequenceNumber seq,
    uint64_t view,
    Hash const& blockHash,
    std::string const& data)
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
    std::string serialized = std::to_string(view) + "|" +
                            binToHex(blockHash) + "|" +
                            data;
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



// ============================================================================
// SCP TRACKING (Fair Comparison)
// ============================================================================

static bool ENABLE_SCP_TRACKING = false;

struct SCPStats {
    int totalBatches = 0;
    std::vector<int64_t> batchLatencies;
    std::chrono::steady_clock::time_point testStart;
    std::chrono::steady_clock::time_point lastBatchTime;
    static const int BATCH_SIZE = 100; // Match your custom protocol batch size
};

static SCPStats g_scpStats;



void submitNextBatchOfTransactions(Application& app)
{
    static uint32_t lastLedger = 0;  // ✅ Track last ledger we submitted for
    
    if (!ENABLE_SCP_TRACKING)
    {
        return;
    }

    if (!app.getConfig().SEND_CUSTOM_MESSAGE)
    {
        // Silently skip on nodes that shouldn't submit
        return;
    }
        
    
    uint32_t currentLedger = app.getLedgerManager().getLastClosedLedgerNum();
    
    //  Same check as before
    if (currentLedger > lastLedger)
    {
        auto submitTime = std::chrono::steady_clock::now();
        
        CLOG_INFO(Overlay, "[SCP SUBMIT] Submitting {} operations for ledger {} at t={}",
                  NUM_TEST_ACCOUNTS * 100, currentLedger + 1,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     submitTime.time_since_epoch()).count());

        
        //  Same submission logic as before
        for (int batchno = 0; batchno < 1; batchno++)
        {
            for (int accountIdx = 0; accountIdx < NUM_TEST_ACCOUNTS; accountIdx++)
            {
                submitBatchedTransactionToSCP(app, accountIdx, 100);
            }
        }
        
        lastLedger = currentLedger;  // ✅ Update tracker
    }

}





static const NodeID DUMMY_NODE_ID = NodeID(); 

bool
OverlayManagerImpl::canAcceptOutboundPeer(PeerBareAddress const& address) const
{
    if (availableOutboundPendingSlots() <= 0)
    {
        CLOG_DEBUG(Overlay,
                   "Peer rejected - all outbound pending connections "
                   "taken: {}",
                   address.toString());
        CLOG_DEBUG(Overlay, "If you wish to allow for more pending "
                            "outbound connections, please update "
                            "your MAX_PENDING_CONNECTIONS setting in "
                            "configuration file.");
        return false;
    }
    if (mShuttingDown)
    {
        CLOG_DEBUG(Overlay, "Peer rejected - overlay shutting down: {}",
                   address.toString());
        return false;
    }
    return true;
}

OverlayManagerImpl::PeersList::PeersList(
    OverlayManagerImpl& overlayManager,
    medida::MetricsRegistry& metricsRegistry,
    std::string const& directionString, std::string const& cancelledName,
    int maxAuthenticatedCount, std::shared_ptr<SurveyManager> sm)
    : mConnectionsAttempted(metricsRegistry.NewMeter(
          {"overlay", directionString, "attempt"}, "connection"))
    , mConnectionsEstablished(metricsRegistry.NewMeter(
          {"overlay", directionString, "establish"}, "connection"))
    , mConnectionsDropped(metricsRegistry.NewMeter(
          {"overlay", directionString, "drop"}, "connection"))
    , mConnectionsCancelled(metricsRegistry.NewMeter(
          {"overlay", directionString, cancelledName}, "connection"))
    , mOverlayManager(overlayManager)
    , mDirectionString(directionString)
    , mMaxAuthenticatedCount(maxAuthenticatedCount)
    , mSurveyManager(sm)
{
}

Peer::pointer
OverlayManagerImpl::PeersList::byAddress(PeerBareAddress const& address) const
{
    ZoneScoped;
    auto pendingPeerIt = std::find_if(std::begin(mPending), std::end(mPending),
                                      [address](Peer::pointer const& peer) {
                                          return peer->getAddress() == address;
                                      });
    if (pendingPeerIt != std::end(mPending))
    {
        return *pendingPeerIt;
    }

    auto authenticatedPeerIt =
        std::find_if(std::begin(mAuthenticated), std::end(mAuthenticated),
                     [address](std::pair<NodeID, Peer::pointer> const& peer) {
                         return peer.second->getAddress() == address;
                     });
    if (authenticatedPeerIt != std::end(mAuthenticated))
    {
        return authenticatedPeerIt->second;
    }

    return {};
}

void
OverlayManagerImpl::PeersList::removePeer(Peer* peer)
{
    ZoneScoped;
    CLOG_TRACE(Overlay, "Removing peer {}", peer->toString());
    peer->assertShuttingDown();

    auto pendingIt =
        std::find_if(std::begin(mPending), std::end(mPending),
                     [&](Peer::pointer const& p) { return p.get() == peer; });
    if (pendingIt != std::end(mPending))
    {
        CLOG_TRACE(Overlay, "Dropping pending {} peer: {}", mDirectionString,
                   peer->toString());
        // Prolong the lifetime of dropped peer for a bit until background
        // thread is done processing it
        mDropped.insert(*pendingIt);
        mPending.erase(pendingIt);
        mConnectionsDropped.Mark();
        return;
    }

    auto authentiatedIt = mAuthenticated.find(peer->getPeerID());
    if (authentiatedIt != std::end(mAuthenticated))
    {
        CLOG_DEBUG(Overlay, "Dropping authenticated {} peer: {}",
                   mDirectionString, peer->toString());
        // Prolong the lifetime of dropped peer for a bit until background
        // thread is done processing it
        mDropped.insert(authentiatedIt->second);
        mAuthenticated.erase(authentiatedIt);
        mConnectionsDropped.Mark();
        mSurveyManager->recordDroppedPeer(*peer);
        return;
    }

    CLOG_WARNING(Overlay, "Dropping unlisted {} peer: {}", mDirectionString,
                 peer->toString());
    CLOG_WARNING(Overlay, "{}", REPORT_INTERNAL_BUG);
}

bool
OverlayManagerImpl::PeersList::moveToAuthenticated(Peer::pointer peer)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    CLOG_TRACE(Overlay, "Moving peer {} to authenticated  state",
               peer->toString());
    auto pendingIt = std::find(std::begin(mPending), std::end(mPending), peer);
    if (pendingIt == std::end(mPending))
    {
        CLOG_WARNING(
            Overlay,
            "Trying to move non-pending {} peer {} to authenticated list",
            mDirectionString, peer->toString());
        CLOG_WARNING(Overlay, "{}", REPORT_INTERNAL_BUG);
        mConnectionsCancelled.Mark();
        return false;
    }

    auto authenticatedIt = mAuthenticated.find(peer->getPeerID());
    if (authenticatedIt != std::end(mAuthenticated))
    {
        CLOG_WARNING(Overlay,
                     "Trying to move authenticated {} peer {} to authenticated "
                     "list again",
                     mDirectionString, peer->toString());
        CLOG_WARNING(Overlay, "{}", REPORT_INTERNAL_BUG);
        mConnectionsCancelled.Mark();
        return false;
    }

    mPending.erase(pendingIt);
    mAuthenticated[peer->getPeerID()] = peer;

    CLOG_INFO(Overlay, "Authenticated to {}", peer->toString());

    mSurveyManager->modifyNodeData([&](CollectingNodeData& nodeData) {
        ++nodeData.mAddedAuthenticatedPeers;
    });

    return true;
}

bool
OverlayManagerImpl::PeersList::acceptAuthenticatedPeer(Peer::pointer peer)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    CLOG_TRACE(Overlay, "Trying to promote peer to authenticated {}",
               peer->toString());
    if (mOverlayManager.isPreferred(peer.get()))
    {
        if (mAuthenticated.size() < mMaxAuthenticatedCount)
        {
            return moveToAuthenticated(peer);
        }

        for (auto victim : mAuthenticated)
        {
            if (!mOverlayManager.isPreferred(victim.second.get()))
            {
                CLOG_INFO(
                    Overlay,
                    "Evicting non-preferred {} peer {} for preferred peer {}",
                    mDirectionString, victim.second->toString(),
                    peer->toString());
                victim.second->sendErrorAndDrop(
                    ERR_LOAD, "preferred peer selected instead");
                return moveToAuthenticated(peer);
            }
        }
    }

    if (!mOverlayManager.mApp.getConfig().PREFERRED_PEERS_ONLY &&
        mAuthenticated.size() < mMaxAuthenticatedCount)
    {
        return moveToAuthenticated(peer);
    }

    CLOG_INFO(Overlay,
              "Non preferred {} authenticated peer {} rejected because all "
              "available slots are taken.",
              mDirectionString, peer->toString());
    CLOG_INFO(
        Overlay,
        "If you wish to allow for more {} connections, please update your "
        "configuration file",
        mDirectionString);

    if (Logging::logTrace("Overlay"))
    {
        CLOG_TRACE(Overlay, "limit: {}, pending: {}, authenticated: {}",
                   mMaxAuthenticatedCount, mPending.size(),
                   mAuthenticated.size());
        std::stringstream pending, authenticated;
        for (auto p : mPending)
        {
            pending << p->toString();
            pending << " ";
        }
        for (auto p : mAuthenticated)
        {
            authenticated << p.second->toString();
            authenticated << " ";
        }
        CLOG_TRACE(Overlay, "pending: [{}] authenticated: [{}]", pending.str(),
                   authenticated.str());
    }

    mConnectionsCancelled.Mark();
    return false;
}





void
OverlayManagerImpl::PeersList::shutdown()
{
    ZoneScoped;
    auto pendingPeersToStop = mPending;
    for (auto& p : pendingPeersToStop)
    {
        p->sendErrorAndDrop(ERR_MISC, "shutdown");
    }
    auto authenticatedPeersToStop = mAuthenticated;
    for (auto& p : authenticatedPeersToStop)
    {
        p.second->sendErrorAndDrop(ERR_MISC, "shutdown");
    }

    for (auto& p : mDropped)
    {
        p->assertShuttingDown();
    }
}

std::unique_ptr<OverlayManager>
OverlayManager::create(Application& app)
{
    return std::make_unique<OverlayManagerImpl>(app);
}

OverlayManagerImpl::OverlayManagerImpl(Application& app)
    : mApp(app)
    , mLiveInboundPeersCounter(make_shared<int>(0))
    , mPeerManager(app)
    , mDoor(mApp)
    , mAuth(mApp)
    , mShuttingDown(false)
    , mOverlayMetrics(app)
    , mMessageCache(0xffff)
    , mTimer(app)
    , mPeerIPTimer(app)
    , mFloodGate(app)
    , mTxDemandsManager(app)
    , mSurveyManager(make_shared<SurveyManager>(app))
    , mInboundPeers(*this, mApp.getMetrics(), "inbound", "reject",
                    mApp.getConfig().MAX_ADDITIONAL_PEER_CONNECTIONS,
                    mSurveyManager)
    , mOutboundPeers(*this, mApp.getMetrics(), "outbound", "cancel",
                     mApp.getConfig().TARGET_PEER_CONNECTIONS, mSurveyManager)
    , mResolvingPeersWithBackoff(true)
    , mResolvingPeersRetryCount(0)
    , mScheduledMessages(100000, true)
{
    mPeerSources[PeerType::INBOUND] = std::make_unique<RandomPeerSource>(
        mPeerManager, RandomPeerSource::nextAttemptCutoff(PeerType::INBOUND));
    mPeerSources[PeerType::OUTBOUND] = std::make_unique<RandomPeerSource>(
        mPeerManager, RandomPeerSource::nextAttemptCutoff(PeerType::OUTBOUND));
    mPeerSources[PeerType::PREFERRED] = std::make_unique<RandomPeerSource>(
        mPeerManager, RandomPeerSource::nextAttemptCutoff(PeerType::PREFERRED));
}

OverlayManagerImpl::~OverlayManagerImpl()
{
}

void
OverlayManagerImpl::start()
{
    mDoor.start();
    mTimer.expires_from_now(std::chrono::seconds(2));

    if (!mApp.getConfig().RUN_STANDALONE)
    {
        mTimer.async_wait(
            [this]() {
                storeConfigPeers();
                purgeDeadPeers();
                triggerPeerResolution();
                tick();
            },
            VirtualTimer::onFailureNoop);
    }

    // Start demand logic
    mTxDemandsManager.start();
}

uint32_t
OverlayManagerImpl::getFlowControlBytesTotal() const
{
    releaseAssert(threadIsMain());
    auto const maxTxSize = mApp.getHerder().getMaxTxSize();
    releaseAssert(maxTxSize > 0);
    auto const& cfg = mApp.getConfig();

    // If flow control parameters weren't provided in the config file, calculate
    // them automatically using initial values, but adjusting them according to
    // maximum transactions byte size.
    if (cfg.PEER_FLOOD_READING_CAPACITY_BYTES == 0 &&
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES == 0)
    {
        if (!(INITIAL_PEER_FLOOD_READING_CAPACITY_BYTES -
                  INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES >=
              maxTxSize))
        {
            return maxTxSize + INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;
        }
        return INITIAL_PEER_FLOOD_READING_CAPACITY_BYTES;
    }

    // If flow control parameters were provided, return them
    return cfg.PEER_FLOOD_READING_CAPACITY_BYTES;
}

uint32_t
OverlayManager::getFlowControlBytesBatch(Config const& cfg)
{
    if (cfg.PEER_FLOOD_READING_CAPACITY_BYTES == 0 &&
        cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES == 0)
    {
        return INITIAL_FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;
    }

    // If flow control parameters were provided, return them
    return cfg.FLOW_CONTROL_SEND_MORE_BATCH_SIZE_BYTES;
}

void
OverlayManagerImpl::connectTo(PeerBareAddress const& address)
{
    ZoneScoped;
    connectToImpl(address, false);
}

bool
OverlayManagerImpl::connectToImpl(PeerBareAddress const& address,
                                  bool forceoutbound)
{
    releaseAssert(threadIsMain());
    CLOG_TRACE(Overlay, "Initiate connect to {}", address.toString());
    auto currentConnection = getConnectedPeer(address);
    if (!currentConnection || (forceoutbound && currentConnection->getRole() ==
                                                    Peer::REMOTE_CALLED_US))
    {
        if (!canAcceptOutboundPeer(address))
        {
            return false;
        }
        getPeerManager().update(address, PeerManager::BackOffUpdate::INCREASE);
        return addOutboundConnection(TCPPeer::initiate(mApp, address));
    }
    else
    {
        CLOG_ERROR(Overlay,
                   "trying to connect to a node we're already connected to {}",
                   address.toString());
        CLOG_ERROR(Overlay, "{}", REPORT_INTERNAL_BUG);
        return false;
    }
}

OverlayManagerImpl::PeersList&
OverlayManagerImpl::getPeersList(Peer* peer)
{
    ZoneScoped;
    switch (peer->getRole())
    {
    case Peer::WE_CALLED_REMOTE:
        return mOutboundPeers;
    case Peer::REMOTE_CALLED_US:
        return mInboundPeers;
    default:
        throw std::runtime_error(fmt::format(
            "Unknown peer role: {}", static_cast<int>(peer->getRole())));
    }
}

void
OverlayManagerImpl::storePeerList(std::vector<PeerBareAddress> const& addresses,
                                  bool setPreferred, bool startup)
{
    ZoneScoped;
    auto type = setPreferred ? PeerType::PREFERRED : PeerType::OUTBOUND;
    if (setPreferred)
    {
        mConfigurationPreferredPeers.clear();
    }

    for (auto const& peer : addresses)
    {
        if (setPreferred)
        {
            mConfigurationPreferredPeers.insert(peer);
        }

        if (startup)
        {
            getPeerManager().update(peer, type,
                                    /* preferredTypeKnown */ false,
                                    PeerManager::BackOffUpdate::HARD_RESET);
        }
        else
        {
            // If address is present in the DB, `update` will ensure
            // type is correctly updated. Otherwise, a new entry is created.
            // Note that this won't downgrade preferred peers back to outbound.
            getPeerManager().update(peer, type,
                                    /* preferredTypeKnown */ false);
        }
    }
}

void
OverlayManagerImpl::storeConfigPeers()
{
    ZoneScoped;
    // Synchronously resolve and store peers from the config
    storePeerList(resolvePeers(mApp.getConfig().KNOWN_PEERS).first, false,
                  true);
    storePeerList(resolvePeers(mApp.getConfig().PREFERRED_PEERS).first, true,
                  true);
}

void
OverlayManagerImpl::purgeDeadPeers()
{
    ZoneScoped;
    getPeerManager().removePeersWithManyFailures(
        Config::REALLY_DEAD_NUM_FAILURES_CUTOFF);
}

void
OverlayManagerImpl::triggerPeerResolution()
{
    ZoneScoped;
    releaseAssert(!mResolvedPeers.valid());

    // Trigger DNS resolution on the background thread
    using task_t = std::packaged_task<ResolvedPeers()>;
    std::shared_ptr<task_t> task =
        std::make_shared<task_t>([this, cfg = mApp.getConfig()]() {
            if (!this->mShuttingDown)
            {
                auto known = resolvePeers(cfg.KNOWN_PEERS);
                auto preferred = resolvePeers(cfg.PREFERRED_PEERS);
                return ResolvedPeers{known.first, preferred.first,
                                     known.second || preferred.second};
            }
            return ResolvedPeers{{}, {}, false};
        });

    mResolvedPeers = task->get_future();
    mApp.postOnBackgroundThread(bind(&task_t::operator(), task),
                                "OverlayManager: resolve peer IPs");
}

std::pair<std::vector<PeerBareAddress>, bool>
OverlayManagerImpl::resolvePeers(std::vector<string> const& peers)
{
    ZoneScoped;
    std::vector<PeerBareAddress> addresses;
    addresses.reserve(peers.size());
    bool errors = false;
    for (auto const& peer : peers)
    {
        try
        {
            addresses.push_back(PeerBareAddress::resolve(peer, mApp));
        }
        catch (std::runtime_error& e)
        {
            errors = true;
            CLOG_ERROR(Overlay, "Unable to resolve peer '{}': {}", peer,
                       e.what());
            CLOG_ERROR(Overlay, "Peer may be no longer available under "
                                "this address. Please update your "
                                "PREFERRED_PEERS and KNOWN_PEERS "
                                "settings in configuration file");
        }
    }
    return std::make_pair(addresses, errors);
}

std::vector<PeerBareAddress>
OverlayManagerImpl::getPeersToConnectTo(int maxNum, PeerType peerType)
{
    ZoneScoped;
    releaseAssert(maxNum >= 0);
    if (maxNum == 0)
    {
        return {};
    }

    auto keep = [&](PeerBareAddress const& address) {
        auto peer = getConnectedPeer(address);
        auto promote = peer && (peerType == PeerType::INBOUND) &&
                       (peer->getRole() == Peer::REMOTE_CALLED_US);
        return !peer || promote;
    };

    // don't connect to too many peers at once
    return mPeerSources[peerType]->getRandomPeers(std::min(maxNum, 50), keep);
}

int
OverlayManagerImpl::connectTo(int maxNum, PeerType peerType)
{
    ZoneScoped;
    return connectTo(getPeersToConnectTo(maxNum, peerType),
                     peerType == PeerType::INBOUND);
}

int
OverlayManagerImpl::connectTo(std::vector<PeerBareAddress> const& peers,
                              bool forceoutbound)
{
    ZoneScoped;
    auto count = 0;
    for (auto& address : peers)
    {
        if (connectToImpl(address, forceoutbound))
        {
            count++;
        }
    }
    return count;
}

void
OverlayManagerImpl::updateTimerAndMaybeDropRandomPeer(bool shouldDrop)
{
    // If we haven't heard from the network for a while, try randomly
    // disconnecting a peer in hopes of picking a better one. (preferred peers
    // aren't affected as we always want to stay connected)
    auto now = mApp.getClock().now();
    if (!mApp.getHerder().isTracking())
    {
        if (mLastOutOfSyncReconnect)
        {
            // We've been out of sync, check if it's time to drop a peer
            if (now - *mLastOutOfSyncReconnect > OUT_OF_SYNC_RECONNECT_DELAY &&
                shouldDrop)
            {
                auto allPeers = getOutboundAuthenticatedPeers();
                std::vector<std::pair<NodeID, Peer::pointer>> nonPreferredPeers;
                std::copy_if(std::begin(allPeers), std::end(allPeers),
                             std::back_inserter(nonPreferredPeers),
                             [&](auto const& peer) {
                                 return !mApp.getOverlayManager().isPreferred(
                                     peer.second.get());
                             });
                if (!nonPreferredPeers.empty())
                {
                    auto peerToDrop = rand_element(nonPreferredPeers);
                    peerToDrop.second->sendErrorAndDrop(
                        ERR_LOAD, "random disconnect due to out of sync");
                }
                // Reset the timer to throttle dropping peers
                mLastOutOfSyncReconnect =
                    std::make_optional<VirtualClock::time_point>(now);
            }
            else
            {
                // Still waiting for the timeout or outbound capacity
                return;
            }
        }
        else
        {
            // Start a timer after going out of sync. Note that we still want to
            // wait for OUT_OF_SYNC_RECONNECT_DELAY for Herder recovery logic to
            // trigger.
            mLastOutOfSyncReconnect =
                std::make_optional<VirtualClock::time_point>(now);
        }
    }
    else
    {
        // Reset timer when in-sync
        mLastOutOfSyncReconnect.reset();
    }
}



// called every PEER_AUTHENTICATION_TIMEOUT + 1=3 seconds
void
OverlayManagerImpl::tick()
{
    ZoneScoped;

    // auto nodeID = mApp.getNodeID();  // returns NodeID
    // CLOG_INFO(Overlay, "This node's ID: {}", mApp.getConfig().toShortString(nodeID));


    if (pbft_start==0)
    {


        size_t authenticatedPeers = getAuthenticatedPeersCount();
        size_t totalNodes = mApp.getConfig().KNOWN_PEERS.size(); // +1 for self
        size_t expectedPeers = totalNodes - 1;
        
        CLOG_INFO(Overlay, "authenticatedPeers,  expectedPeers: {}, {}", authenticatedPeers,  expectedPeers);

        if (authenticatedPeers == expectedPeers)

        {
            prop();
            pbft_start = 1;

            pbftStartTime = std::chrono::steady_clock::now();
            pbftStartTimeSet = true;

            CLOG_INFO(Overlay, "PBFT started — timer armed");




        }
    }


    auto rescheduleTick = gsl::finally([&]() {
        mTimer.expires_from_now(std::chrono::seconds(
            mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT + 1));
        mTimer.async_wait([this]() { this->tick(); },
                          VirtualTimer::onFailureNoop);
    });

    // Cleanup unreferenced peers.
    auto cleanupPeers = [](auto& peerList) {
        for (auto it = peerList.mDropped.begin();
             it != peerList.mDropped.end();)
        {
            auto const& p = *it;
            p->assertShuttingDown();
            if (p.use_count() == 1)
            {
                it = peerList.mDropped.erase(it);
            }
            else
            {
                ++it;
            }
        }
    };

    cleanupPeers(mInboundPeers);
    cleanupPeers(mOutboundPeers);

    if (futureIsReady(mResolvedPeers))
    {
        CLOG_TRACE(Overlay, "Resolved peers are ready");
        auto res = mResolvedPeers.get();
        storePeerList(res.known, false, false);
        storePeerList(res.preferred, true, false);
        std::chrono::seconds retryDelay = PEER_IP_RESOLVE_DELAY;

        if (mResolvingPeersWithBackoff)
        {
            // no errors -> disable retries completely from now on
            if (!res.errors)
            {
                mResolvingPeersWithBackoff = false;
            }
            else
            {
                ++mResolvingPeersRetryCount;
                auto newDelay =
                    mResolvingPeersRetryCount * PEER_IP_RESOLVE_RETRY_DELAY;
                // if we retried too many times, give up on retries
                if (newDelay > PEER_IP_RESOLVE_DELAY)
                {
                    mResolvingPeersWithBackoff = false;
                }
                else
                {
                    retryDelay = newDelay;
                }
            }
        }

        mPeerIPTimer.expires_from_now(retryDelay);
        mPeerIPTimer.async_wait([this]() { this->triggerPeerResolution(); },
                                VirtualTimer::onFailureNoop);
    }

    // Check and update the overlay survey state
    mSurveyManager->updateSurveyPhase(getInboundAuthenticatedPeers(),
                                      getOutboundAuthenticatedPeers(),
                                      mApp.getConfig());

    auto availablePendingSlots = availableOutboundPendingSlots();
    if (availablePendingSlots == 0)
    {
        // Exit early: no pending slots available
        return;
    }

    auto availableAuthenticatedSlots = availableOutboundAuthenticatedSlots();

    // First, connect to preferred peers
    {
        // in that context, an available slot is either a free slot or a non
        // preferred one
        int preferredToConnect =
            availableAuthenticatedSlots + nonPreferredAuthenticatedCount();
        preferredToConnect =
            std::min(availablePendingSlots, preferredToConnect);

        auto pendingUsedByPreferred =
            connectTo(preferredToConnect, PeerType::PREFERRED);

        releaseAssert(pendingUsedByPreferred <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByPreferred;
    }

    // Only trigger reconnecting if:
    //   * no outbound slots are available
    //   * we didn't establish any new preferred peers connections (those
    //      will evict regular peers anyway)
    bool shouldDrop =
        availableAuthenticatedSlots == 0 && availablePendingSlots > 0;
    updateTimerAndMaybeDropRandomPeer(shouldDrop);

    availableAuthenticatedSlots = availableOutboundAuthenticatedSlots();

    // Second, if there is capacity for pending and authenticated outbound
    // connections, connect to more peers. Note: connect even if
    // PREFERRED_PEER_ONLY is set, to support key-based preferred peers mode
    // (see PREFERRED_PEER_KEYS). When PREFERRED_PEER_ONLY is set and we connect
    // to a non-preferred peer, drop it and backoff during handshake.
    if (availablePendingSlots > 0 && availableAuthenticatedSlots > 0)
    {
        // try to leave at least some pending slots for peer promotion
        constexpr const auto RESERVED_FOR_PROMOTION = 1;
        auto outboundToConnect =
            availablePendingSlots > RESERVED_FOR_PROMOTION
                ? std::min(availablePendingSlots - RESERVED_FOR_PROMOTION,
                           availableAuthenticatedSlots)
                : availablePendingSlots;
        auto pendingUsedByOutbound =
            connectTo(outboundToConnect, PeerType::OUTBOUND);
        releaseAssert(pendingUsedByOutbound <= availablePendingSlots);
        availablePendingSlots -= pendingUsedByOutbound;
    }

    // Finally, attempt to promote some inbound connections to outbound
    if (availablePendingSlots > 0)
    {
        connectTo(availablePendingSlots, PeerType::INBOUND);
    }


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
            // Silently skip on nodes that shouldn't submit
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
                CLOG_WARNING(Overlay, "⚠️  IMPORTANT: This creates accounts LOCALLY, not through SCP!");
                CLOG_WARNING(Overlay, "⚠️  For proper testing, pre-create accounts in genesis ledger");
                CLOG_INFO(Overlay, "========================================");
                
                // Initialize test start time
                g_scpTxnStats.testStart = std::chrono::steady_clock::now();
                
                submitAccountCreationTransaction(mApp);
                
                g_scpStats.testStart = std::chrono::steady_clock::now();
                g_scpStats.lastBatchTime = std::chrono::steady_clock::now();
            }
            
            // Wait up to 2 ticks for account creation to complete
            initWaitTicks++;
            
            if (initWaitTicks <= 5)
            {
                CLOG_INFO(Overlay, "[SCP TXN] Waiting for local account creation... (tick {}/5)", 
                         initWaitTicks);
                return;
            }
            
            // Initialize sequence tracking
            if (!g_accountInitialized)
            {
                initializeSequenceTracking(mApp);
                if (!g_accountInitialized)
                {
                    CLOG_ERROR(Overlay, "[SCP TXN] Failed to initialize sequence tracking!");
                }
            }
            
            initializeMultipleAccounts(mApp);
            
            initialized = true;

            CLOG_INFO(Overlay, "[SCP TXN] Initialization complete, starting transaction submission");
            


            uint32_t currentLedger = mApp.getLedgerManager().getLastClosedLedgerNum();
            if (currentLedger > lastLedger && !firstOps)


            {


                // Submit FIRST batch of transactions here
                // This ensures there are transactions for ledger 1
                for (int batchno = 0; batchno < 1; batchno++)
                {
                    for (int accountIdx = 0; accountIdx < NUM_TEST_ACCOUNTS; accountIdx++)
                    {
                        submitBatchedTransactionToSCP(mApp, accountIdx, 100);
                    }
                }

                firstOps = true;
                lastLedger = currentLedger;

            }





            
            CLOG_INFO(Overlay, "[SCP TXN] Initial transaction batch submitted");
        }
        
    }

    
    // Call this - it won't access LedgerManager anymore
    // checkSCPCommits(mApp);



}



void
OverlayManagerImpl::prop()
{

    // std::this_thread::sleep_for(std::chrono::milliseconds(100));
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();

    std::string shortID = KeyUtils::toShortString(selfID);
    CLOG_DEBUG(Overlay, "shortID, txn_count: {}, {}", shortID, txn_count);


    if (mApp.getConfig().SEND_CUSTOM_MESSAGE)
    {

        CLOG_DEBUG(Overlay, "SEND_CUSTOM_MESSAGE");

        static auto lastSent = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();


        lastSent = now;




        CLOG_INFO(Overlay, "txn_count={}, latestCommittedView: {}, currentView: {}", txn_count, latestCommittedView, currentView);




        // ---- Activate forced COLLECT window after 30s ----
        if (pbftStartTimeSet && !collectWindowArmed)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(now - pbftStartTime);

            if (elapsed.count() >= FORCE_COLLECT_AFTER_SEC)
            {

                collectWindowArmed  = true;
                collectWindowActive = true;
                collectAttempts     = 0;


                CLOG_INFO(Overlay,
                        "⏱️ Forcing COLLECT for next {} views starting at view {}",
                        MAX_COLLECT_ATTEMPTS, collectWindowStartView);
            }
        }
                

        // // If window active, force COLLECT by desync
        if (collectWindowActive)
        {
            // latestCommittedView = currentView - 2;
            force_collect = true;

        }



        if (latestCommittedView == currentView - 1 && force_collect == false)
        {

            CLOG_DEBUG(Overlay, "SEND_CUSTOM_MESSAGE 2");




            const size_t BATCH_SIZE = 100;
            TransactionBatch batch;
            
            uint64_t currentTime = VirtualClock::to_time_t(mApp.getClock().system_now());
            
            for (size_t i = 0; i < BATCH_SIZE; ++i)
            {
                CustomTransaction txn;
                txn.txnId = txn_count + i;
                txn.payload = "data_" + std::to_string(txn_count + i);  // Example payload
                txn.timestamp = currentTime;
                txn.sender = shortID;
                
                batch.transactions.push_back(txn);
            }

            
            Hash blockHash = makeBlock(latestCommittedBlock, txn_count);
            // txn_count++;
            
            txn_count += BATCH_SIZE;


            CLOG_DEBUG(Overlay, "SEND_CUSTOM_MESSAGE 3");


            auto msg = std::make_shared<StellarMessage>();
            msg->type(CUSTOM_MESSAGE);

            msg->customMessage().msgType   = CUSTOM_PROPOSE;
            msg->customMessage().view      = currentView;
            msg->customMessage().blockHash = blockHash;

            
            // msg->customMessage().data      = std::to_string(txn_count);
            // msg->customMessage().data      = batch.serialize();
            msg->customMessage().data      = "";

            // msg->customMessage().data      = std::string(19000, 'X');

            CLOG_INFO(Overlay, "prop(): Leader proposing block {} in view {}",
                    hexAbbrev(blockHash), currentView);






            BlockKey key{currentView, blockHash};


            broadcastMessage(msg);

            // =====================================================
            // Option B: LOCAL handling of CUSTOM_PROPOSE (no network)
            // =====================================================
            {
                BlockKey key{currentView, blockHash};
                auto& st = g_txn[key];

                if (!st.preparedSent && latestCommittedView <= currentView - 1)
                {
                    CLOG_DEBUG(Overlay,
                            "[SELF-LOCAL] PROPOSE block {} at view {}",
                            hexAbbrev(blockHash), currentView);

                    // Local prepared state
                    st.preparedSent  = true;
                    st.preparedView  = currentView;
                    st.preparedBlock = blockHash;

                    // Track prepared set
                    g_ps.insert(key);

                    // Self prepare vote ONLY (no network)
                    st.prepareVoters.insert(selfID);

                    auto const& cm = (*msg).customMessage();

                    sendPrepare(cm.view, cm.blockHash, cm.data);

                    // Activate deferred CONDREADY votes
                    ViewBlockKey vb{st.preparedView, st.preparedBlock};
                    if (st.pendingCondReady.count(vb))
                    {
                        auto& voters = st.pendingCondReady[vb];
                        st.readies[vb].insert(voters.begin(), voters.end());
                        st.pendingCondReady.erase(vb);

                        CLOG_DEBUG(Overlay,
                                "[SELF-LOCAL] Activated {} deferred CONDREADY votes for (vp={}, bp={})",
                                voters.size(),
                                st.preparedView,
                                hexAbbrev(st.preparedBlock));
                    }
                }
            }





            


        }

        else
        {
            // ---- COLLECT (once per view) ----
            // if (lastCollectSentView != currentView)
            {
                lastCollectSentView = currentView;


                auto msg = std::make_shared<StellarMessage>();
                msg->type(CUSTOM_MESSAGE);
                msg->customMessage().msgType = CUSTOM_COLLECT;
                msg->customMessage().view    = currentView;

                CLOG_INFO(Overlay,
                        "Leader initiating COLLECT for view {}",
                        currentView);

                broadcastMessage(msg);

                //  Count ONLY real COLLECT sends
                if (collectWindowActive)
                {
                    collectAttempts++;

                    CLOG_DEBUG(Overlay,
                            "Forced COLLECT attempt {}/{}",
                            collectAttempts, MAX_COLLECT_ATTEMPTS);

                    if (collectAttempts >= MAX_COLLECT_ATTEMPTS)
                    {
                        collectWindowActive = false;
                        force_collect = false;

                        CLOG_INFO(Overlay,
                                "Forced COLLECT window ended after {} attempts",
                                MAX_COLLECT_ATTEMPTS);
                    }
                }
            }
        }


    }

    else
    {
        txn_count++;
    }

}

int
OverlayManagerImpl::availableOutboundPendingSlots() const
{
    if (mOutboundPeers.mPending.size() <
        mApp.getConfig().MAX_OUTBOUND_PENDING_CONNECTIONS)
    {
        return static_cast<int>(
            mApp.getConfig().MAX_OUTBOUND_PENDING_CONNECTIONS -
            mOutboundPeers.mPending.size());
    }
    else
    {
        return 0;
    }
}

int
OverlayManagerImpl::availableOutboundAuthenticatedSlots() const
{
    auto adjustedTarget =
        mInboundPeers.mAuthenticated.size() == 0 &&
                !mApp.getConfig()
                     .ARTIFICIALLY_SKIP_CONNECTION_ADJUSTMENT_FOR_TESTING
            ? OverlayManager::MIN_INBOUND_FACTOR
            : mApp.getConfig().TARGET_PEER_CONNECTIONS;

    if (mOutboundPeers.mAuthenticated.size() < adjustedTarget)
    {
        return static_cast<int>(adjustedTarget -
                                mOutboundPeers.mAuthenticated.size());
    }
    else
    {
        return 0;
    }
}

int
OverlayManagerImpl::nonPreferredAuthenticatedCount() const
{
    unsigned short nonPreferredCount{0};
    for (auto const& p : mOutboundPeers.mAuthenticated)
    {
        if (!isPreferred(p.second.get()))
        {
            nonPreferredCount++;
        }
    }

    releaseAssert(nonPreferredCount <=
                  mApp.getConfig().TARGET_PEER_CONNECTIONS);
    return nonPreferredCount;
}

Peer::pointer
OverlayManagerImpl::getConnectedPeer(PeerBareAddress const& address)
{
    auto outbound = mOutboundPeers.byAddress(address);
    return outbound ? outbound : mInboundPeers.byAddress(address);
}

void
OverlayManagerImpl::clearLedgersBelow(uint32_t ledgerSeq, uint32_t lclSeq)
{
    mFloodGate.clearBelow(ledgerSeq);
    mSurveyManager->clearOldLedgers(lclSeq);
    for (auto const& peer : getAuthenticatedPeers())
    {
        peer.second->clearBelow(ledgerSeq);
    }
}

void
OverlayManagerImpl::updateSizeCounters()
{

    // return;

    mOverlayMetrics.mPendingPeersSize.set_count(getPendingPeersCount());
    mOverlayMetrics.mAuthenticatedPeersSize.set_count(
        getAuthenticatedPeersCount());
}

void
OverlayManagerImpl::maybeAddInboundConnection(Peer::pointer peer)
{
    ZoneScoped;
    mInboundPeers.mConnectionsAttempted.Mark();

    if (peer)
    {
        releaseAssert(peer->getRole() == Peer::REMOTE_CALLED_US);
        bool haveSpace = haveSpaceForConnection(peer->getAddress().getIP());

        if (mShuttingDown || !haveSpace)
        {
            mInboundPeers.mConnectionsCancelled.Mark();
            peer->drop("all pending inbound connections are taken",
                       Peer::DropDirection::WE_DROPPED_REMOTE);
            mInboundPeers.mDropped.insert(peer);
            return;
        }
        CLOG_DEBUG(Overlay, "New (inbound) connected peer {}",
                   peer->toString());
        mInboundPeers.mConnectionsEstablished.Mark();
        mInboundPeers.mPending.push_back(peer);
        updateSizeCounters();
    }
    else
    {
        mInboundPeers.mConnectionsCancelled.Mark();
    }
}

bool
OverlayManagerImpl::isPossiblyPreferred(std::string const& ip) const
{
    return std::any_of(
        std::begin(mConfigurationPreferredPeers),
        std::end(mConfigurationPreferredPeers),
        [&](PeerBareAddress const& address) { return address.getIP() == ip; });
}

bool
OverlayManagerImpl::haveSpaceForConnection(std::string const& ip) const
{
    auto totalAuthenticated = getInboundAuthenticatedPeers().size();
    auto totalTracked = *getLiveInboundPeersCounter();

    size_t totalPendingCount = 0;
    if (totalTracked > totalAuthenticated)
    {
        totalPendingCount = totalTracked - totalAuthenticated;
    }
    auto adjustedInCount =
        std::max<size_t>(mInboundPeers.mPending.size(), totalPendingCount);

    auto haveSpace =
        adjustedInCount < mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS;

    if (!haveSpace &&
        adjustedInCount < mApp.getConfig().MAX_INBOUND_PENDING_CONNECTIONS +
                              Config::POSSIBLY_PREFERRED_EXTRA)
    {
        // for peers that are possibly preferred (they have the same IP as some
        // preferred peer we enocuntered in past), we allow an extra
        // Config::POSSIBLY_PREFERRED_EXTRA incoming pending connections, that
        // are not available for non-preferred peers
        haveSpace = isPossiblyPreferred(ip);
    }

    if (!haveSpace)
    {
        CLOG_DEBUG(
            Overlay,
            "Peer rejected - all pending inbound connections are taken: {}",
            ip);
        CLOG_DEBUG(Overlay, "If you wish to allow for more pending "
                            "inbound connections, please update your "
                            "MAX_PENDING_CONNECTIONS setting in "
                            "configuration file.");
    }

    return haveSpace;
}

bool
OverlayManagerImpl::addOutboundConnection(Peer::pointer peer)
{
    ZoneScoped;
    releaseAssert(peer->getRole() == Peer::WE_CALLED_REMOTE);
    mOutboundPeers.mConnectionsAttempted.Mark();

    if (!canAcceptOutboundPeer(peer->getAddress()))
    {
        mOutboundPeers.mConnectionsCancelled.Mark();
        peer->drop("all outbound connections taken",
                   Peer::DropDirection::WE_DROPPED_REMOTE);
        mOutboundPeers.mDropped.insert(peer);
        return false;
    }
    CLOG_DEBUG(Overlay, "New (outbound) connected peer {}", peer->toString());
    mOutboundPeers.mConnectionsEstablished.Mark();
    mOutboundPeers.mPending.push_back(peer);
    updateSizeCounters();

    return true;
}

void
OverlayManagerImpl::removePeer(Peer* peer)
{
    releaseAssert(threadIsMain());
    ZoneScoped;
    getPeersList(peer).removePeer(peer);
    getPeerManager().removePeersWithManyFailures(
        Config::REALLY_DEAD_NUM_FAILURES_CUTOFF, &peer->getAddress());
    updateSizeCounters();
}

bool
OverlayManagerImpl::moveToAuthenticated(Peer::pointer peer)
{
    auto result = getPeersList(peer.get()).moveToAuthenticated(peer);
    updateSizeCounters();
    return result;
}

bool
OverlayManagerImpl::acceptAuthenticatedPeer(Peer::pointer peer)
{
    return getPeersList(peer.get()).acceptAuthenticatedPeer(peer);
}

std::vector<Peer::pointer> const&
OverlayManagerImpl::getInboundPendingPeers() const
{
    return mInboundPeers.mPending;
}

std::vector<Peer::pointer> const&
OverlayManagerImpl::getOutboundPendingPeers() const
{
    return mOutboundPeers.mPending;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getPendingPeers() const
{
    auto result = mOutboundPeers.mPending;
    result.insert(std::end(result), std::begin(mInboundPeers.mPending),
                  std::end(mInboundPeers.mPending));
    return result;
}

std::map<NodeID, Peer::pointer> const&
OverlayManagerImpl::getInboundAuthenticatedPeers() const
{
    return mInboundPeers.mAuthenticated;
}

std::map<NodeID, Peer::pointer> const&
OverlayManagerImpl::getOutboundAuthenticatedPeers() const
{
    return mOutboundPeers.mAuthenticated;
}

std::map<NodeID, Peer::pointer>
OverlayManagerImpl::getAuthenticatedPeers() const
{
    auto result = mOutboundPeers.mAuthenticated;
    result.insert(std::begin(mInboundPeers.mAuthenticated),
                  std::end(mInboundPeers.mAuthenticated));
    return result;
}

std::shared_ptr<int>
OverlayManagerImpl::getLiveInboundPeersCounter() const
{
    return mLiveInboundPeersCounter;
}

int
OverlayManagerImpl::getPendingPeersCount() const
{
    return static_cast<int>(mInboundPeers.mPending.size() +
                            mOutboundPeers.mPending.size());
}

int
OverlayManagerImpl::getAuthenticatedPeersCount() const
{
    return static_cast<int>(mInboundPeers.mAuthenticated.size() +
                            mOutboundPeers.mAuthenticated.size());
}

bool
OverlayManagerImpl::isPreferred(Peer* peer) const
{
    std::string pstr = peer->toString();

    if (mConfigurationPreferredPeers.find(peer->getAddress()) !=
        mConfigurationPreferredPeers.end())
    {
        CLOG_DEBUG(Overlay, "Peer {} is preferred", pstr);
        return true;
    }

    bool isPreferred = false;
    peer->doIfAuthenticated([&]() {
        isPreferred =
            mApp.getConfig().PREFERRED_PEER_KEYS.count(peer->getPeerID()) != 0;
    });

    if (isPreferred)
    {
        CLOG_DEBUG(Overlay, "Peer key {} is preferred",
                   mApp.getConfig().toShortString(peer->getPeerID()));
        return true;
    }

    CLOG_TRACE(Overlay, "Peer {} is not preferred", pstr);
    return false;
}

static const xdr::opaque_array<32> TX_BATCH_HASH = [] {
    xdr::opaque_array<32> bytes{};
    for (auto& b : bytes)
    {
        b = 0x1;
    }
    return bytes;
}();

std::shared_ptr<StellarMessage>
OverlayManager::createTxBatch()
{
    // In testing, allow legacy TX_SET messages to represent a "batch" of
    // transactions to flood by hard-coding a special previousLedgerHash.
    auto msg = std::make_shared<StellarMessage>();
    msg->type(TX_SET);
    msg->txSet().previousLedgerHash = TX_BATCH_HASH;
    return msg;
}

bool
OverlayManager::isFloodMessage(StellarMessage const& msg)
{
    bool isFlood = msg.type() == SCP_MESSAGE || msg.type() == TRANSACTION ||
                   msg.type() == FLOOD_DEMAND || msg.type() == FLOOD_ADVERT;
#ifdef BUILD_TESTS
    isFlood = isFlood || (msg.type() == TX_SET &&
                          msg.txSet().previousLedgerHash ==
                              createTxBatch()->txSet().previousLedgerHash);
#endif

    return isFlood;
}
std::vector<Peer::pointer>
OverlayManagerImpl::getRandomAuthenticatedPeers()
{
    std::vector<Peer::pointer> result;
    result.reserve(mInboundPeers.mAuthenticated.size() +
                   mOutboundPeers.mAuthenticated.size());
    extractPeersFromMap(mInboundPeers.mAuthenticated, result);
    extractPeersFromMap(mOutboundPeers.mAuthenticated, result);
    shufflePeerList(result);
    return result;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomInboundAuthenticatedPeers()
{
    std::vector<Peer::pointer> result;
    result.reserve(mInboundPeers.mAuthenticated.size());
    extractPeersFromMap(mInboundPeers.mAuthenticated, result);
    shufflePeerList(result);
    return result;
}

std::vector<Peer::pointer>
OverlayManagerImpl::getRandomOutboundAuthenticatedPeers()
{
    std::vector<Peer::pointer> result;
    result.reserve(mOutboundPeers.mAuthenticated.size());
    extractPeersFromMap(mOutboundPeers.mAuthenticated, result);
    shufflePeerList(result);
    return result;
}

void
OverlayManagerImpl::extractPeersFromMap(
    std::map<NodeID, Peer::pointer> const& peerMap,
    std::vector<Peer::pointer>& result)
{
    auto extractPeer = [](std::pair<NodeID, Peer::pointer> const& peer) {
        return peer.second;
    };
    std::transform(std::begin(peerMap), std::end(peerMap),
                   std::back_inserter(result), extractPeer);
}

void
OverlayManagerImpl::shufflePeerList(std::vector<Peer::pointer>& peerList)
{
    stellar::shuffle(peerList.begin(), peerList.end(), getGlobalRandomEngine());
}

bool
OverlayManagerImpl::recvFloodedMsgID(Peer::pointer peer, Hash const& msgID)
{
    ZoneScoped;
    return mFloodGate.addRecord(peer, msgID);

}


void
OverlayManagerImpl::sendPrepare(uint64_t view, Hash const& blockHash, std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType   = CUSTOM_PREPARE;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;

    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "Broadcast PREPARE for block {} view {}", hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendCommit(uint64_t view, Hash const& blockHash, std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType   = CUSTOM_COMMIT;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;

    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "Broadcast COMMIT for block {} view {}", hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendExecute(uint64_t view, Hash const& blockHash, std::string const& data)
{

    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType   = CUSTOM_EXECUTE;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;

    // Send only if this node is leader (SEND_CUSTOM_MESSAGE == true)

    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "Broadcast EXECUTE for block {} view {}", hexAbbrev(blockHash), view);


}





void
OverlayManagerImpl::recvCustomMessage(StellarMessage const& stellarMsg,
                                      Peer::pointer peer)
{





    // In any handler where you need the index:
    auto computeNodeIndex = [this]() {
        NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
        std::vector<NodeID> allNodes;
        allNodes.push_back(selfID);
        
        auto authenticatedPeers = getAuthenticatedPeers();
        for (auto const& peer : authenticatedPeers)
        {
            allNodes.push_back(peer.first);
        }
        
        std::sort(allNodes.begin(), allNodes.end());
        auto it = std::find(allNodes.begin(), allNodes.end(), selfID);
        return it != allNodes.end() ? std::distance(allNodes.begin(), it) : 0;
    };








    auto const& cm = stellarMsg.customMessage();
    NodeID sender = peer->getPeerID();
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();

    BlockKey key{cm.view, cm.blockHash};
    auto& st = g_txn[key];

    size_t N = getAuthenticatedPeersCount() + 1;
    size_t f = (N - 1) / 3;

    switch (cm.msgType)
    {
        // ================================================================
        case CUSTOM_PROPOSE:
        if (cm.view >= currentView)
        {
            CLOG_INFO(Overlay, "Received PROPOSE block {} at view {}",
                      hexAbbrev(cm.blockHash), cm.view);

            CLOG_INFO(Overlay, "OverlayManagerImpl tick; MEMORY RSS: {} MB, number of elements: {}", getRSS_MB(),g_txn.size());
            

            if (!st.preparedSent && latestCommittedView <= cm.view - 1)
            {
                st.preparedSent = true;
                st.preparedView = cm.view;
                st.preparedBlock = cm.blockHash;

                g_ps.insert(BlockKey{cm.view, cm.blockHash});

                // Self counts as prepare voter
                st.prepareVoters.insert(selfID);
                sendPrepare(cm.view, cm.blockHash, cm.data);

                // 🔹 Activate deferred CondReady votes for this (vp,bp)
                ViewBlockKey vb{st.preparedView, st.preparedBlock};
                if (st.pendingCondReady.count(vb))
                {
                    auto& voters = st.pendingCondReady[vb];
                    st.readies[vb].insert(voters.begin(), voters.end());
                    st.pendingCondReady.erase(vb);

                    CLOG_INFO(Overlay,
                              "Activated {} deferred CONDREADY votes for (vp={}, bp={})",
                              voters.size(),
                              st.preparedView, hexAbbrev(st.preparedBlock));
                }
            }
        }
        else
        {
            CLOG_INFO(Overlay, "Ignoring PROPOSE at view {} (current={})",
                      cm.view, currentView); // 🔹 ignore old/future proposals

                    //   while((cm.view != currentView))
                    //   {

                    //   }
        }
        break;


        // ================================================================
        case CUSTOM_PREPARE:

            st.prepareVoters.insert(sender);

            CLOG_DEBUG(Overlay, "Received PREPARE block {} at view {} with st.prepareVoters: {} ",
                      hexAbbrev(cm.blockHash), cm.view, st.prepareVoters.size());
            if (st.prepareVoters.size() >= 2*f + 1 && st.commitView < cm.view)
            {
                st.commitView = cm.view;
                st.prepareVoters.insert(selfID); // self vote
                st.commitVoters.insert(selfID);
                sendCommit(cm.view, cm.blockHash, cm.data);
            }
            break;

        // ================================================================
        case CUSTOM_COMMIT:

            CLOG_DEBUG(Overlay, "Received COMMIT block {} at view {}, with my node index: {}",
                      hexAbbrev(cm.blockHash), cm.view, computeNodeIndex());


            // if (mApp.getConfig().MEMORY_PROF && cm.view > 30000 && cm.view %10000 < 3000)
            // {
            //     return;
            // }
            

            st.commitVoters.insert(sender);

            if (st.commitVoters.size() >= f + 1 && st.commitView < cm.view)
            {
                st.commitView = cm.view;
                st.commitVoters.insert(selfID);
                sendCommit(cm.view, cm.blockHash, cm.data); // amplify
            }
            if (st.commitVoters.size() >= 2*f + 1 && st.committedView < cm.view)
            {
                st.committedView = cm.view;
                st.committedBlock = cm.blockHash;

                latestCommittedView = cm.view;
                latestCommittedBlock = cm.blockHash;
                

                currentView = cm.view + 1;
                



                BlockKey nextKey{currentView, Hash()};
                g_txn[nextKey].proposalSentForView = false;

                // CLOG_INFO(Overlay, "Committed block {} at view {}",
                //           hexAbbrev(cm.blockHash), cm.view);



                // ✅ Deserialize and print all transactions in the batch
                TransactionBatch batch = TransactionBatch::deserialize(cm.data);
                
                CLOG_DEBUG(Overlay, "========================================");
                // CLOG_INFO(Overlay, "Committed block {} at view {} with {} transactions",
                //         hexAbbrev(cm.blockHash), cm.view, batch.transactions.size());
                CLOG_INFO(Overlay, "Committed block {} at view {} with {} transactions",
                        hexAbbrev(cm.blockHash), cm.view, 100);

                CLOG_DEBUG(Overlay, "========================================");


                
                // ✅ Print each transaction's details
                for (size_t i = 0; i < batch.transactions.size(); ++i)
                {
                    const auto& txn = batch.transactions[i];
                    CLOG_DEBUG(Overlay, "  Txn[{}]: ID={}, Payload={}, Timestamp={}, Sender={}",
                            i, txn.txnId, txn.payload, txn.timestamp, txn.sender);
                }
                
                CLOG_INFO(Overlay, "========================================");

                
                // cleanupOldTxnStates();

                prop();

            }
            break;


        // ================================================================
        case CUSTOM_COLLECT:
            CLOG_INFO(Overlay, "Received COLLECT for view {} from {}",
                    cm.view, KeyUtils::toShortString(sender));

            {
                uint64_t vp = st.preparedView;
                Hash bp = st.preparedBlock;


                if (st.preparedView > 0) {
                    // Best: reply with prepared info
                    vp = st.preparedView;
                    bp = st.preparedBlock;
                } else if (latestCommittedView > 0) {
                    // Fallback: reply with last committed
                    vp = latestCommittedView;
                    bp = latestCommittedBlock;
                } else {
                    // Nothing yet, fallback to genesis
                    vp = 0;
                    bp = Hash();  // all zeros
                }


                //  Step 1: Broadcast SEND (for others)
                auto sendMsg = std::make_shared<StellarMessage>();
                sendMsg->type(CUSTOM_MESSAGE);
                sendMsg->customMessage().msgType    = CUSTOM_SEND;
                sendMsg->customMessage().view       = cm.view;
                sendMsg->customMessage().vp         = vp;
                sendMsg->customMessage().bp         = bp;
                sendMsg->customMessage().origin     = selfID;
                broadcastMessage(sendMsg);

            }
            break;


        // ================================================================
        case CUSTOM_SEND:
            CLOG_INFO(Overlay, "Received SEND (vp={}, bp={}) for view {} from {} (origin={})",
                      cm.vp, hexAbbrev(cm.bp), cm.view,
                      KeyUtils::toShortString(sender),
                      KeyUtils::toShortString(cm.origin));

            {
                ViewBlockKey vb{cm.vp, cm.bp};
                if (!st.eSent.count(vb))
                {
                    st.eSent.insert(vb);



                    auto msg = std::make_shared<StellarMessage>();
                    msg->type(CUSTOM_MESSAGE);
                    msg->customMessage().msgType    = CUSTOM_ECHO;
                    msg->customMessage().view       = cm.view;
                    msg->customMessage().vp         = cm.vp;
                    msg->customMessage().bp         = cm.bp;
                    msg->customMessage().origin     = cm.origin;
                    broadcastMessage(msg);
                    st.echoes[vb].insert(selfID);

                    CLOG_INFO(Overlay, "Sending ECHO (vp={}, bp={}) for view {} from {} (origin={})",
                    cm.vp, hexAbbrev(cm.bp), cm.view,
                    KeyUtils::toShortString(sender),
                    KeyUtils::toShortString(cm.origin));

                }
            }
            break;

        // ================================================================
        case CUSTOM_ECHO:
            CLOG_INFO(Overlay, "Received ECHO (vp={}, bp={}) for view {} from {} (origin={})",
                      cm.vp, hexAbbrev(cm.bp), cm.view,
                      KeyUtils::toShortString(sender),
                      KeyUtils::toShortString(cm.origin));

            {
                ViewBlockKey vb{cm.vp, cm.bp};
                st.echoes[vb].insert(sender);

                CLOG_INFO(Overlay, "st.echoes[vb].size():  {} (origin={})",
                st.echoes[vb].size(),
                KeyUtils::toShortString(cm.origin));



                if (st.echoes[vb].size() >= 2*f+1 && !st.rSent.count(vb))
                {

                    CLOG_INFO(Overlay, "want to send READY MSG");

                    if (g_ps.count(BlockKey{cm.vp, cm.bp}))
                    {
                        st.rSent.insert(vb);

                        CLOG_INFO(Overlay, "Sending READY MSG");
                        // I prepared it → send READY
                        auto msg = std::make_shared<StellarMessage>();
                        msg->type(CUSTOM_MESSAGE);
                        msg->customMessage().msgType    = CUSTOM_READY;
                        msg->customMessage().view       = cm.view;
                        msg->customMessage().vp         = cm.vp;
                        msg->customMessage().bp         = cm.bp;
                        msg->customMessage().origin     = cm.origin;
                        broadcastMessage(msg);
                        st.readies[vb].insert(selfID);


                    }
                    else if (cm.vp < st.preparedView)
                    {
                        st.rSent.insert(vb);

                        CLOG_INFO(Overlay, "Sending CONDREADY MSG");
                        // I have higher prepared → send CONDREADY
                        auto msg = std::make_shared<StellarMessage>();
                        msg->type(CUSTOM_MESSAGE);
                        msg->customMessage().msgType    = CUSTOM_CONDREADY;
                        msg->customMessage().view       = cm.view;
                        msg->customMessage().vp         = st.preparedView;
                        msg->customMessage().bp         = st.preparedBlock;
                        msg->customMessage().origin     = cm.origin;
                        broadcastMessage(msg);

                        st.readies[vb].insert(selfID);
                    }
                }
            }
            break;

        // ================================================================
        case CUSTOM_READY:
            CLOG_INFO(Overlay, "Received READY (vp={}, bp={}) for view {} from {}",
                    cm.vp, hexAbbrev(cm.bp), cm.view,
                    KeyUtils::toShortString(sender));
            {
                ViewBlockKey vb{cm.vp, cm.bp};
                st.readies[vb].insert(sender);
                
                // ✅ Record ONCE - use sender, not origin
                st.collection[sender] = {cm.vp, cm.bp};
                
                CLOG_INFO(Overlay, "Added sender={} with (vp={}, bp={}) to collection (size={}, readies={})",
                        KeyUtils::toShortString(sender),
                        cm.vp, hexAbbrev(cm.bp), 
                        st.collection.size(),
                        st.readies[vb].size());
                
                //  Check if threshold reached AND we haven't proposed yet
                if (st.readies[vb].size() == 2*f+1 &&  // ← Changed >= to ==
                    selfID == mApp.getConfig().NODE_SEED.getPublicKey() &&
                    !st.proposalSentForView && mApp.getConfig().SEND_CUSTOM_MESSAGE)  // ← Added this check
                {
                    auto [maxView, maxBlock] = maxPreparedFromCollection(st.collection);
                    Hash newBlock = makeBlock(maxBlock, txn_count++);
                    
                    auto msg = std::make_shared<StellarMessage>();
                    msg->type(CUSTOM_MESSAGE);
                    msg->customMessage().msgType   = CUSTOM_PROPOSE;
                    msg->customMessage().view      = currentView;
                    msg->customMessage().blockHash = newBlock;
                    msg->customMessage().data      = std::to_string(txn_count);

                    broadcastMessage(msg);
                    
                    st.proposalSentForView = true;  // Set flag
                    
                    // CLOG_INFO(Overlay, "Leader proposing new block {} in view {} (extending vp={})",
                    //         hexAbbrev(newBlock), currentView, maxView);

                    CLOG_INFO(Overlay, "CUSTOM_READY");

                    CLOG_INFO(Overlay, "CUSTOM_READY: Leader proposing block {} in view {}",
                    hexAbbrev(newBlock), currentView);

                }
            }
            break;

        // ================================================================
        case CUSTOM_CONDREADY:
            CLOG_INFO(Overlay, "Received CONDREADY (vp={}, bp={}) for view {} from {}",
                    cm.vp, hexAbbrev(cm.bp), cm.view,
                    KeyUtils::toShortString(sender));
            {
                ViewBlockKey vb{cm.vp, cm.bp};
                
                // Record vote
                st.collection[sender] = {cm.vp, cm.bp};
                CLOG_INFO(Overlay, "Added CONDREADY sender={} (vp={}, bp={}) to collection (size={})",
                        KeyUtils::toShortString(sender),
                        cm.vp, hexAbbrev(cm.bp), st.collection.size());
                
                if (g_ps.count(BlockKey{cm.vp, cm.bp}))
                {
                    st.readies[vb].insert(sender);
                    CLOG_INFO(Overlay, "CONDREADY immediately counted for (vp={}, bp={}), readies={}",
                            cm.vp, hexAbbrev(cm.bp), st.readies[vb].size());
                    
                    // ✅ Check threshold with flag
                    if (st.readies[vb].size() == 2*f+1 &&  // ← Changed >= to ==
                        selfID == mApp.getConfig().NODE_SEED.getPublicKey() &&
                        !st.proposalSentForView)  // ← Added this check
                    {
                        auto [maxView, maxBlock] = maxPreparedFromCollection(st.collection);
                        Hash newBlock = makeBlock(maxBlock, txn_count++);
                        
                        auto msg = std::make_shared<StellarMessage>();
                        msg->type(CUSTOM_MESSAGE);
                        msg->customMessage().msgType   = CUSTOM_PROPOSE;
                        msg->customMessage().view      = currentView;
                        msg->customMessage().blockHash = newBlock;
                        msg->customMessage().data      = std::to_string(txn_count);
                        broadcastMessage(msg);
                        
                        st.proposalSentForView = true;  // ✅ Set flag

                        CLOG_INFO(Overlay, "CUSTOM_COND_READY");
                        CLOG_INFO(Overlay, "CUSTOM_CONDREADY: Leader proposing block {} in view {}",
                        hexAbbrev(newBlock), currentView);



                    }
                }
                else
                {
                    st.pendingCondReady[vb].insert(sender);
                    CLOG_INFO(Overlay, "CONDREADY deferred for (vp={}, bp={})",
                            cm.vp, hexAbbrev(cm.bp));
                }
            }
            break;
    }
}



bool
OverlayManagerImpl::checkScheduledAndCache(
    std::shared_ptr<CapacityTrackedMessage> tracker)
{
#ifndef BUILD_TESTS
    releaseAssert(!threadIsMain() ||
                  !mApp.getConfig().BACKGROUND_OVERLAY_PROCESSING);
#endif
    if (!tracker->maybeGetHash())
    {
        return false;
    }
    auto index = tracker->maybeGetHash().value();
    if (mScheduledMessages.exists(index))
    {
        if (mScheduledMessages.get(index).lock())
        {
            return true;
        }
    }
    mScheduledMessages.put(index,
                           std::weak_ptr<CapacityTrackedMessage>(tracker));
    return false;
}

void
OverlayManagerImpl::recvTransaction(TransactionFrameBasePtr transaction,
                                    Peer::pointer peer, Hash const& index)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    if (transaction)
    {
        // record that this peer sent us this transaction
        // add it to the floodmap so that this peer gets credit for it
        recvFloodedMsgID(peer, index);
        mTxDemandsManager.recordTxPullLatency(transaction->getFullHash(), peer);

        // add it to our current set
        // and make sure it is valid
        auto addResult = mApp.getHerder().recvTransaction(transaction, false);
        bool pulledRelevantTx = false;
        if (!(addResult.code ==
                  TransactionQueue::AddResultCode::ADD_STATUS_PENDING ||
              addResult.code ==
                  TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE))
        {
            forgetFloodedMsg(index);
            CLOG_DEBUG(Overlay,
                       "Peer::recvTransaction Discarded transaction {} from {}",
                       hexAbbrev(transaction->getFullHash()), peer->toString());
        }
        else
        {
            bool dup = addResult.code ==
                       TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE;
            if (!dup)
            {
                pulledRelevantTx = true;
            }
            CLOG_DEBUG(
                Overlay,
                "Peer::recvTransaction Received {} transaction {} from {}",
                (dup ? "duplicate" : "unique"),
                hexAbbrev(transaction->getFullHash()), peer->toString());
        }

        auto const& om = getOverlayMetrics();
        auto& meter =
            pulledRelevantTx ? om.mPulledRelevantTxs : om.mPulledIrrelevantTxs;
        meter.Mark();
    }
}

void
OverlayManagerImpl::forgetFloodedMsg(Hash const& msgID)
{
    ZoneScoped;
    mFloodGate.forgetRecord(msgID);
}

void
OverlayManagerImpl::recvTxDemand(FloodDemand const& dmd, Peer::pointer peer)
{
    ZoneScoped;
    mTxDemandsManager.recvTxDemand(dmd, peer);
}

bool
OverlayManagerImpl::broadcastMessage(std::shared_ptr<StellarMessage const> msg,
                                     std::optional<Hash> const hash)
{
    ZoneScoped;
    auto res = mFloodGate.broadcast(msg, hash);
    if (res)
    {
        // mOverlayMetrics.mMessagesBroadcast.Mark();
    }
    return res;
}

void
OverlayManager::dropAll(Database& db)
{
    PeerManager::dropAll(db);
}

std::set<Peer::pointer>
OverlayManagerImpl::getPeersKnows(Hash const& h)
{
    return mFloodGate.getPeersKnows(h);
}

OverlayMetrics&
OverlayManagerImpl::getOverlayMetrics()
{
    return mOverlayMetrics;
}

PeerAuth&
OverlayManagerImpl::getPeerAuth()
{
    return mAuth;
}

PeerManager&
OverlayManagerImpl::getPeerManager()
{
    return mPeerManager;
}

SurveyManager&
OverlayManagerImpl::getSurveyManager()
{
    return *mSurveyManager;
}

void
OverlayManagerImpl::shutdown()
{
    if (mShuttingDown)
    {
        return;
    }
    mDoor.close();
    mFloodGate.shutdown();
    mInboundPeers.shutdown();
    mOutboundPeers.shutdown();
    mTxDemandsManager.shutdown();

    // Switch overlay to "shutting down" state _after_ shutting down peers to
    // allow graceful connection drop
    mShuttingDown = true;

    // Stop ticking and resolving peers
    mTimer.cancel();
    mPeerIPTimer.cancel();
}

bool
OverlayManagerImpl::isShuttingDown() const
{
    return mShuttingDown;
}

void
OverlayManagerImpl::recordMessageMetric(StellarMessage const& stellarMsg,
                                        Peer::pointer peer)
{

    ZoneScoped;

    releaseAssert(threadIsMain());
    


    // added return to prevent metrics
    return; 

    auto logMessage = [&](bool unique, std::string const& msgType) {
        CLOG_TRACE(Overlay, "recv: {} {} ({}) of size: {} from: {}",
                   (unique ? "unique" : "duplicate"),
                   peer->msgSummary(stellarMsg), msgType,
                   xdr::xdr_argpack_size(stellarMsg),
                   mApp.getConfig().toShortString(peer->getPeerID()));
    };

    bool flood = false;
    if (isFloodMessage(stellarMsg) ||
        stellarMsg.type() == TIME_SLICED_SURVEY_START_COLLECTING ||
        stellarMsg.type() == TIME_SLICED_SURVEY_STOP_COLLECTING ||
        stellarMsg.type() == TIME_SLICED_SURVEY_REQUEST ||
        stellarMsg.type() == TIME_SLICED_SURVEY_RESPONSE)
    {
        flood = true;
    }
    else if (stellarMsg.type() != TX_SET &&
             stellarMsg.type() != GENERALIZED_TX_SET &&
             stellarMsg.type() != SCP_QUORUMSET)
    {
        return;
    }

    auto& peerMetrics = peer->getPeerMetrics();

    size_t size = xdr::xdr_argpack_size(stellarMsg);
    auto hash = shortHash::xdrComputeHash(stellarMsg);
    if (mMessageCache.exists(hash))
    {
        if (flood)
        {
            mOverlayMetrics.mDuplicateFloodBytesRecv.Mark(size);

            peerMetrics.mDuplicateFloodBytesRecv += size;
            ++peerMetrics.mDuplicateFloodMessageRecv;

            logMessage(false, "flood");
        }
        else
        {
            mOverlayMetrics.mDuplicateFetchBytesRecv.Mark(size);

            peerMetrics.mDuplicateFetchBytesRecv += size;
            ++peerMetrics.mDuplicateFetchMessageRecv;

            logMessage(false, "fetch");
        }
    }
    else
    {
        // NOTE: false is used here as a placeholder value, since no value is
        // needed.
        mMessageCache.put(hash, false);
        if (flood)
        {
            mOverlayMetrics.mUniqueFloodBytesRecv.Mark(size);

            peerMetrics.mUniqueFloodBytesRecv += size;
            ++peerMetrics.mUniqueFloodMessageRecv;

            logMessage(true, "flood");
        }
        else
        {
            mOverlayMetrics.mUniqueFetchBytesRecv.Mark(size);

            peerMetrics.mUniqueFetchBytesRecv += size;
            ++peerMetrics.mUniqueFetchMessageRecv;

            logMessage(true, "fetch");
        }
    }
}

SearchableSnapshotConstPtr&
OverlayManagerImpl::getOverlayThreadSnapshot()
{
    releaseAssert(mApp.threadIsType(Application::ThreadType::OVERLAY));
    if (!mOverlayThreadSnapshot)
    {
        // Create a new snapshot
        mOverlayThreadSnapshot = mApp.getBucketManager()
                                     .getBucketSnapshotManager()
                                     .copySearchableLiveBucketListSnapshot();
    }
    return mOverlayThreadSnapshot;
}

}