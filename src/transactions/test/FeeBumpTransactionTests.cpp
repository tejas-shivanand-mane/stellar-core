// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SponsorshipTestUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static void
sign(Hash const& networkID, SecretKey key, FeeBumpTransactionEnvelope& env)
{
    env.signatures.emplace_back(SignatureUtils::sign(
        key, sha256(xdr::xdr_to_opaque(networkID, ENVELOPE_TYPE_TX_FEE_BUMP,
                                       env.tx))));
}

static TransactionEnvelope
feeBumpUnsigned(TestAccount& feeSource, TestAccount& source, TestAccount& dest,
                int64_t outerFee, uint32_t innerFee, int64_t amount)
{
    TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
    fb.feeBump().tx.feeSource = toMuxedAccount(feeSource);
    fb.feeBump().tx.fee = outerFee;

    auto& env = fb.feeBump().tx.innerTx;
    env.type(ENVELOPE_TYPE_TX);
    env.v1().tx.sourceAccount = toMuxedAccount(source);
    env.v1().tx.fee = innerFee;
    env.v1().tx.seqNum = source.nextSequenceNumber();
    env.v1().tx.operations = {payment(dest, amount)};

    return fb;
}

static TransactionTestFramePtr
feeBump(Hash const& networkID, TestAccount& feeSource, TestAccount& source,
        TestAccount& dest, int64_t outerFee, uint32_t innerFee, int64_t amount)
{
    auto fb =
        feeBumpUnsigned(feeSource, source, dest, outerFee, innerFee, amount);
    auto& env = fb.feeBump().tx.innerTx;
    sign(networkID, source, env.v1());
    sign(networkID, feeSource, fb.feeBump());
    auto tx = TransactionFrameBase::makeTransactionFromWire(networkID, fb);
    return TransactionTestFrame::fromTxFrame(tx);
}

TEST_CASE_VERSIONS("fee bump transactions", "[tx][feebump]")
{
    VirtualClock clock;
    auto app = createTestApplication(
        clock, getTestConfig(0, Config::TESTDB_IN_MEMORY));

    auto& lm = app->getLedgerManager();
    auto fee = lm.getLastClosedLedgerHeader().header.baseFee;
    auto reserve = lm.getLastClosedLedgerHeader().header.baseReserve;

    auto root = app->getRoot();

    SECTION("validity")
    {
        SECTION("not supported")
        {
            for_versions({12}, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), *root, *root, *root,
                                  2 * fee, fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txNOT_SUPPORTED);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("insufficient fee, less than min")
        {
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), *root, *root, *root,
                                  2 * fee - 1, 1, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txINSUFFICIENT_FEE);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("insufficient fee, rate less than inner")
        {
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), *root, *root, *root,
                                  2 * fee + 1, 101, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txINSUFFICIENT_FEE);
                REQUIRE(result->getFeeCharged() == 2 * 101);
            });
        }

        SECTION("fee source does not exist")
        {
            auto acc = TestAccount(*app, getAccount("A"));
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txNO_ACCOUNT);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("bad signatures, signature missing")
        {
            auto acc = root->create("A", 2 * reserve);
            for_versions_from(13, *app, [&] {
                auto fbXDR =
                    feeBumpUnsigned(acc, *root, *root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), *root,
                     fbXDR.feeBump().tx.innerTx.v1());
                auto fb = TransactionTestFrame::fromTxFrame(
                    TransactionFrameBase::makeTransactionFromWire(
                        app->getNetworkID(), fbXDR));
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txBAD_AUTH);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("bad signatures, signature invalid")
        {
            auto acc = root->create("A", 2 * reserve);
            for_versions_from(13, *app, [&] {
                auto fbXDR =
                    feeBumpUnsigned(acc, *root, *root, 2 * fee, fee, 1);
                // These signatures are applied in the wrong order, so the outer
                // signature is invalid
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                sign(app->getNetworkID(), *root,
                     fbXDR.feeBump().tx.innerTx.v1());
                auto fb = TransactionTestFrame::fromTxFrame(
                    TransactionFrameBase::makeTransactionFromWire(
                        app->getNetworkID(), fbXDR));
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txBAD_AUTH);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("insufficient balance")
        {
            auto acc = root->create("A", 2 * reserve);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txINSUFFICIENT_BALANCE);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("extra signatures")
        {
            auto acc = root->create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fbXDR =
                    feeBumpUnsigned(acc, *root, *root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), *root,
                     fbXDR.feeBump().tx.innerTx.v1());
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                sign(app->getNetworkID(), *root, fbXDR.feeBump());
                auto fb = TransactionTestFrame::fromTxFrame(
                    TransactionFrameBase::makeTransactionFromWire(
                        app->getNetworkID(), fbXDR));
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txBAD_AUTH_EXTRA);
                REQUIRE(result->getFeeCharged() == 2 * fee);
            });
        }

        SECTION("inner transaction invalid, transaction level")
        {
            auto acc = root->create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fbXDR =
                    feeBumpUnsigned(acc, *root, *root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                auto fb = TransactionTestFrame::fromTxFrame(
                    TransactionFrameBase::makeTransactionFromWire(
                        app->getNetworkID(), fbXDR));
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_FAILED);
                auto const& fbRes = result->getXDR();
                REQUIRE(fbRes.feeCharged == 2 * fee);
                auto const& innerRes = fbRes.result.innerResultPair().result;
                REQUIRE(innerRes.feeCharged == 0);
                REQUIRE(innerRes.result.code() == txBAD_AUTH);
            });
        }

        SECTION("inner transaction invalid, operation level")
        {
            auto acc = root->create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, -1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(!result->isSuccess());
                REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_FAILED);
                auto const& fbRes = result->getXDR();
                REQUIRE(fbRes.feeCharged == 2 * fee);
                auto const& innerRes = fbRes.result.innerResultPair().result;
                REQUIRE(innerRes.feeCharged == 0);
                REQUIRE(innerRes.result.code() == txFAILED);
                auto const& payRes =
                    innerRes.result.results()[0].tr().paymentResult();
                REQUIRE(payRes.code() == PAYMENT_MALFORMED);
            });
        }

        SECTION("valid")
        {
            auto acc = root->create("A", 2 * reserve + 2 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, 1);
                LedgerTxn ltx(app->getLedgerTxnRoot());
                auto result =
                    fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0);
                REQUIRE(result->isSuccess());
                REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
                auto const& fbRes = result->getXDR();
                REQUIRE(fbRes.feeCharged == 2 * fee);
                auto const& innerRes = fbRes.result.innerResultPair().result;
                REQUIRE(innerRes.result.results().size() == 1);
                REQUIRE(innerRes.result.results()[0].code() == opINNER);
                auto const& payRes =
                    innerRes.result.results()[0].tr().paymentResult();
                REQUIRE(payRes.code() == PAYMENT_SUCCESS);
            });
        }
    }

    SECTION("fee processing")
    {
        auto acc = root->create("A", 2 * reserve + 2 * fee);
        for_versions_from(13, *app, [&] {
            auto fb = static_cast<TransactionFrameBaseConstPtr>(feeBump(
                app->getNetworkID(), acc, *root, *root, 2 * fee, fee, 1));
            LedgerTxn ltx(app->getLedgerTxnRoot());
            auto result = fb->processFeeSeqNum(ltx, fee);
            auto delta = ltx.getDelta();
            REQUIRE(delta.entry.size() == 1);
            auto gkey = delta.entry.begin()->first;
            REQUIRE(gkey.type() == InternalLedgerEntryType::LEDGER_ENTRY);
            REQUIRE(gkey.ledgerKey().account().accountID == acc.getPublicKey());
            auto entryDelta = delta.entry.begin()->second;
            auto prev = entryDelta.previous->ledgerEntry().data.account();
            auto curr = entryDelta.current->ledgerEntry().data.account();
            REQUIRE(prev.balance == curr.balance + 2 * fee);
            REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_SUCCESS);
            REQUIRE(result->getFeeCharged() == 2 * fee);
            REQUIRE(
                result->getXDR().result.innerResultPair().result.feeCharged ==
                fee);
        });
    }

    SECTION("apply")
    {
        SECTION("fee source does not exist")
        {
            auto acc = root->create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }

                MutableTxResultPtr result;
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    result = fb->processFeeSeqNum(ltx, fee);
                    ltx.commit();
                }
                acc.merge(*root);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder meta(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    REQUIRE(
                        fb->apply(app->getAppConnector(), ltx, meta, *result));
                    REQUIRE(result->getResultCode() ==
                            txFEE_BUMP_INNER_SUCCESS);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    REQUIRE(result->getXDR()
                                .result.innerResultPair()
                                .result.feeCharged == 100);
                }
            });
        }

        SECTION("bad signatures")
        {
            auto acc = root->create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }
                acc.setOptions(setMasterWeight(0));
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder meta(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    auto result = fb->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        fb->apply(app->getAppConnector(), ltx, meta, *result));
                    REQUIRE(result->getResultCode() ==
                            txFEE_BUMP_INNER_SUCCESS);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    REQUIRE(result->getXDR()
                                .result.innerResultPair()
                                .result.feeCharged == fee);
                }
            });
        }

        SECTION("insufficient balance")
        {
            auto acc = root->create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, *root,
                                  2 * fee, fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }
                acc.pay(*root, 2 * fee);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder meta(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    auto result = fb->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        fb->apply(app->getAppConnector(), ltx, meta, *result));
                    REQUIRE(result->getResultCode() ==
                            txFEE_BUMP_INNER_SUCCESS);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    REQUIRE(result->getXDR()
                                .result.innerResultPair()
                                .result.feeCharged == fee);
                }
            });
        }

        SECTION("extra signatures")
        {
            auto acc = root->create("A", 3 * reserve + 4 * fee);
            acc.setOptions(setSigner(makeSigner(*root, 1)) |
                           setLowThreshold(2));
            for_versions_from(13, *app, [&] {
                auto fbXDR =
                    feeBumpUnsigned(acc, *root, *root, 2 * fee, fee, 1);
                sign(app->getNetworkID(), *root,
                     fbXDR.feeBump().tx.innerTx.v1());
                sign(app->getNetworkID(), acc, fbXDR.feeBump());
                sign(app->getNetworkID(), *root, fbXDR.feeBump());
                auto rawTx = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                auto fb = TransactionTestFrame::fromTxFrame(rawTx);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }

                auto setOptionsTx = acc.tx({setOptions(setLowThreshold(1))});
                setOptionsTx->addSignature(*root);
                applyCheck(setOptionsTx, *app);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder meta(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    auto result = fb->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        fb->apply(app->getAppConnector(), ltx, meta, *result));
                    REQUIRE(result->getResultCode() ==
                            txFEE_BUMP_INNER_SUCCESS);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    REQUIRE(result->getXDR()
                                .result.innerResultPair()
                                .result.feeCharged == fee);
                }
            });
        }

        SECTION("inner transaction fails, transaction level")
        {
            auto acc = root->create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, acc, 2 * fee,
                                  fee, 1);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }

                auto setOptionsOp = setOptions(setMasterWeight(0));
                setOptionsOp.sourceAccount.activate() = toMuxedAccount(*root);
                auto setOptionsTx = acc.tx({setOptionsOp});
                setOptionsTx->addSignature(*root);
                applyCheck(setOptionsTx, *app);

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder meta(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    auto result = fb->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        !fb->apply(app->getAppConnector(), ltx, meta, *result));
                    REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_FAILED);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    auto const& innerRes =
                        result->getXDR().result.innerResultPair().result;
                    REQUIRE(innerRes.feeCharged == fee);
                    REQUIRE(innerRes.result.code() == txBAD_AUTH);
                }
            });
        }

        SECTION("inner transaction fails, operation level")
        {
            auto acc = root->create("A", 2 * reserve + 3 * fee);
            for_versions_from(13, *app, [&] {
                auto fb = feeBump(app->getNetworkID(), acc, *root, acc, 2 * fee,
                                  fee, INT64_MAX);
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder meta(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    auto result = fb->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        !fb->apply(app->getAppConnector(), ltx, meta, *result));
                    REQUIRE(result->getResultCode() == txFEE_BUMP_INNER_FAILED);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    auto const& innerRes =
                        result->getXDR().result.innerResultPair().result;
                    REQUIRE(innerRes.feeCharged == fee);
                    REQUIRE(innerRes.result.code() == txFAILED);
                    REQUIRE(innerRes.result.results()[0].code() == opINNER);
                    auto const& payRes =
                        innerRes.result.results()[0].tr().paymentResult();
                    REQUIRE(payRes.code() == PAYMENT_LINE_FULL);
                }
            });
        }

        SECTION("one-time signer removal")
        {
            auto acc = root->create("A", 3 * reserve + 3 * fee);
            auto sponsoring = root->create("sponsoring", 3 * reserve);

            auto signerTest = [&](bool isFbSignerSponsored) {
                auto fbXDR =
                    feeBumpUnsigned(acc, *root, *root, 2 * fee, fee, 1);
                ++fbXDR.feeBump().tx.innerTx.v1().tx.seqNum;

                auto rawTx = TransactionFrameBase::makeTransactionFromWire(
                    app->getNetworkID(), fbXDR);
                auto fb = TransactionTestFrame::fromTxFrame(rawTx);

                SignerKey txSigner(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                txSigner.preAuthTx() = sha256(
                    xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                       fbXDR.feeBump().tx.innerTx.v1().tx));
                root->loadSequenceNumber();
                root->setOptions(setSigner(Signer{txSigner, 1}));

                SignerKey fbSigner(SIGNER_KEY_TYPE_PRE_AUTH_TX);
                fbSigner.preAuthTx() = sha256(xdr::xdr_to_opaque(
                    app->getNetworkID(), ENVELOPE_TYPE_TX_FEE_BUMP,
                    fbXDR.feeBump().tx));

                if (isFbSignerSponsored)
                {
                    auto tx = transactionFrameFromOps(
                        app->getNetworkID(), acc,
                        {sponsoring.op(beginSponsoringFutureReserves(acc)),
                         acc.op(setOptions(setSigner(Signer{fbSigner, 1}))),
                         acc.op(endSponsoringFutureReserves())},
                        {sponsoring});

                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder txm(
                        true, *tx, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    REQUIRE(tx->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                    auto result = tx->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        tx->apply(app->getAppConnector(), ltx, txm, *result));
                    REQUIRE(result->getResultCode() == txSUCCESS);

                    checkSponsorship(ltx, acc, fbSigner, 2,
                                     &sponsoring.getPublicKey());
                    ltx.commit();
                }
                else
                {
                    acc.setOptions(setSigner(Signer{fbSigner, 1}));
                }

                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    REQUIRE(fb->checkValid(app->getAppConnector(), ltx, 0, 0, 0)
                                ->isSuccess());
                }
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    TransactionMetaBuilder metaBuilder(
                        true, *fb, ltx.loadHeader().current().ledgerVersion,
                        app->getAppConnector());
                    auto result = fb->processFeeSeqNum(ltx, fee);
                    REQUIRE(
                        fb->apply(app->getAppConnector(), ltx, metaBuilder));
                    TransactionMetaFrame meta(metaBuilder.finalize(true));
                    REQUIRE(result->getResultCode() ==
                            txFEE_BUMP_INNER_SUCCESS);
                    REQUIRE(result->getFeeCharged() == 2 * fee);
                    REQUIRE(result->getXDR()
                                .result.innerResultPair()
                                .result.feeCharged == fee);
                    REQUIRE(meta.getNumChangesBefore() ==
                            (isFbSignerSponsored ? 6 : 4));
                    for (auto const& change : meta.getChangesBefore())
                    {
                        if (change.type() == LEDGER_ENTRY_STATE)
                        {
                            auto const& ae = change.state().data.account();
                            // The sponsoring account doesn't have any signers,
                            // but the account can still change due to
                            // sponsorship
                            REQUIRE(
                                (ae.accountID == sponsoring.getPublicKey() ||
                                 ae.signers.size() == 1));
                        }
                        else if (change.type() == LEDGER_ENTRY_UPDATED)
                        {
                            auto const& ae = change.updated().data.account();
                            REQUIRE(ae.signers.empty());
                        }
                    }
                    ltx.commit();
                }

                REQUIRE(getAccountSigners(*root, *app).size() == 0);
                REQUIRE(getAccountSigners(acc, *app).size() == 0);

                if (isFbSignerSponsored)
                {
                    LedgerTxn ltx(app->getLedgerTxnRoot());
                    checkSponsorship(ltx, acc, 0, nullptr, 0, 2, 0, 0);
                    checkSponsorship(ltx, sponsoring, 0, nullptr, 0, 2, 0, 0);
                }
            };

            SECTION("not sponsored")
            {
                for_versions_from(13, *app, [&] { signerTest(false); });
            }
            SECTION("sponsored")
            {
                for_versions_from(14, *app, [&] { signerTest(true); });
            }
        }
    }
}
