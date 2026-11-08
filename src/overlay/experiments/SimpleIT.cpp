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
                                   mNormalLeader.value(), mNormalMembers.size(),
                                   SIMPLE_IT_OPTIMISTIC, safeParent);
    decided =
        slot.simple.decide(mNormalQuorum, mNormalLeader.value(), safeParent);
    for (auto phase : outgoing)
    {
        auto type = static_cast<CustomMessageType>(firstVote + phase - 1);
        // Simple-IT section 3.2 / Figure 2: COMMIT identifies only the round.
        // Basic Opt-RBC (2505.02761v4 Figure 1) carries m in ECHO/VOTE/ACK/
        // READY. We use the full batch as m, not the erasure-coded variant.
        if (type == CUSTOM_SIMPLE_COMMIT)
            normalSend(type, seq, Hash{});
        else
            normalSend(type, seq, slot.digest, slot.data);
    }
    return decided;
}

} // namespace stellar
