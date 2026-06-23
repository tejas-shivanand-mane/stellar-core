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


#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "overlay/CustomProtocolTypes.h"
#include "overlay/CustomYCSBWorkload.h"


static constexpr size_t SERVER_BATCH_SIZE = 100;


static std::atomic<bool> g_propPending{false};
std::map<uint64_t, std::vector<std::pair<StellarMessage, Peer::pointer>>> g_futureFastMsgs;

namespace {

    // Memory management
    constexpr uint64_t MAX_VIEW_HISTORY = 1000;      // Views to keep in memory
}


static bool PBFT_MODE = false;
static bool ITHS_MODE = false;


size_t N = 0;
size_t f = 0;


static uint64_t g_ithsLockView  = 0;
static Hash     g_ithsLockBlock = Hash();

static std::unordered_map<uint64_t, Hash> g_ithsEchoSentForView;
static std::unordered_map<uint64_t, Hash> g_ithsAcceptSentForView;
static std::unordered_map<uint64_t, Hash> g_ithsLockSentForView;
static std::unordered_map<uint64_t, Hash> g_ithsCommitSentForView;

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
static std::chrono::steady_clock::time_point shabdizStartTime;
static bool shabdizStartTimeSet = false;

static bool collectWindowActive = false;
static uint64_t collectWindowStartView = 0;

static uint64_t collectAttempts = 0;
static constexpr uint64_t MAX_COLLECT_ATTEMPTS = 50;


constexpr int FORCE_COLLECT_AFTER_SEC = 100;

static bool collectWindowArmed = false;
static uint64_t lastCollectSentView = UINT64_MAX;

static bool force_collect = false;




static std::unordered_map<std::string, std::string> g_kvStore;





static int                g_clientListenerFd = -1;
static std::vector<int>   g_clientFds;
static std::mutex         g_clientFdsMutex;




static bool               g_clientListenerActive = false;



static std::mutex g_clientRequestMutex;
static std::queue<PendingClientRequest> g_clientRequestQueue;

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


struct OriginViewBlockKey
{
    NodeID origin;
    uint64_t view;
    Hash block;

    bool operator==(OriginViewBlockKey const& other) const noexcept
    {
        return origin == other.origin &&
               view == other.view &&
               block == other.block;
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

struct PendingCondReadyVote
{
    NodeID sender;
    OriginViewBlockKey target;
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

struct ClientAck
{
    uint64_t batchId;
    int clientFd;
};

static std::unordered_map<BlockKey,
                          std::vector<ClientAck>,
                          BlockKeyHash> g_blockClientAcks;

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


struct OriginViewBlockKeyHash
{
    size_t operator()(OriginViewBlockKey const& k) const noexcept
    {
        size_t h0 = NodeIDHash{}(k.origin);
        size_t h1 = std::hash<uint64_t>()(k.view);

        size_t h2 = 0;
        for (auto b : k.block)
        {
            h2 = (h2 * 131) ^ b;
        }

        return h0 ^ (h1 << 1) ^ (h2 << 2);
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

    bool ithsEchoSent   = false;
    bool ithsAcceptSent = false;
    bool ithsLockSent   = false;
    bool ithsCommitSent = false;

    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsEchoVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsAcceptVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsLockVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> ithsCommitVoters;



    Hash preparedBlock;
    Hash committedBlock;

    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> prepareVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> commitVoters;
    std::unordered_set<NodeID, NodeIDHash, NodeIDEq> executeVoters;


    // ====== For collection / Bracha-like broadcast ======

    // Which origin/value I already echoed.
    std::unordered_set<OriginViewBlockKey, OriginViewBlockKeyHash> eSent;

    // Which origin/value I already readied or cond-readied.
    std::unordered_set<OriginViewBlockKey, OriginViewBlockKeyHash> rSent;

    // Echoes received for one origin's reported prepared value.
    std::unordered_map<OriginViewBlockKey,
                    std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                    OriginViewBlockKeyHash> echoes;

    // Ready votes received for one origin's reported prepared value.
    std::unordered_map<OriginViewBlockKey,
                    std::unordered_set<NodeID, NodeIDHash, NodeIDEq>,
                    OriginViewBlockKeyHash> readies;

    // Delivered origin/value pairs, to avoid recording collection repeatedly.
    std::unordered_set<OriginViewBlockKey, OriginViewBlockKeyHash> delivered;

    // Collection: origin process p′ -> (vp,bp) it reported
    std::unordered_map<NodeID,
                       std::pair<uint64_t, Hash>,
                       NodeIDHash,
                       NodeIDEq> collection;

    // ====== NEW for conditional ready ======

    // Deferred CondReady votes:
    // dependency (vp,bp) -> list of CONDREADY votes waiting for that dependency.
    std::unordered_map<ViewBlockKey,
                    std::vector<PendingCondReadyVote>,
                    ViewBlockKeyHash> pendingCondReady;
};


static std::unordered_map<BlockKey, TxnState, BlockKeyHash> g_txn;
static std::unordered_set<BlockKey, BlockKeyHash> g_ps;



static void
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

        CLOG_DEBUG(Overlay,
                   "[CLIENT ACK] batchId={} fd={} block={} view={}",
                   ack.batchId,
                   ack.clientFd,
                   hexAbbrev(blockHash),
                   view);
    }

    g_blockClientAcks.erase(it);
}








// Global tracking
static uint64_t currentView = 1;         
static uint64_t latestCommittedView = 0;
static Hash latestCommittedBlock = Hash();
static int txn_count = 0;
static int shabdiz_start = 0;



static uint64_t g_csentView = 0; // algorithm variable csent: last view where COMMIT was sent
static std::unordered_set<uint64_t> g_fastProposedViews;
// static int forceCollectRound = 0;


static uint64_t g_localPreparedView = 0;
static Hash g_localPreparedBlock = Hash();

static void
rememberLocalPrepared(uint64_t view, Hash const& block)
{
    if (view >= g_localPreparedView)
    {
        g_localPreparedView = view;
        g_localPreparedBlock = block;
    }
}


void cleanupOldTxnStates()
{


    if (PBFT_MODE) return;

    static const int MAX_HISTORY = MAX_VIEW_HISTORY;

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




    if (shabdiz_start == 0)
    {
        size_t authenticatedPeers = getAuthenticatedPeersCount();
        size_t totalNodes = mApp.getConfig().KNOWN_PEERS.size();
        size_t expectedPeers = totalNodes - 1;

        CLOG_INFO(Overlay,
                "authenticatedPeers, expectedPeers: {}, {}",
                authenticatedPeers,
                expectedPeers);

        // Strict condition: start only after all nodes are connected.
        if (authenticatedPeers == expectedPeers)
        {
            if (mApp.getConfig().SEND_CUSTOM_MESSAGE &&
                !ENABLE_SCP_TRACKING &&
                !g_clientListenerActive)
            {
                startClientListener(12000);
                CLOG_INFO(Overlay, "[SHABDIZ READY] Client listener started after all peers connected");
            }

            shabdiz_start = 1;

            // shabdizStartTime = std::chrono::steady_clock::now();
            // shabdizStartTimeSet = true;

            CLOG_INFO(Overlay, "Shabdiz started — all peers connected, timer armed");
        }
    }

    // Retry proposal only after all peers are connected and listener is active.
    // This prevents the startup race where prop() runs before the client queue has enough requests.
    if (shabdiz_start == 1 &&
        mApp.getConfig().SEND_CUSTOM_MESSAGE &&
        g_clientListenerActive &&
        latestCommittedView == currentView - 1)
    {
        prop();
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
OverlayManagerImpl::startClientListener(int port)
{
    g_clientListenerFd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(g_clientListenerFd, SOL_SOCKET,
               SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(g_clientListenerFd,
             (sockaddr*)&addr, sizeof(addr)) < 0)
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
            if (clientFd < 0) break;

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
                    if (n <= 0) break;

                    uint32_t len = ntohl(lenNet);
                    if (len == 0 || len > 1024 * 1024) break;

                    // Read data
                    std::string data(len, '\0');
                    n = recv(clientFd, &data[0], len, MSG_WAITALL);
                    if (n <= 0) break;

                    // Parse: requestId|singleTxn
                    size_t sep = data.find('|');
                    if (sep == std::string::npos)
                    {
                        CLOG_WARNING(Overlay, "[CLIENT REJECT] missing request separator");
                        continue;
                    }

                    uint64_t requestId = 0;
                    try
                    {
                        requestId = std::stoull(data.substr(0, sep));
                    }
                    catch (...)
                    {
                        CLOG_WARNING(Overlay, "[CLIENT REJECT] malformed requestId");
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
                                    "[CLIENT REJECT] malformed transaction for requestId={}",
                                    requestId);
                        continue;
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
                            "[CLIENT REQUEST QUEUE] queued requestId={} queueSize={} serverBatchSize={}",
                            requestId,
                            queueSize,
                            SERVER_BATCH_SIZE);

                    // Trigger prop() only when enough individual requests have accumulated
                    // to form one server-side block.
                    if (queueSize >= SERVER_BATCH_SIZE)
                    {
                        bool expected = false;
                        if (g_propPending.compare_exchange_strong(expected, true))
                        {
                            mApp.postOnMainThread([this]() {
                                prop();
                                g_propPending.store(false);
                            }, "client-triggered server-batch prop");
                        }
                    }

                }

                CLOG_INFO(Overlay, "Client disconnected fd={}", clientFd);
                close(clientFd);

                std::lock_guard<std::mutex> lock(g_clientFdsMutex);
                g_clientFds.erase(
                    std::remove(g_clientFds.begin(),
                                g_clientFds.end(), clientFd),
                    g_clientFds.end());
            }).detach();
        }
    }).detach();
}



static void
updateForcedCollectState()
{
    if (shabdizStartTimeSet && !collectWindowArmed)
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(
                now - shabdizStartTime);

        if (elapsed.count() >= FORCE_COLLECT_AFTER_SEC)
        {
            collectWindowArmed  = true;
            collectWindowActive = true;
            collectAttempts     = 0;

            CLOG_INFO(Overlay,
                      "⏱️ Forcing COLLECT for next {} views starting at view {}",
                      MAX_COLLECT_ATTEMPTS,
                      collectWindowStartView);
        }
    }

    if (collectWindowActive)
    {
        force_collect = true;
    }
}

static TransactionBatch
makeSyntheticYCSBBatch(Application& app,
                       std::string const& senderShortID,
                       uint64_t startTxnId,
                       size_t batchSize)
{
    TransactionBatch batch;

    uint64_t currentTime =
        VirtualClock::to_time_t(app.getClock().system_now());

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

static std::shared_ptr<StellarMessage>
makeProposalMessage(bool ithsMode,
                    uint64_t view,
                    Hash const& blockHash,
                    uint64_t parentView,
                    Hash const& parentBlock,
                    std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);

    msg->customMessage().msgType =
        ithsMode ? CUSTOM_ITHS_PROPOSE : CUSTOM_PROPOSE;

    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;

    // Parent metadata for Shabdiz fast-path validation.
    msg->customMessage().vp = parentView;
    msg->customMessage().bp = parentBlock;

    msg->customMessage().data = data;

    return msg;
}


static size_t
getClientRequestQueueSize()
{
    std::lock_guard<std::mutex> lock(g_clientRequestMutex);
    return g_clientRequestQueue.size();
}

static bool
tryBuildServerBatch(TransactionBatch& batch,
                    std::vector<ClientAck>& clientAcks)
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


void
OverlayManagerImpl::prop()
{
    NodeID selfID = mApp.getConfig().NODE_SEED.getPublicKey();
    std::string shortID = KeyUtils::toShortString(selfID);

    size_t clientQueueSize = getClientRequestQueueSize();

    CLOG_DEBUG(Overlay,
            "prop(): self={}, txn_count={}, latestCommittedView={}, currentView={}, clientRequestQueue={}",
            shortID,
            txn_count,
            latestCommittedView,
            currentView,
            clientQueueSize);

    // Non-leader/custom-disabled nodes do not drive Shabdiz / IT-HS proposals.
    if (!mApp.getConfig().SEND_CUSTOM_MESSAGE)
    {
        txn_count++;
        return;
    }

    updateForcedCollectState();

    bool canUseFastPath =
        (latestCommittedView == currentView - 1) && !force_collect;

    if (canUseFastPath)
    {
        // Avoid duplicate proposals for the same view.
        if (g_fastProposedViews.count(currentView))
        {
            CLOG_DEBUG(Overlay,
                    "[FAST PROP SKIP] already proposed in view {}",
                    currentView);
            return;
        }

        TransactionBatch batch;
bool fromExternalClient = false;
std::vector<ClientAck> clientAcks;

        if (g_clientListenerActive)
        {
            if (!tryBuildServerBatch(batch, clientAcks))
            {
                CLOG_DEBUG(Overlay,
                        "[CLIENT] Only {} queued requests; need {} to propose view {}",
                        getClientRequestQueueSize(),
                        SERVER_BATCH_SIZE,
                        currentView);
                return;
            }

            fromExternalClient = true;

            releaseAssert(batch.transactions.size() == SERVER_BATCH_SIZE);
            releaseAssert(clientAcks.size() == SERVER_BATCH_SIZE);

            CLOG_DEBUG(Overlay,
                    "[CLIENT SERVER-BATCH] built block txns={} requests={} view={}",
                    batch.transactions.size(),
                    clientAcks.size(),
                    currentView);
        }
        else
        {
            batch = makeSyntheticYCSBBatch(
                mApp,
                shortID,
                txn_count,
                SERVER_BATCH_SIZE);

            CLOG_DEBUG(Overlay,
                    "[INTERNAL] Using synthetic block batch, txns={}, view={}",
                    batch.transactions.size(),
                    currentView);
        }

        if (batch.transactions.empty())
        {
            CLOG_WARNING(Overlay,
                        "[PROP SKIP] empty batch for view {}",
                        currentView);
            return;
        }

        // Only mark the view as proposed after we actually have a non-empty batch.
        g_fastProposedViews.insert(currentView);

        txn_count += batch.transactions.size();

        Hash blockHash = makeBlock(latestCommittedBlock, txn_count);
        BlockKey key{currentView, blockHash};

        // Remember which client batch should be ACKed when this exact block commits.
        if (fromExternalClient)
        {
            auto& acks = g_blockClientAcks[key];
            acks.insert(acks.end(), clientAcks.begin(), clientAcks.end());
        }

        std::string serializedBatch = batch.serialize();
        CLOG_DEBUG(Overlay,
          "[BATCH SIZE] path=fast txns={} serialized_batch_bytes={} bytes_per_txn={}",
          batch.transactions.size(),
          serializedBatch.size(),
          batch.transactions.empty() ? 0 : serializedBatch.size() / batch.transactions.size());

        auto msg = makeProposalMessage(
            ITHS_MODE,
            currentView,
            blockHash,
            latestCommittedView,
            latestCommittedBlock,
            batch.serialize());




        CLOG_INFO(Overlay,
                "[FAST SEND PROPOSE] block={} view={} parentView={} parentBlock={} latestCommittedView={} latestCommittedBlock={} txns={} source={}",
                hexAbbrev(blockHash),
                currentView,
                msg->customMessage().vp,
                hexAbbrev(msg->customMessage().bp),
                latestCommittedView,
                hexAbbrev(latestCommittedBlock),
                batch.transactions.size(),
                fromExternalClient ? "external-client" : "synthetic");

        if (shabdizStartTimeSet==false)
        {
            shabdizStartTime = std::chrono::steady_clock::now();
            shabdizStartTimeSet = true;
        }

        broadcastMessage(msg);

        // =====================================================
        // Local leader handling: the leader also participates.
        // =====================================================
        auto& st = g_txn[key];
        auto const& cm = msg->customMessage();

        if (ITHS_MODE)
        {
            bool firstEchoThisView = !g_ithsEchoSentForView.count(cm.view);
            // No view-change implementation: treat parent/lock metadata as the safety check.
            bool safeForLocalLock = (cm.vp >= g_ithsLockView);

            if (firstEchoThisView && safeForLocalLock)
            {
                g_ithsEchoSentForView[cm.view] = cm.blockHash;

                st.ithsEchoSent = true;
                st.ithsEchoVoters.insert(selfID);

                sendITHSEcho(cm.view, cm.blockHash, cm.data);

                CLOG_DEBUG(Overlay,
                        "[IT-HS SELF-LOCAL SEND ECHO] block={} view={} proposalLockView={} localLockView={}",
                        hexAbbrev(blockHash),
                        currentView,
                        cm.vp,
                        g_ithsLockView);
            }
            else
            {
                CLOG_DEBUG(Overlay,
                          "[IT-HS SELF-LOCAL NO ECHO] block={} view={} ithsEchoSent={} latestCommittedView={}",
                          hexAbbrev(blockHash),
                          currentView,
                          st.ithsEchoSent,
                          latestCommittedView);
            }
        }
        else
        {
            bool selfExtendsCommitted =
                (latestCommittedView == currentView - 1) &&
                (cm.vp == latestCommittedView) &&
                (cm.bp == latestCommittedBlock);

            if (!st.preparedSent && selfExtendsCommitted)
            {
                st.preparedSent  = true;
                st.preparedView  = currentView;
                st.preparedBlock = blockHash;

                g_ps.insert(key);

                st.prepareVoters.insert(selfID);

                sendPrepare(cm.view, cm.blockHash, cm.data);

                CLOG_DEBUG(Overlay,
                          "[SELF-LOCAL SEND PREPARE] block={} view={} parentView={} parentBlock={} prepareVotes={}",
                          hexAbbrev(blockHash),
                          currentView,
                          cm.vp,
                          hexAbbrev(cm.bp),
                          st.prepareVoters.size());


            }
            else
            {
                CLOG_DEBUG(Overlay,
                          "[SELF-LOCAL NO PREPARE] block={} view={} preparedSent={} latestCommittedView={} expectedPrev={} parentView={} parentBlock={} localCommittedBlock={}",
                          hexAbbrev(blockHash),
                          currentView,
                          st.preparedSent,
                          latestCommittedView,
                          currentView - 1,
                          cm.vp,
                          hexAbbrev(cm.bp),
                          hexAbbrev(latestCommittedBlock));
            }
        }

        return;
    }

    // =====================================================
    // Slow path / forced COLLECT path
    // =====================================================
    if (lastCollectSentView == currentView)
    {
        CLOG_DEBUG(Overlay,
                "[SLOW COLLECT SKIP] already sent COLLECT for view {}",
                currentView);
        return;
    }

    lastCollectSentView = currentView;

    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType = CUSTOM_COLLECT;
    msg->customMessage().view    = currentView;

    CLOG_INFO(Overlay,
              "Leader initiating COLLECT for view {}",
              currentView);

    broadcastMessage(msg);

    uint64_t vp = 0;
    Hash bp = Hash();

    if (g_localPreparedView > 0)
    {
        vp = g_localPreparedView;
        bp = g_localPreparedBlock;
    }
    else if (latestCommittedView > 0)
    {
        vp = latestCommittedView;
        bp = latestCommittedBlock;
    }
    else
    {
        vp = 0;
        bp = Hash();
    }

    auto sendMsg = std::make_shared<StellarMessage>();
    sendMsg->type(CUSTOM_MESSAGE);
    sendMsg->customMessage().msgType = CUSTOM_SEND;
    sendMsg->customMessage().view    = currentView;
    sendMsg->customMessage().vp      = vp;
    sendMsg->customMessage().bp      = bp;
    sendMsg->customMessage().origin  = selfID;

    broadcastMessage(sendMsg);

    CLOG_DEBUG(Overlay,
            "[SLOW SELF SEND] origin={} view={} vp={} bp={}",
            shortID,
            currentView,
            vp,
            hexAbbrev(bp));



    if (collectWindowActive)
    {
        collectAttempts++;

        CLOG_DEBUG(Overlay,
                   "Forced COLLECT attempt {}/{}",
                   collectAttempts,
                   MAX_COLLECT_ATTEMPTS);

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
OverlayManagerImpl::sendITHSEcho(uint64_t view, Hash const& blockHash,
                                  std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType   = CUSTOM_ITHS_ECHO;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;
    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "[IT-HS] Broadcast ECHO block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendITHSAccept(uint64_t view, Hash const& blockHash,
                                    std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType   = CUSTOM_ITHS_ACCEPT;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;
    broadcastMessage(msg);
    CLOG_DEBUG(Overlay, "[IT-HS] Broadcast ACCEPT block {} view {}",
               hexAbbrev(blockHash), view);
}

void
OverlayManagerImpl::sendITHSLock(uint64_t view, Hash const& blockHash,
                                  std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType   = CUSTOM_ITHS_LOCK;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;
    broadcastMessage(msg);
}

void
OverlayManagerImpl::sendITHSCommit(uint64_t view, Hash const& blockHash,
                                   std::string const& data)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(CUSTOM_MESSAGE);
    msg->customMessage().msgType   = CUSTOM_ITHS_COMMIT;
    msg->customMessage().view      = view;
    msg->customMessage().blockHash = blockHash;
    msg->customMessage().data      = data;

    broadcastMessage(msg);

    CLOG_DEBUG(Overlay, "[IT-HS] Broadcast COMMIT block {} view {}",
               hexAbbrev(blockHash), view);
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


    auto deliverBufferedForCurrentView = [this]() {
        while (true)
        {
            auto it = g_futureFastMsgs.find(currentView);
            if (it == g_futureFastMsgs.end())
            {
                return;
            }

            auto buffered = std::move(it->second);
            g_futureFastMsgs.erase(it);

            CLOG_DEBUG(Overlay,
                    "[FAST DELIVER BUFFERED] view={} count={}",
                    currentView,
                    buffered.size());

            for (auto& [msg, p] : buffered)
            {
                this->recvCustomMessage(msg, p);
            }
        }
    };


    bool ithsOldBackgroundMsg =
    ITHS_MODE &&
    (cm.msgType == CUSTOM_ITHS_ACCEPT ||
     cm.msgType == CUSTOM_ITHS_COMMIT);

    if (cm.view < currentView && !ithsOldBackgroundMsg)
    {
        CLOG_DEBUG(Overlay,
                "[FAST DROP OLD] type={} view={} currentView={} block={}",
                static_cast<int>(cm.msgType),
                cm.view,
                currentView,
                hexAbbrev(cm.blockHash));
        return;
    }

    if (cm.view > currentView)
    {
        CLOG_DEBUG(Overlay,
                "[FAST BUFFER FUTURE] type={} view={} currentView={} block={}",
                static_cast<int>(cm.msgType),
                cm.view,
                currentView,
                hexAbbrev(cm.blockHash));

        g_futureFastMsgs[cm.view].push_back({stellarMsg, peer});
        return;
    }


    BlockKey key{cm.view, cm.blockHash};
    auto& st = g_txn[key];

    if (N ==0)
    {
        N = getAuthenticatedPeersCount() + 1;
        f = (N - 1) / 3;
    }




    auto tryLeaderSendSlowProposal = [&](decltype(st)& state) {
        if (!mApp.getConfig().SEND_CUSTOM_MESSAGE)
        {
            return;
        }

        if (state.proposalSentForView)
        {
            return;
        }

        if (state.collection.size() < 2 * f + 1)
        {
            return;
        }

        auto [maxView, maxBlock] = maxPreparedFromCollection(state.collection);

        TransactionBatch batch;
        bool fromExternalClient = false;
        std::vector<ClientAck> clientAcks;

        if (g_clientListenerActive)
        {
            if (!tryBuildServerBatch(batch, clientAcks))
            {
                CLOG_DEBUG(Overlay,
                        "[SLOW CLIENT] Only {} queued requests; need {} to propose slow-path view {}",
                        getClientRequestQueueSize(),
                        SERVER_BATCH_SIZE,
                        currentView);
                return;
            }

            fromExternalClient = true;

            releaseAssert(batch.transactions.size() == SERVER_BATCH_SIZE);
            releaseAssert(clientAcks.size() == SERVER_BATCH_SIZE);
        }
        else
        {
            batch = makeSyntheticYCSBBatch(
                mApp,
                KeyUtils::toShortString(selfID),
                txn_count,
                SERVER_BATCH_SIZE);
        }

        if (batch.transactions.empty())
        {
            CLOG_WARNING(Overlay,
                        "[SLOW PROP SKIP] empty batch for view {}",
                        currentView);
            return;
        }

        txn_count += batch.transactions.size();

        Hash newBlock = makeBlock(maxBlock, txn_count);
        BlockKey newKey{currentView, newBlock};

        if (fromExternalClient)
        {
            auto& acks = g_blockClientAcks[newKey];
            acks.insert(acks.end(), clientAcks.begin(), clientAcks.end());
        }

        auto msg = std::make_shared<StellarMessage>();
        msg->type(CUSTOM_MESSAGE);
        msg->customMessage().msgType   = CUSTOM_PROPOSE;
        msg->customMessage().view      = currentView;
        msg->customMessage().blockHash = newBlock;

        // Important: slow-path proposal extends the highest prepared block
        // selected from the collection.
        msg->customMessage().vp = maxView;
        msg->customMessage().bp = maxBlock;

        msg->customMessage().data = batch.serialize();



        broadcastMessage(msg);

        state.proposalSentForView = true;

        CLOG_INFO(Overlay,
                "[SLOW SEND PROPOSE] block={} view={} parentView={} parentBlock={} txns={} source={}",
                hexAbbrev(newBlock),
                currentView,
                maxView,
                hexAbbrev(maxBlock),
                batch.transactions.size(),
                fromExternalClient ? "external-client" : "synthetic");
    };


    auto deliverSlowValue =
    [&](decltype(st)& state, OriginViewBlockKey const& target)
    {
        if (!state.delivered.insert(target).second)
        {
            return;
        }

        state.collection[target.origin] = {target.view, target.block};

        CLOG_DEBUG(Overlay,
                "[SLOW DELIVER] origin={} target=(vp={}, bp={}) collectionSize={}",
                KeyUtils::toShortString(target.origin),
                target.view,
                hexAbbrev(target.block),
                state.collection.size());

        tryLeaderSendSlowProposal(state);
    };

    auto activatePendingCondReadyForDependency =
    [&](decltype(st)& state, ViewBlockKey const& dependency)
    {
        auto it = state.pendingCondReady.find(dependency);
        if (it == state.pendingCondReady.end())
        {
            return;
        }

        auto pendingVotes = std::move(it->second);
        state.pendingCondReady.erase(it);

        CLOG_INFO(Overlay,
                "[CONDREADY ACTIVATE] dependency=(vp={}, bp={}) pendingVotes={}",
                dependency.view,
                hexAbbrev(dependency.block),
                pendingVotes.size());

        for (auto const& vote : pendingVotes)
        {
            state.readies[vote.target].insert(vote.sender);

            CLOG_INFO(Overlay,
                    "[CONDREADY COUNTED] sender={} origin={} target=(vp={}, bp={}) readies={}",
                    KeyUtils::toShortString(vote.sender),
                    KeyUtils::toShortString(vote.target.origin),
                    vote.target.view,
                    hexAbbrev(vote.target.block),
                    state.readies[vote.target].size());

            if (state.readies[vote.target].size() >= 2 * f + 1)
            {
                deliverSlowValue(state, vote.target);
            }
        }
    };


    switch (cm.msgType)
    {
        // ================================================================
        case CUSTOM_PROPOSE:
        {
            CLOG_DEBUG(Overlay,
                    "[RECV PROPOSE] self={} sender={} view={} block={} currentView={} latestCommittedView={} parentView={} parentBlock={}",
                    KeyUtils::toShortString(selfID),
                    KeyUtils::toShortString(sender),
                    cm.view,
                    hexAbbrev(cm.blockHash),
                    currentView,
                    latestCommittedView,
                    cm.vp,
                    hexAbbrev(cm.bp));

            bool fastExtendsCommitted =
                (latestCommittedView == cm.view - 1) &&
                (cm.vp == latestCommittedView) &&
                (cm.bp == latestCommittedBlock);

            auto& collectSt = g_txn[BlockKey{cm.view, Hash()}];

            bool slowProposalOk = false;
            if (collectSt.collection.size() >= 2 * f + 1)
            {
                auto [maxView, maxBlock] =
                    maxPreparedFromCollection(collectSt.collection);

                slowProposalOk =
                    (cm.vp == maxView) &&
                    (cm.bp == maxBlock);

                CLOG_INFO(Overlay,
                        "[SLOW PROPOSE CHECK] collectionSize={} selectedParentView={} selectedParentBlock={} proposedParentView={} proposedParentBlock={} ok={}",
                        collectSt.collection.size(),
                        maxView,
                        hexAbbrev(maxBlock),
                        cm.vp,
                        hexAbbrev(cm.bp),
                        slowProposalOk);
            }
            else
            {
                CLOG_INFO(Overlay,
                        "[SLOW PROPOSE CHECK] insufficient collection size={} threshold={}",
                        collectSt.collection.size(),
                        2 * f + 1);
            }

            bool extendsValidParent =
                fastExtendsCommitted || slowProposalOk;

            if (!st.preparedSent && extendsValidParent)
            {
                st.preparedSent = true;
                st.preparedView = cm.view;
                st.preparedBlock = cm.blockHash;

                g_ps.insert(BlockKey{cm.view, cm.blockHash});

                st.prepareVoters.insert(selfID);
                sendPrepare(cm.view, cm.blockHash, cm.data);

                CLOG_DEBUG(Overlay,
                        "[SEND PREPARE] view={} block={} prepareVotes={} reason={}",
                        cm.view,
                        hexAbbrev(cm.blockHash),
                        st.prepareVoters.size(),
                        fastExtendsCommitted ? "fast-parent" : "slow-collection");
            }
            else
            {
                CLOG_DEBUG(Overlay,
                        "[NO PREPARE] view={} block={} preparedSent={} fastExtendsCommitted={} slowProposalOk={} collectionSize={} latestCommittedView={} proposedParentView={} proposedParentBlock={} localCommittedBlock={}",
                        cm.view,
                        hexAbbrev(cm.blockHash),
                        st.preparedSent,
                        fastExtendsCommitted,
                        slowProposalOk,
                        collectSt.collection.size(),
                        latestCommittedView,
                        cm.vp,
                        hexAbbrev(cm.bp),
                        hexAbbrev(latestCommittedBlock));
            }

            break;
        }


        // ================================================================
        case CUSTOM_PREPARE:
        {
            bool inserted = st.prepareVoters.insert(sender).second;

            CLOG_DEBUG(Overlay,
                    "[FAST RECV PREPARE] self={} sender={} inserted={} view={} block={} prepareVotes={} threshold={}",
                    KeyUtils::toShortString(selfID),
                    KeyUtils::toShortString(sender),
                    inserted,
                    cm.view,
                    hexAbbrev(cm.blockHash),
                    st.prepareVoters.size(),
                    2 * f + 1);

            if (st.prepareVoters.size() >= 2 * f + 1 && g_csentView < currentView)
            {
                g_csentView = currentView;

                st.commitVoters.insert(selfID);
                sendCommit(cm.view, cm.blockHash, cm.data);

                st.preparedView  = cm.view;
                st.preparedBlock = cm.blockHash;

                rememberLocalPrepared(cm.view, cm.blockHash);

                ViewBlockKey dependency{cm.view, cm.blockHash};
                activatePendingCondReadyForDependency(st, dependency);

                if (!PBFT_MODE)
                {
                    for (auto it = g_ps.begin(); it != g_ps.end(); )
                    {
                        if (it->view != st.preparedView)
                            it = g_ps.erase(it);
                        else
                            ++it;
                    }
                }

                CLOG_DEBUG(Overlay,
                        "[FAST SEND COMMIT] view={} block={} prepareVotes={} commitVotes={}",
                        cm.view,
                        hexAbbrev(cm.blockHash),
                        st.prepareVoters.size(),
                        st.commitVoters.size());
            }

            break;
        }



        // ================================================================
        case CUSTOM_COMMIT:
        {
            bool inserted = st.commitVoters.insert(sender).second;

            CLOG_DEBUG(Overlay,
                    "[FAST RECV COMMIT] self={} sender={} inserted={} view={} block={} commitVotes={} fPlusOne={} quorum={}",
                    KeyUtils::toShortString(selfID),
                    KeyUtils::toShortString(sender),
                    inserted,
                    cm.view,
                    hexAbbrev(cm.blockHash),
                    st.commitVoters.size(),
                    f + 1,
                    2 * f + 1);

            // Amplification: f+1 COMMITs and I have not sent COMMIT in this view.
            if (st.commitVoters.size() >= f + 1 && g_csentView < currentView)
            {
                g_csentView = currentView;

                st.commitVoters.insert(selfID);
                sendCommit(cm.view, cm.blockHash, cm.data);

                st.preparedView  = cm.view;
                st.preparedBlock = cm.blockHash;
                rememberLocalPrepared(cm.view, cm.blockHash);

                ViewBlockKey dependency{cm.view, cm.blockHash};
                activatePendingCondReadyForDependency(st, dependency);

                if (!PBFT_MODE)
                {
                    for (auto it = g_ps.begin(); it != g_ps.end(); )
                    {
                        if (it->view != st.preparedView)
                            it = g_ps.erase(it);
                        else
                            ++it;
                    }
                }

                CLOG_DEBUG(Overlay,
                        "[FAST AMPLIFY COMMIT] view={} block={} commitVotes={}",
                        cm.view,
                        hexAbbrev(cm.blockHash),
                        st.commitVoters.size());
            }

            // Final commit: 2f+1 COMMITs and not already committed this view.
            if (st.commitVoters.size() >= 2 * f + 1 && latestCommittedView < currentView)
            {
                st.committedView = cm.view;
                st.committedBlock = cm.blockHash;

                latestCommittedView = cm.view;
                latestCommittedBlock = cm.blockHash;

                st.preparedView  = cm.view;
                st.preparedBlock = cm.blockHash;
                rememberLocalPrepared(cm.view, cm.blockHash);

                ViewBlockKey dependency{cm.view, cm.blockHash};
                activatePendingCondReadyForDependency(st, dependency);

                if (!PBFT_MODE)
                {
                    for (auto it = g_ps.begin(); it != g_ps.end(); )
                    {
                        if (it->view != st.preparedView)
                            it = g_ps.erase(it);
                        else
                            ++it;
                    }
                }

                TransactionBatch batch = TransactionBatch::deserialize(cm.data);
                for (auto const& txn : batch.transactions)
                {
                    std::istringstream ss(txn.payload);
                    std::string op, key, value;
                    ss >> op >> key;
                    if (op == "READ")
                    {
                        auto it = g_kvStore.find(key);
                        (void)it;
                    }
                    else if (op == "UPDATE" || op == "RMW")
                    {
                        ss >> value;
                        g_kvStore[key] = value;
                    }
                }

                CLOG_INFO(Overlay,
                        "[FAST COMMITTED] block={} view={} txns={} nextView={}",
                        hexAbbrev(cm.blockHash),
                        cm.view,
                        batch.transactions.size(),
                        cm.view + 1);

                ackClientBatchesForBlock(cm.view, cm.blockHash);

                currentView = cm.view + 1;
                lastCollectSentView = UINT64_MAX;

                BlockKey nextKey{currentView, Hash()};
                g_txn[nextKey].proposalSentForView = false;

                cleanupOldTxnStates();

                // Critical: deliver buffered messages for the new view before proposing again.
                deliverBufferedForCurrentView();

                // Only the leader node actually proposes because prop() checks SEND_CUSTOM_MESSAGE.
                if (latestCommittedView == currentView - 1)
                {
                    prop();
                }
            }

            break;
        }


        // ================================================================
        case CUSTOM_COLLECT:
            CLOG_INFO(Overlay, "Received COLLECT for view {} from {}",
                    cm.view, KeyUtils::toShortString(sender));

            {
                uint64_t vp = 0;
                Hash bp = Hash();

                if (g_localPreparedView > 0)
                {
                    vp = g_localPreparedView;
                    bp = g_localPreparedBlock;

                    CLOG_INFO(Overlay,
                            "[COLLECT REPLY] using local prepared vp={} bp={}",
                            vp,
                            hexAbbrev(bp));
                }
                else if (latestCommittedView > 0)
                {
                    vp = latestCommittedView;
                    bp = latestCommittedBlock;

                    CLOG_INFO(Overlay,
                            "[COLLECT REPLY] using latest committed vp={} bp={}",
                            vp,
                            hexAbbrev(bp));
                }
                else
                {
                    vp = 0;
                    bp = Hash();

                    CLOG_INFO(Overlay,
                            "[COLLECT REPLY] using genesis vp=0 bp={}",
                            hexAbbrev(bp));
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
        {
            CLOG_INFO(Overlay,
                    "Received SEND target=(vp={}, bp={}) view={} from={} origin={}",
                    cm.vp,
                    hexAbbrev(cm.bp),
                    cm.view,
                    KeyUtils::toShortString(sender),
                    KeyUtils::toShortString(cm.origin));

            OriginViewBlockKey ovb{cm.origin, cm.vp, cm.bp};

            if (!st.eSent.count(ovb))
            {
                st.eSent.insert(ovb);

                auto msg = std::make_shared<StellarMessage>();
                msg->type(CUSTOM_MESSAGE);
                msg->customMessage().msgType = CUSTOM_ECHO;
                msg->customMessage().view    = cm.view;
                msg->customMessage().vp      = cm.vp;
                msg->customMessage().bp      = cm.bp;
                msg->customMessage().origin  = cm.origin;

                broadcastMessage(msg);

                st.echoes[ovb].insert(selfID);

                CLOG_INFO(Overlay,
                        "Sending ECHO origin={} target=(vp={}, bp={})",
                        KeyUtils::toShortString(cm.origin),
                        cm.vp,
                        hexAbbrev(cm.bp));
            }

            break;
        }

        // ================================================================
        case CUSTOM_ECHO:
        {
            CLOG_INFO(Overlay,
                    "Received ECHO origin={} target=(vp={}, bp={}) view={} from={}",
                    KeyUtils::toShortString(cm.origin),
                    cm.vp,
                    hexAbbrev(cm.bp),
                    cm.view,
                    KeyUtils::toShortString(sender));

            OriginViewBlockKey ovb{cm.origin, cm.vp, cm.bp};
            st.echoes[ovb].insert(sender);

            if (st.echoes[ovb].size() >= 2 * f + 1 &&
                !st.rSent.count(ovb))
            {
                if (g_ps.count(BlockKey{cm.vp, cm.bp}))
                {
                    st.rSent.insert(ovb);

                    auto msg = std::make_shared<StellarMessage>();
                    msg->type(CUSTOM_MESSAGE);
                    msg->customMessage().msgType = CUSTOM_READY;
                    msg->customMessage().view    = cm.view;
                    msg->customMessage().vp      = cm.vp;
                    msg->customMessage().bp      = cm.bp;
                    msg->customMessage().origin  = cm.origin;

                    broadcastMessage(msg);

                    st.readies[ovb].insert(selfID);

                    CLOG_DEBUG(Overlay,
                            "Sending READY origin={} target=(vp={}, bp={})",
                            KeyUtils::toShortString(cm.origin),
                            cm.vp,
                            hexAbbrev(cm.bp));


                    if (st.readies[ovb].size() >= 2 * f + 1)
                    {
                        deliverSlowValue(st, ovb);
                    }
                }
                else if (cm.vp < g_localPreparedView)
                {
                    st.rSent.insert(ovb);

                    auto msg = std::make_shared<StellarMessage>();
                    msg->type(CUSTOM_MESSAGE);
                    msg->customMessage().msgType = CUSTOM_CONDREADY;
                    msg->customMessage().view    = cm.view;

                    // Target.
                    msg->customMessage().vp = cm.vp;
                    msg->customMessage().bp = cm.bp;

                    // Original SEND origin.
                    msg->customMessage().origin = cm.origin;

                    // Higher prepared dependency.
                    msg->customMessage().dependencyVp = g_localPreparedView;
                    msg->customMessage().dependencyBp = g_localPreparedBlock;

                    broadcastMessage(msg);

                    CLOG_DEBUG(Overlay,
                            "Sending CONDREADY origin={} target=(vp={}, bp={}) dependency=(vp={}, bp={})",
                            KeyUtils::toShortString(cm.origin),
                            cm.vp,
                            hexAbbrev(cm.bp),
                            g_localPreparedView,
                            hexAbbrev(g_localPreparedBlock));
                }
            }

            break;
        }

        // ================================================================
        case CUSTOM_READY:
        {
            CLOG_INFO(Overlay,
                    "Received READY origin={} target=(vp={}, bp={}) view={} from={}",
                    KeyUtils::toShortString(cm.origin),
                    cm.vp,
                    hexAbbrev(cm.bp),
                    cm.view,
                    KeyUtils::toShortString(sender));

            OriginViewBlockKey ovb{cm.origin, cm.vp, cm.bp};

            st.readies[ovb].insert(sender);

            // READY amplification: f+1 READYs imply send READY if not already sent.
            if (st.readies[ovb].size() >= f + 1 &&
                !st.rSent.count(ovb))
            {
                st.rSent.insert(ovb);

                auto msg = std::make_shared<StellarMessage>();
                msg->type(CUSTOM_MESSAGE);
                msg->customMessage().msgType = CUSTOM_READY;
                msg->customMessage().view    = cm.view;
                msg->customMessage().vp      = cm.vp;
                msg->customMessage().bp      = cm.bp;
                msg->customMessage().origin  = cm.origin;

                broadcastMessage(msg);

                st.readies[ovb].insert(selfID);

                CLOG_INFO(Overlay,
                        "[READY AMPLIFY] origin={} target=(vp={}, bp={}) readies={}",
                        KeyUtils::toShortString(cm.origin),
                        cm.vp,
                        hexAbbrev(cm.bp),
                        st.readies[ovb].size());
            }

            // Delivery: 2f+1 READYs deliver this origin's reported prepared value.
            if (st.readies[ovb].size() >= 2 * f + 1)
            {
                deliverSlowValue(st, ovb);
            }

            break;
        }

        // ================================================================
        case CUSTOM_CONDREADY:
        {
            CLOG_INFO(Overlay,
                    "Received CONDREADY origin={} target=(vp={}, bp={}) dependency=(vp={}, bp={}) view={} from={}",
                    KeyUtils::toShortString(cm.origin),
                    cm.vp,
                    hexAbbrev(cm.bp),
                    cm.dependencyVp,
                    hexAbbrev(cm.dependencyBp),
                    cm.view,
                    KeyUtils::toShortString(sender));

            OriginViewBlockKey target{cm.origin, cm.vp, cm.bp};
            ViewBlockKey dependency{cm.dependencyVp, cm.dependencyBp};

            bool dependencyKnown =
                g_ps.count(BlockKey{cm.dependencyVp, cm.dependencyBp}) ||
                (g_localPreparedView >= cm.dependencyVp &&
                g_localPreparedBlock == cm.dependencyBp) ||
                (latestCommittedView >= cm.dependencyVp &&
                latestCommittedBlock == cm.dependencyBp);

            if (dependencyKnown)
            {
                st.readies[target].insert(sender);

                CLOG_INFO(Overlay,
                        "CONDREADY counted origin={} target=(vp={}, bp={}) dependency=(vp={}, bp={}) readies={}",
                        KeyUtils::toShortString(cm.origin),
                        cm.vp,
                        hexAbbrev(cm.bp),
                        cm.dependencyVp,
                        hexAbbrev(cm.dependencyBp),
                        st.readies[target].size());

                if (st.readies[target].size() >= 2 * f + 1)
                {
                    deliverSlowValue(st, target);
                }
            }
            else
            {
                PendingCondReadyVote pending;
                pending.sender = sender;
                pending.target = target;

                st.pendingCondReady[dependency].push_back(std::move(pending));

                CLOG_INFO(Overlay,
                        "CONDREADY deferred origin={} target=(vp={}, bp={}) waiting for dependency=(vp={}, bp={})",
                        KeyUtils::toShortString(cm.origin),
                        cm.vp,
                        hexAbbrev(cm.bp),
                        cm.dependencyVp,
                        hexAbbrev(cm.dependencyBp));
            }

            break;
        }

        // ================================================================
        case CUSTOM_ITHS_PROPOSE:
            if (cm.view == currentView)
            {
                CLOG_DEBUG(Overlay, "[IT-HS] Received PROPOSE block {} view {}",
                        hexAbbrev(cm.blockHash), cm.view);

                bool firstEchoThisView = !g_ithsEchoSentForView.count(cm.view);

                // No view-change implementation: proposal's lock/parent view must
                // be at least the local IT-HS lock view.
                bool safeForLocalLock = (cm.vp >= g_ithsLockView);

                if (firstEchoThisView && safeForLocalLock)
                {
                    g_ithsEchoSentForView[cm.view] = cm.blockHash;

                    st.ithsEchoSent = true;
                    st.ithsEchoVoters.insert(selfID);
                    sendITHSEcho(cm.view, cm.blockHash, cm.data);
                }
                else
                {
                    CLOG_DEBUG(Overlay,
                            "[IT-HS] Reject/skip PROPOSE block {} view {} firstEchoThisView={} proposalLockView={} localLockView={}",
                            hexAbbrev(cm.blockHash),
                            cm.view,
                            firstEchoThisView,
                            cm.vp,
                            g_ithsLockView);
                }
            }
            break;

        // ================================================================
        case CUSTOM_ITHS_ECHO:
            if (cm.view == currentView)
            {
                st.ithsEchoVoters.insert(sender);

                CLOG_DEBUG(Overlay, "[IT-HS] Received ECHO block {} view {} echoes={}",
                        hexAbbrev(cm.blockHash), cm.view, st.ithsEchoVoters.size());

                // Blog threshold is n-f, not always 2f+1.
                if (st.ithsEchoVoters.size() >= N - f &&
                    !g_ithsAcceptSentForView.count(cm.view))
                {
                    g_ithsAcceptSentForView[cm.view] = cm.blockHash;

                    st.ithsAcceptSent = true;
                    st.ithsAcceptVoters.insert(selfID);

                    sendITHSAccept(cm.view, cm.blockHash, cm.data);

                    CLOG_DEBUG(Overlay, "[IT-HS] Sent ACCEPT block {} view {}",
                            hexAbbrev(cm.blockHash), cm.view);
                }
            }
            break;

        // ================================================================
        case CUSTOM_ITHS_ACCEPT:
            {
                st.ithsAcceptVoters.insert(sender);

                CLOG_DEBUG(Overlay, "[IT-HS] Received ACCEPT block {} view {} accepts={}",
                        hexAbbrev(cm.blockHash), cm.view, st.ithsAcceptVoters.size());

                // Background boosting: f+1 ACCEPT -> send ACCEPT.
                // This should run even for old views.
                if (st.ithsAcceptVoters.size() >= f + 1 &&
                    !g_ithsAcceptSentForView.count(cm.view))
                {
                    g_ithsAcceptSentForView[cm.view] = cm.blockHash;

                    st.ithsAcceptSent = true;
                    st.ithsAcceptVoters.insert(selfID);

                    sendITHSAccept(cm.view, cm.blockHash, cm.data);

                    CLOG_DEBUG(Overlay,
                            "[IT-HS] Boosted ACCEPT block {} view {}",
                            hexAbbrev(cm.blockHash), cm.view);
                }

                // Main lock rule: n-f ACCEPT -> set lock and send LOCK.
                // Only do lock update for the current view.
                if (cm.view == currentView &&
                    st.ithsAcceptVoters.size() >= N - f &&
                    !g_ithsLockSentForView.count(cm.view))
                {
                    g_ithsLockView  = cm.view;
                    g_ithsLockBlock = cm.blockHash;

                    g_ithsLockSentForView[cm.view] = cm.blockHash;

                    st.ithsLockSent = true;
                    st.ithsLockVoters.insert(selfID);

                    sendITHSLock(cm.view, cm.blockHash, cm.data);

                    CLOG_DEBUG(Overlay, "[IT-HS] Set lock=({},{}) and sent LOCK",
                            cm.view, hexAbbrev(cm.blockHash));
                }

                break;
            }

        // ================================================================

        case CUSTOM_ITHS_LOCK:
        {
            if (cm.view == currentView)
            {
                st.ithsLockVoters.insert(sender);

                CLOG_DEBUG(Overlay, "[IT-HS] Received LOCK block {} view {} locks={}",
                        hexAbbrev(cm.blockHash), cm.view, st.ithsLockVoters.size());

                // Blog rule: n-f LOCK -> send COMMIT.
                // Do NOT commit locally here.
                if (st.ithsLockVoters.size() >= N - f &&
                    !g_ithsCommitSentForView.count(cm.view))
                {
                    g_ithsCommitSentForView[cm.view] = cm.blockHash;

                    st.ithsCommitSent = true;
                    st.ithsCommitVoters.insert(selfID);

                    sendITHSCommit(cm.view, cm.blockHash, cm.data);

                    CLOG_DEBUG(Overlay,
                            "[IT-HS] Sent COMMIT after LOCK quorum block {} view {}",
                            hexAbbrev(cm.blockHash), cm.view);
                }
            }

            break;
        }
        // ================================================================
        case CUSTOM_ITHS_COMMIT:
        {
            st.ithsCommitVoters.insert(sender);

            CLOG_DEBUG(Overlay, "[IT-HS] Received COMMIT block {} view {} commits={}",
                    hexAbbrev(cm.blockHash), cm.view, st.ithsCommitVoters.size());

            // Background boosting: f+1 COMMIT -> send COMMIT.
            // This should run even for old views.
            if (st.ithsCommitVoters.size() >= f + 1 &&
                !g_ithsCommitSentForView.count(cm.view))
            {
                g_ithsCommitSentForView[cm.view] = cm.blockHash;

                st.ithsCommitSent = true;
                st.ithsCommitVoters.insert(selfID);

                sendITHSCommit(cm.view, cm.blockHash, cm.data);

                CLOG_DEBUG(Overlay,
                        "[IT-HS] Boosted COMMIT block {} view {}",
                        hexAbbrev(cm.blockHash), cm.view);
            }

            // Blog termination/output rule: n-f COMMIT -> output.
            // In your chain code, output means apply batch and advance currentView.
            if (cm.view == currentView &&
                st.ithsCommitVoters.size() >= N - f &&
                st.committedView < cm.view)
            {
                st.committedView     = cm.view;
                st.committedBlock    = cm.blockHash;
                latestCommittedView  = cm.view;
                latestCommittedBlock = cm.blockHash;

                TransactionBatch batch = TransactionBatch::deserialize(cm.data);
                for (auto const& txn : batch.transactions)
                {
                    std::istringstream ss(txn.payload);
                    std::string op, key, value;
                    ss >> op >> key;

                    if (op == "READ")
                    {
                        auto it = g_kvStore.find(key);
                        (void)it;
                    }
                    else if (op == "UPDATE" || op == "RMW")
                    {
                        ss >> value;
                        g_kvStore[key] = value;
                    }
                }

                CLOG_INFO(Overlay,
                        "[IT-HS COMMITTED] block={} view={} txns={} nextView={}",
                        hexAbbrev(cm.blockHash),
                        cm.view,
                        batch.transactions.size(),
                        cm.view + 1);

                ackClientBatchesForBlock(cm.view, cm.blockHash);

                currentView = cm.view + 1;
                lastCollectSentView = UINT64_MAX;

                BlockKey nextKey{currentView, Hash()};
                g_txn[nextKey].proposalSentForView = false;

                cleanupOldTxnStates();
                deliverBufferedForCurrentView();

                if (latestCommittedView == currentView - 1)
                {
                    prop();
                }
            }

            break;
        }




        
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