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
OverlayManagerImpl::advanceForgetIT(uint64_t seq)
{
    auto& slot = mNormalSlots.at(seq);
    std::vector<unsigned> outgoing;
    CustomMessageType firstVote;
    bool decided;

    firstVote = CUSTOM_FORGET_VOTE1;
    outgoing = slot.forget.advance(mApp.getConfig().NODE_SEED.getPublicKey(),
                                   mNormalQuorum);
    size_t f = (mNormalMembers.size() - 1) / 3;
    decided = slot.forget.decide(mNormalQuorum, f + 1);
    // Vote on the batch digest; the first-view vote-2 guard remains SAFE.
    for (auto phase : outgoing)
        normalSend(static_cast<CustomMessageType>(firstVote + phase - 1), seq,
                   slot.digest);
    return decided;
}

} // namespace stellar
