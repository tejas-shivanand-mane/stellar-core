#pragma once

#include <cstddef>
#include <cstdint>

namespace stellar::experiment
{
// Build-time experiment settings shared by replicas and shab_client.
// Rebuild all replicas after edits. SEND_CUSTOM_MESSAGE=true selects client
// ingress. Shabdiz, native multi-shot TetraBFT and Simple-IT rotate leaders.
// Forget-IT rotates within an instance through Algorithm 1's view changes;
// each fresh benchmark instance still begins with the client-ingress leader.
// IT-HS Part One also rotates across its clock-scheduled views, with full
// SUGGEST/lock validation and background boosting. PBFT keeps its primary.
//
// Scope: fault-free benchmarks. TetraBFT implements section 6.1's chained
// multi-shot path; Simple-IT implements Figure 2's non-speculative S/S_opt
// round progression and RBC/commit overlap. Their view change, timeouts,
// recovery/disabled rounds are NOT implemented. Simple-IT uses the existing
// full-value proposal/digest-vote RBC, without erasure coding or speculation.
// Forget-IT implements the native view state machine, using reliable TCP.
// Runtime crash/restart persistence is not wired; bounded benchmark instance
// retention and client ingress are outside the one-shot paper algorithm.
// IT-HS requires synchronized clocks and reliable channels as in Part One;
// it retains evidence across views, without the OPODIS storage optimization.
// PBFT still lacks its view-change/recovery components. Do not
// report these adapters as complete fault-tolerant implementations of the
// papers.

// Protocol selection: all false selects Shabdiz.
inline constexpr bool ITHS_MODE = false;
inline constexpr bool PBFT_MODE = false;
inline constexpr bool ENABLE_SCP_TRACKING = false;
inline constexpr bool TETRABFT_MODE = false;
inline constexpr bool SIMPLE_IT_MODE = false;
inline constexpr bool FORGET_IT_MODE = false;

// Simple-IT variant: optimistic RBC (true) or Bracha RBC (false).
inline constexpr bool SIMPLE_IT_OPTIMISTIC = true;
inline constexpr bool NonPrune_MODE = false;
static_assert(int(ITHS_MODE) + int(PBFT_MODE) + int(ENABLE_SCP_TRACKING) +
                      int(TETRABFT_MODE) + int(SIMPLE_IT_MODE) +
                      int(FORGET_IT_MODE) <=
                  1,
              "Select only one consensus protocol");

// Shared TCP workload and server batching.
inline constexpr int CLIENT_PORT = 12000;
inline constexpr std::size_t SERVER_BATCH_SIZE = 100;

// IT-HS Part One: synchronized wall clocks, 2 Delta proposal wait, and views
// every 10 Delta from each instance's shared epoch. This is an actual wait
// even with no failures. Tune Delta to bound network/processing delays.
inline constexpr uint64_t ITHS_BLOG_DELTA_MS = 1000;
static_assert(ITHS_BLOG_DELTA_MS > 0 && ITHS_BLOG_DELTA_MS <= UINT64_MAX / 10);

// Forget-IT's known delay bound Delta: vote-2 timeout at 3 Delta, view change
// at 4 Delta, as in Algorithm 1. n=3f+1 is required for this protocol.
inline constexpr uint64_t FORGET_IT_DELTA_MS = 1000;
static_assert(FORGET_IT_DELTA_MS > 0 && FORGET_IT_DELTA_MS <= UINT64_MAX / 4);

// Retention and the existing Shabdiz forced-COLLECT experiment.
inline constexpr uint64_t MAX_VIEW_HISTORY = 1000;
inline constexpr uint64_t PBFT_HISTORY = 1000;
inline constexpr uint64_t NORMAL_PATH_WINDOW = 64;
inline constexpr uint64_t MAX_COLLECT_ATTEMPTS = 300;
inline constexpr int FORCE_COLLECT_AFTER_SEC = 3000;

// Updated Shabdiz only: T_w is independent of network delay and grows with w.
inline constexpr uint64_t SHABDIZ_VIEW_TIMEOUT_MS = 2000;
inline constexpr uint64_t SHABDIZ_VIEW_TIMEOUT_STEP_MS = 1;

// Benchmark ablation only. Disable collection, WISH/view-timeout recovery and
// COMMITTED-report catch-up. Preserve fast-path voting, normal view progress,
// data fetching and archival storage. A faulty leader can stall this mode.
// Keep false when evaluating the complete protocol. Rebuild every replica.
inline constexpr bool SHABDIZ_FAST_PATH_ONLY = false;

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
