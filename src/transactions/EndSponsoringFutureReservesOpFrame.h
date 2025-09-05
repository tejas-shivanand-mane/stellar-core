// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{

class EndSponsoringFutureReservesOpFrame : public OperationFrame
{
    bool isOpSupported(LedgerHeader const& header) const override;

    EndSponsoringFutureReservesResult&
    innerResult(OperationResult& res) const
    {
        return res.tr().endSponsoringFutureReservesResult();
    }

  public:
    EndSponsoringFutureReservesOpFrame(Operation const& op,
                                       TransactionFrame const& parentTx);

    bool doApply(AppConnector& app, AbstractLedgerTxn& ltx,
                 Hash const& sorobanBasePrngSeed, OperationResult& res,
                 std::optional<RefundableFeeTracker>& refundableFeeTracker,
                 OperationMetaBuilder& opMeta) const override;
    bool doCheckValid(uint32_t ledgerVersion,
                      OperationResult& res) const override;

    static EndSponsoringFutureReservesResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().endSponsoringFutureReservesResult().code();
    }
};
}
