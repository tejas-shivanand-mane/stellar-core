#pragma once

#include "overlay/ExperimentBatch.h"

namespace stellar
{
using ShabdizOperation = ExperimentOperation;
using ShabdizBatch = ExperimentBatch;

inline std::optional<ShabdizBatch>
parseShabdizBatch(std::string_view data, size_t batchSize)
{
    return parseExperimentBatch(data, batchSize, BatchRules::Shabdiz);
}
} // namespace stellar
