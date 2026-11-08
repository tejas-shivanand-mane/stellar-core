// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/experiments/ExperimentState.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

namespace stellar
{
using namespace experiment;

bool
OverlayManagerImpl::advanceTetraBFT(uint64_t seq)
{
    // arXiv:2405.02615v2 section 6.1: the healthy multi-shot protocol,
    // not repeated instances of Basic TetraBFT's four voting phases.
    auto& slot = mNormalSlots.at(seq);
    bool parentReady =
        seq == 1 ? slot.parent == Hash{}
                 : seq - 1 <= mNormalNotarized && mNormalSlots.count(seq - 1) &&
                       mNormalSlots.at(seq - 1).digest == slot.parent;
    if (!parentReady)
        return false;
    if (!slot.tetraVoted)
    {
        slot.tetraVoted = true;
        slot.tetraVotes.emplace(mApp.getConfig().NODE_SEED.getPublicKey(),
                                slot.digest);
        normalSend(CUSTOM_TETRA_VOTE1, seq, slot.digest);
        // The next leader may propose before this block is notarized.
        if (seq == mNormalReady + 1)
            mNormalReady = seq;
    }
    size_t votes = 0;
    for (auto const& vote : slot.tetraVotes)
        votes += vote.second == slot.digest;
    if (!slot.tetraNotarized && votes >= mNormalQuorum)
    {
        slot.tetraNotarized = true;
        if (seq == mNormalNotarized + 1)
            mNormalNotarized = seq;
        return seq >= 4; // four consecutively notarized, linked blocks
    }
    return false;
}

} // namespace stellar
