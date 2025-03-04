// Copyright 2023 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/RestoreFootprintOpFrame.h"
#include "TransactionUtils.h"
#include <Tracy.hpp>

namespace hcnet
{

struct RestoreFootprintMetrics
{
    medida::MetricsRegistry& mMetrics;

    uint32_t mLedgerReadByte{0};
    uint32_t mLedgerWriteByte{0};

    RestoreFootprintMetrics(medida::MetricsRegistry& metrics)
        : mMetrics(metrics)
    {
    }

    ~RestoreFootprintMetrics()
    {
        mMetrics
            .NewMeter({"soroban", "restore-fprint-op", "read-ledger-byte"},
                      "byte")
            .Mark(mLedgerReadByte);
        mMetrics
            .NewMeter({"soroban", "restore-fprint-op", "write-ledger-byte"},
                      "byte")
            .Mark(mLedgerWriteByte);
    }
};

RestoreFootprintOpFrame::RestoreFootprintOpFrame(Operation const& op,
                                                 OperationResult& res,
                                                 TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mRestoreFootprintOp(mOperation.body.restoreFootprintOp())
{
}

bool
RestoreFootprintOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
RestoreFootprintOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error("RestoreFootprintOpFrame::doApply needs Config");
}

bool
RestoreFootprintOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx,
                                 Hash const& sorobanBasePrngSeed)
{
    ZoneNamedN(applyZone, "RestoreFootprintOpFrame apply", true);

    RestoreFootprintMetrics metrics(app.getMetrics());

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto ledgerSeq = ltx.loadHeader().current().ledgerSeq;
    auto const& sorobanConfig =
        app.getLedgerManager().getSorobanNetworkConfig();

    auto const& archivalSettings = sorobanConfig.stateArchivalSettings();
    rust::Vec<CxxLedgerEntryRentChange> rustEntryRentChanges;
    // Extend the TTL on the restored entry to minimum TTL, including
    // the current ledger.
    uint32_t restoredLiveUntilLedger =
        ledgerSeq + archivalSettings.minPersistentTTL - 1;
    rustEntryRentChanges.reserve(footprint.readWrite.size());
    for (auto const& lk : footprint.readWrite)
    {
        auto ttlKey = getTTLKey(lk);
        {
            auto constTTLLtxe = ltx.loadWithoutRecord(ttlKey);
            // Skip entry if the TTLEntry is missing or if it's already live.
            if (!constTTLLtxe || isLive(constTTLLtxe.current(), ledgerSeq))
            {
                continue;
            }
        }

        // We must load the ContractCode/ContractData entry for fee purposes, as
        // restore is considered a write
        auto constEntryLtxe = ltx.loadWithoutRecord(lk);

        // We checked for TTLEntry existence above
        releaseAssertOrThrow(constEntryLtxe);

        uint32_t entrySize =
            static_cast<uint32>(xdr::xdr_size(constEntryLtxe.current()));
        metrics.mLedgerReadByte += entrySize;
        if (resources.readBytes < metrics.mLedgerReadByte)
        {
            mParentTx.pushSimpleDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-read resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerReadByte),
                 makeU64SCVal(resources.readBytes)});
            innerResult().code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        // To maintain consistency with InvokeHostFunction, TTLEntry
        // writes come out of refundable fee, so only add entrySize
        metrics.mLedgerWriteByte += entrySize;
        if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                         mParentTx))
        {
            innerResult().code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        if (resources.writeBytes < metrics.mLedgerWriteByte)
        {
            mParentTx.pushSimpleDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation byte-write resources exceeds amount specified",
                {makeU64SCVal(metrics.mLedgerWriteByte),
                 makeU64SCVal(resources.writeBytes)});
            innerResult().code(RESTORE_FOOTPRINT_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        rustEntryRentChanges.emplace_back();
        auto& rustChange = rustEntryRentChanges.back();
        rustChange.is_persistent = true;
        // Treat the entry as if it hasn't existed before restoration
        // for the rent fee purposes.
        rustChange.old_size_bytes = 0;
        rustChange.old_live_until_ledger = 0;
        rustChange.new_size_bytes = entrySize;
        rustChange.new_live_until_ledger = restoredLiveUntilLedger;

        // Entry exists if we get this this point due to the constTTLLtxe
        // loadWithoutRecord logic above.
        auto ttlLtxe = ltx.load(ttlKey);
        ttlLtxe.current().data.ttl().liveUntilLedgerSeq =
            restoredLiveUntilLedger;
    }
    uint32_t ledgerVersion = ltx.loadHeader().current().ledgerVersion;
    int64_t rentFee = rust_bridge::compute_rent_fee(
        app.getConfig().CURRENT_LEDGER_PROTOCOL_VERSION, ledgerVersion,
        rustEntryRentChanges,
        app.getLedgerManager()
            .getSorobanNetworkConfig()
            .rustBridgeRentFeeConfiguration(),
        ledgerSeq);
    if (!mParentTx.consumeRefundableSorobanResources(
            0, rentFee, ltx.loadHeader().current().ledgerVersion,
            app.getLedgerManager().getSorobanNetworkConfig(), app.getConfig()))
    {
        innerResult().code(RESTORE_FOOTPRINT_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }
    innerResult().code(RESTORE_FOOTPRINT_SUCCESS);
    return true;
}

bool
RestoreFootprintOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                      uint32_t ledgerVersion)
{
    auto const& footprint = mParentTx.sorobanResources().footprint;
    if (!footprint.readOnly.empty())
    {
        innerResult().code(RESTORE_FOOTPRINT_MALFORMED);
        return false;
    }

    for (auto const& lk : footprint.readWrite)
    {
        if (!isPersistentEntry(lk))
        {
            innerResult().code(RESTORE_FOOTPRINT_MALFORMED);
            return false;
        }
    }

    return true;
}

bool
RestoreFootprintOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    throw std::runtime_error(
        "RestoreFootprintOpFrame::doCheckValid needs Config");
}

void
RestoreFootprintOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
RestoreFootprintOpFrame::isSoroban() const
{
    return true;
}

ThresholdLevel
RestoreFootprintOpFrame::getThresholdLevel() const
{
    return ThresholdLevel::LOW;
}
}