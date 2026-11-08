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
    auto& slot = mNormalSlots.at(seq);
    std::vector<unsigned> outgoing;
    CustomMessageType firstVote;
    bool decided;

    firstVote = CUSTOM_TETRA_VOTE1;
    outgoing = slot.tetra.advance(mApp.getConfig().NODE_SEED.getPublicKey(),
                                  mNormalQuorum);
    decided = slot.tetra.decide(mNormalQuorum);
    // The batch digest identifies the proposed value; data stays in the slot.
    for (auto phase : outgoing)
        normalSend(static_cast<CustomMessageType>(firstVote + phase - 1), seq,
                   slot.digest);
    return decided;
}

} // namespace stellar
