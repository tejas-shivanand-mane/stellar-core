#pragma once

#include "overlay/experiments/ExperimentConfig.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace stellar
{
// A deterministic increasing timeout shared by all replicas. Healthy batches
// also advance w, so use sublinear growth to avoid long waits for silent leaders.
inline std::chrono::steady_clock::duration
shabdizViewTimeout(uint64_t confirmedView)
{
    using Clock = std::chrono::steady_clock;
    using Milliseconds = std::chrono::duration<double, std::milli>;
    Milliseconds timeout{experiment::SHABDIZ_VIEW_TIMEOUT_MS +
                         experiment::SHABDIZ_VIEW_TIMEOUT_STEP_MS *
                             std::sqrt(static_cast<double>(confirmedView))};
    return std::chrono::duration_cast<Clock::duration>(
        std::min(timeout, Milliseconds(Clock::duration::max() / 4)));
}
}
