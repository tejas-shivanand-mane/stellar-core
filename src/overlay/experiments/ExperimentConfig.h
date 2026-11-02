#pragma once

#include <cstddef>
#include <cstdint>

namespace stellar::experiment
{
// Build-time experiment settings shared by replicas and shab_client.
// Rebuild the affected binaries after edits. Runtime leader and normal-path
// selection remain in each node's stellar-core.cfg (see README.md).

// Legacy protocol selection: all false selects Shabdiz.
inline constexpr bool ITHS_MODE = false;
inline constexpr bool PBFT_MODE = false;
inline constexpr bool ENABLE_SCP_TRACKING = false;
inline constexpr bool NonPrune_MODE = false;
static_assert(int(ITHS_MODE) + int(PBFT_MODE) + int(ENABLE_SCP_TRACKING) <= 1,
              "Select only one legacy protocol");

// Shared TCP workload and server batching.
inline constexpr int CLIENT_PORT = 12000;
inline constexpr std::size_t SERVER_BATCH_SIZE = 100;

// Retention and the existing Shabdiz forced-COLLECT experiment.
inline constexpr uint64_t MAX_VIEW_HISTORY = 1000;
inline constexpr uint64_t PBFT_HISTORY = 1000;
inline constexpr uint64_t NORMAL_PATH_WINDOW = 64;
inline constexpr uint64_t MAX_COLLECT_ATTEMPTS = 300;
inline constexpr int FORCE_COLLECT_AFTER_SEC = 3000;

// Native SCP workload generator (not SCP consensus parameters).
inline constexpr int NUM_TEST_ACCOUNTS = 100;
inline constexpr int SCP_NATIVE_ROUNDS = 1;
inline constexpr int SCP_OPS_PER_TX = 100;

enum YCSBWorkload
{
    WORKLOAD_A,
    WORKLOAD_B,
    WORKLOAD_F,
    WORKLOAD_W,

    // New explicit read-ratio workloads
    WORKLOAD_R0,
    WORKLOAD_R10,
    WORKLOAD_R20,
    WORKLOAD_R25,
    WORKLOAD_R40,
    WORKLOAD_R50,
    WORKLOAD_R60,
    WORKLOAD_R75,
    WORKLOAD_R80,
    WORKLOAD_R100
};

inline constexpr YCSBWorkload currentWorkload = WORKLOAD_W;
inline constexpr double zipfian_alpha = 0.99;
inline constexpr uint64_t zipfian_n = 1000000;

static_assert(SERVER_BATCH_SIZE > 0 && NORMAL_PATH_WINDOW > 0);
} // namespace stellar::experiment
