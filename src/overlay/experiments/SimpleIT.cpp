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
OverlayManagerImpl::advanceSimpleIT(uint64_t seq)
{
    auto& slot = mNormalSlots.at(seq);
    std::vector<unsigned> outgoing;
    CustomMessageType firstVote;
    bool decided;

    firstVote = CUSTOM_SIMPLE_ECHO;
    // arXiv:2606.14404v2 Figure 2 (non-speculative S/S_opt), healthy path:
    // a safe parent need not be committed. Vote, then enter the next round
    // immediately, overlapping this round's commit with the next RBC.
    bool safeParent = seq - 1 <= mNormalReady;
    outgoing = slot.simple.advance(mApp.getConfig().NODE_SEED.getPublicKey(),
                                   normalLeader(seq), mNormalMembers.size(),
                                   SIMPLE_IT_OPTIMISTIC, safeParent);
    decided = slot.simple.decide(mNormalQuorum, normalLeader(seq), safeParent);
    for (auto phase : outgoing)
    {
        auto type = static_cast<CustomMessageType>(firstVote + phase - 1);
        // Simple-IT section 3.2 / Figure 2: COMMIT identifies only the round.
        // RBC votes identify the proposal by digest. Execution still waits
        // for the validated full proposal and local RBC delivery.
        if (type == CUSTOM_SIMPLE_COMMIT)
            normalSend(type, seq, Hash{});
        else
            normalSend(type, seq, slot.digest);
    }
    if (slot.simple.commitSent() && seq == mNormalReady + 1)
        mNormalReady = seq;
    return decided;
}

} // namespace stellar
