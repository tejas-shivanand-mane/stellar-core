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
    // Consecutive parent is safe once already executed. Serialized proposals
    // deliberately omit Simple-IT's pipeline and disabled-round machinery.
    bool safeParent = seq - 1 <= mNormalExecuted;
    outgoing = slot.simple.advance(mApp.getConfig().NODE_SEED.getPublicKey(),
                                   experimentLeader(seq), mNormalMembers.size(),
                                   SIMPLE_IT_OPTIMISTIC, safeParent);
    decided =
        slot.simple.decide(mNormalQuorum, experimentLeader(seq), safeParent);
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
    return decided;
}

} // namespace stellar
