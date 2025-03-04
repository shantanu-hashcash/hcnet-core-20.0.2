// Copyright 2022 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// clang-format off
// This needs to be included first
#include "TransactionUtils.h"
#include "util/GlobalChecks.h"
#include "xdr/Hcnet-ledger-entries.h"
#include <cstdint>
#include <json/json.h>
#include <medida/metrics_registry.h>
#include <xdrpp/types.h>
#include "xdr/Hcnet-contract.h"
#include "rust/RustVecXdrMarshal.h"
// clang-format on

#include "ledger/LedgerTxnImpl.h"
#include "rust/CppShims.h"
#include "xdr/Hcnet-transaction.h"
#include <stdexcept>
#include <xdrpp/xdrpp/printer.h>

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "rust/RustBridge.h"
#include "transactions/InvokeHostFunctionOpFrame.h"
#include <Tracy.hpp>
#include <crypto/SHA.h>

namespace hcnet
{
namespace
{
bool
isCodeKey(LedgerKey const& lk)
{
    return lk.type() == CONTRACT_CODE;
}

template <typename T>
std::vector<uint8_t>
toVec(T const& t)
{
    return std::vector<uint8_t>(xdr::xdr_to_opaque(t));
}

template <typename T>
CxxBuf
toCxxBuf(T const& t)
{
    return CxxBuf{std::make_unique<std::vector<uint8_t>>(toVec(t))};
}

CxxLedgerInfo
getLedgerInfo(AbstractLedgerTxn& ltx, Application& app,
              SorobanNetworkConfig const& sorobanConfig)
{
    CxxLedgerInfo info{};
    auto const& hdr = ltx.loadHeader().current();
    info.base_reserve = hdr.baseReserve;
    info.protocol_version = hdr.ledgerVersion;
    info.sequence_number = hdr.ledgerSeq;
    info.timestamp = hdr.scpValue.closeTime;
    info.memory_limit = sorobanConfig.txMemoryLimit();
    info.min_persistent_entry_ttl =
        sorobanConfig.stateArchivalSettings().minPersistentTTL;
    info.min_temp_entry_ttl =
        sorobanConfig.stateArchivalSettings().minTemporaryTTL;
    info.max_entry_ttl = sorobanConfig.stateArchivalSettings().maxEntryTTL;
    info.cpu_cost_params = toCxxBuf(sorobanConfig.cpuCostParams());
    info.mem_cost_params = toCxxBuf(sorobanConfig.memCostParams());
    auto& networkID = app.getNetworkID();
    info.network_id.reserve(networkID.size());
    for (auto c : networkID)
    {
        info.network_id.emplace_back(static_cast<unsigned char>(c));
    }
    return info;
}

DiagnosticEvent
metricsEvent(bool success, std::string&& topic, uint64_t value)
{
    DiagnosticEvent de;
    de.inSuccessfulContractCall = success;
    de.event.type = ContractEventType::DIAGNOSTIC;
    SCVec topics = {
        makeSymbolSCVal("core_metrics"),
        makeSymbolSCVal(std::move(topic)),
    };
    de.event.body.v0().topics = topics;
    de.event.body.v0().data = makeU64SCVal(value);
    return de;
}

} // namespace

struct HostFunctionMetrics
{
    medida::MetricsRegistry& mMetrics;

    uint32_t mReadEntry{0};
    uint32_t mWriteEntry{0};

    uint32_t mLedgerReadByte{0};
    uint32_t mLedgerWriteByte{0};

    uint32_t mReadKeyByte{0};
    uint32_t mWriteKeyByte{0};

    uint32_t mReadDataByte{0};
    uint32_t mWriteDataByte{0};

    uint32_t mReadCodeByte{0};
    uint32_t mWriteCodeByte{0};

    uint32_t mEmitEvent{0};
    uint32_t mEmitEventByte{0};

    // host runtime metrics
    uint64_t mCpuInsn{0};
    uint64_t mMemByte{0};
    uint64_t mInvokeTimeNsecs{0};
    uint64_t mCpuInsnExclVm{0};
    uint64_t mInvokeTimeNsecsExclVm{0};

    // max single entity size metrics
    uint32_t mMaxReadWriteKeyByte{0};
    uint32_t mMaxReadWriteDataByte{0};
    uint32_t mMaxReadWriteCodeByte{0};
    uint32_t mMaxEmitEventByte{0};

    bool mSuccess{false};

    HostFunctionMetrics(medida::MetricsRegistry& metrics) : mMetrics(metrics)
    {
    }

    void
    noteReadEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize)
    {
        mReadEntry++;
        mReadKeyByte += keySize;
        mMaxReadWriteKeyByte = std::max(mMaxReadWriteKeyByte, keySize);
        mLedgerReadByte += entrySize;
        if (isCodeEntry)
        {
            mReadCodeByte += entrySize;
            mMaxReadWriteCodeByte = std::max(mMaxReadWriteCodeByte, entrySize);
        }
        else
        {
            mReadDataByte += entrySize;
            mMaxReadWriteDataByte = std::max(mMaxReadWriteDataByte, entrySize);
        }
    }

    void
    noteWriteEntry(bool isCodeEntry, uint32_t keySize, uint32_t entrySize)
    {
        mWriteEntry++;
        mMaxReadWriteKeyByte = std::max(mMaxReadWriteKeyByte, keySize);
        mLedgerWriteByte += entrySize;
        if (isCodeEntry)
        {
            mWriteCodeByte += entrySize;
            mMaxReadWriteCodeByte = std::max(mMaxReadWriteCodeByte, entrySize);
        }
        else
        {
            mWriteDataByte += entrySize;
            mMaxReadWriteDataByte = std::max(mMaxReadWriteDataByte, entrySize);
        }
    }

    ~HostFunctionMetrics()
    {
        mMetrics.NewMeter({"soroban", "host-fn-op", "read-entry"}, "entry")
            .Mark(mReadEntry);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-entry"}, "entry")
            .Mark(mWriteEntry);

        mMetrics.NewMeter({"soroban", "host-fn-op", "read-key-byte"}, "byte")
            .Mark(mReadKeyByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-key-byte"}, "byte")
            .Mark(mWriteKeyByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "read-ledger-byte"}, "byte")
            .Mark(mLedgerReadByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "read-data-byte"}, "byte")
            .Mark(mReadDataByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "read-code-byte"}, "byte")
            .Mark(mReadCodeByte);

        mMetrics
            .NewMeter({"soroban", "host-fn-op", "write-ledger-byte"}, "byte")
            .Mark(mLedgerWriteByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-data-byte"}, "byte")
            .Mark(mWriteDataByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "write-code-byte"}, "byte")
            .Mark(mWriteCodeByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "emit-event"}, "event")
            .Mark(mEmitEvent);
        mMetrics.NewMeter({"soroban", "host-fn-op", "emit-event-byte"}, "byte")
            .Mark(mEmitEventByte);

        mMetrics.NewMeter({"soroban", "host-fn-op", "cpu-insn"}, "insn")
            .Mark(mCpuInsn);
        mMetrics.NewMeter({"soroban", "host-fn-op", "mem-byte"}, "byte")
            .Mark(mMemByte);
        mMetrics
            .NewMeter({"soroban", "host-fn-op", "invoke-time-nsecs"}, "time")
            .Mark(mInvokeTimeNsecs);
        mMetrics.NewMeter({"soroban", "host-fn-op", "cpu-insn-excl-vm"}, "insn")
            .Mark(mCpuInsnExclVm);
        mMetrics
            .NewMeter({"soroban", "host-fn-op", "invoke-time-nsecs-excl-vm"},
                      "time")
            .Mark(mInvokeTimeNsecsExclVm);

        mMetrics.NewMeter({"soroban", "host-fn-op", "max-rw-key-byte"}, "byte")
            .Mark(mMaxReadWriteKeyByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "max-rw-data-byte"}, "byte")
            .Mark(mMaxReadWriteDataByte);
        mMetrics.NewMeter({"soroban", "host-fn-op", "max-rw-code-byte"}, "byte")
            .Mark(mMaxReadWriteCodeByte);
        mMetrics
            .NewMeter({"soroban", "host-fn-op", "max-emit-event-byte"}, "byte")
            .Mark(mMaxEmitEventByte);

        if (mSuccess)
        {
            mMetrics.NewMeter({"soroban", "host-fn-op", "success"}, "call")
                .Mark();
        }
        else
        {
            mMetrics.NewMeter({"soroban", "host-fn-op", "failure"}, "call")
                .Mark();
        }
    }
    medida::TimerContext
    getExecTimer()
    {
        return mMetrics.NewTimer({"soroban", "host-fn-op", "exec"}).TimeScope();
    }
};

InvokeHostFunctionOpFrame::InvokeHostFunctionOpFrame(Operation const& op,
                                                     OperationResult& res,
                                                     TransactionFrame& parentTx)
    : OperationFrame(op, res, parentTx)
    , mInvokeHostFunction(mOperation.body.invokeHostFunctionOp())
{
}

bool
InvokeHostFunctionOpFrame::isOpSupported(LedgerHeader const& header) const
{
    return header.ledgerVersion >= 20;
}

bool
InvokeHostFunctionOpFrame::doApply(AbstractLedgerTxn& ltx)
{
    throw std::runtime_error(
        "InvokeHostFunctionOpFrame::doApply needs Config and base PRNG seed");
}

void
InvokeHostFunctionOpFrame::maybePopulateDiagnosticEvents(
    Config const& cfg, InvokeHostFunctionOutput const& output,
    HostFunctionMetrics const& metrics)
{
    if (cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS)
    {
        xdr::xvector<DiagnosticEvent> diagnosticEvents;
        diagnosticEvents.reserve(output.diagnostic_events.size() + 20);
        for (auto const& e : output.diagnostic_events)
        {
            DiagnosticEvent evt;
            xdr::xdr_from_opaque(e.data, evt);
            diagnosticEvents.emplace_back(evt);
            CLOG_DEBUG(Tx, "Soroban diagnostic event: {}",
                       xdr::xdr_to_string(evt));
        }

        // add additional diagnostic events for metrics
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "read_entry", metrics.mReadEntry));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "write_entry", metrics.mWriteEntry));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "ledger_read_byte", metrics.mLedgerReadByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "ledger_write_byte", metrics.mLedgerWriteByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "read_key_byte", metrics.mReadKeyByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_key_byte", metrics.mWriteKeyByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "read_data_byte", metrics.mReadDataByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_data_byte", metrics.mWriteDataByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "read_code_byte", metrics.mReadCodeByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "write_code_byte", metrics.mWriteCodeByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "emit_event", metrics.mEmitEvent));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "emit_event_byte", metrics.mEmitEventByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "cpu_insn", metrics.mCpuInsn));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "mem_byte", metrics.mMemByte));
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "invoke_time_nsecs", metrics.mInvokeTimeNsecs));
        // skip publishing `cpu_insn_excl_vm` and `invoke_time_nsecs_excl_vm`,
        // we are mostly interested in those internally
        diagnosticEvents.emplace_back(metricsEvent(
            metrics.mSuccess, "max_rw_key_byte", metrics.mMaxReadWriteKeyByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_data_byte",
                         metrics.mMaxReadWriteDataByte));
        diagnosticEvents.emplace_back(
            metricsEvent(metrics.mSuccess, "max_rw_code_byte",
                         metrics.mMaxReadWriteCodeByte));
        diagnosticEvents.emplace_back(metricsEvent(metrics.mSuccess,
                                                   "max_emit_event_byte",
                                                   metrics.mMaxEmitEventByte));

        mParentTx.pushDiagnosticEvents(std::move(diagnosticEvents));
    }
}

bool
InvokeHostFunctionOpFrame::doApply(Application& app, AbstractLedgerTxn& ltx,
                                   Hash const& sorobanBasePrngSeed)
{
    ZoneNamedN(applyZone, "InvokeHostFunctionOpFrame apply", true);

    Config const& cfg = app.getConfig();
    HostFunctionMetrics metrics(app.getMetrics());
    auto const& sorobanConfig =
        app.getLedgerManager().getSorobanNetworkConfig();

    // Get the entries for the footprint
    rust::Vec<CxxBuf> ledgerEntryCxxBufs;
    rust::Vec<CxxBuf> ttlEntryCxxBufs;

    auto const& resources = mParentTx.sorobanResources();
    auto const& footprint = resources.footprint;
    auto footprintLength =
        footprint.readOnly.size() + footprint.readWrite.size();

    ledgerEntryCxxBufs.reserve(footprintLength);
    ttlEntryCxxBufs.reserve(footprintLength);

    auto addReads = [&ledgerEntryCxxBufs, &ttlEntryCxxBufs, &ltx, &metrics,
                     &resources, &sorobanConfig,
                     this](auto const& keys) -> bool {
        for (auto const& lk : keys)
        {
            uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
            uint32_t entrySize = 0u;
            std::optional<TTLEntry> ttlEntry;
            bool sorobanEntryLive = false;

            // For soroban entries, check if the entry is expired before loading
            if (isSorobanEntry(lk))
            {
                auto ttlKey = getTTLKey(lk);
                auto ttlLtxe = ltx.loadWithoutRecord(ttlKey);
                if (ttlLtxe)
                {
                    if (!isLive(ttlLtxe.current(), ltx.getHeader().ledgerSeq))
                    {
                        // For temporary entries, treat the expired entry as
                        // if the key did not exist
                        if (!isTemporaryEntry(lk))
                        {
                            // Cannot access an archived entry
                            this->innerResult().code(
                                INVOKE_HOST_FUNCTION_ENTRY_ARCHIVED);
                            return false;
                        }
                    }
                    else
                    {
                        sorobanEntryLive = true;
                        ttlEntry = ttlLtxe.current().data.ttl();
                    }
                }
                // If ttlLtxe doesn't exist, this is a new Soroban entry
            }

            if (!isSorobanEntry(lk) || sorobanEntryLive)
            {
                auto ltxe = ltx.loadWithoutRecord(lk);
                if (ltxe)
                {
                    auto leBuf = toCxxBuf(ltxe.current());
                    entrySize = static_cast<uint32_t>(leBuf.data->size());

                    // For entry types that don't have an ttlEntry (i.e.
                    // Accounts), the rust host expects an "empty" CxxBuf such
                    // that the buffer has a non-null pointer that points to an
                    // empty byte vector
                    auto ttlBuf =
                        ttlEntry
                            ? toCxxBuf(*ttlEntry)
                            : CxxBuf{std::make_unique<std::vector<uint8_t>>()};

                    ledgerEntryCxxBufs.emplace_back(std::move(leBuf));
                    ttlEntryCxxBufs.emplace_back(std::move(ttlBuf));
                }
                else if (isSorobanEntry(lk))
                {
                    releaseAssertOrThrow(!ttlEntry);
                }
            }

            metrics.noteReadEntry(isCodeKey(lk), keySize, entrySize);
            if (!validateContractLedgerEntry(lk, entrySize, sorobanConfig,
                                             mParentTx))
            {
                this->innerResult().code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }

            if (resources.readBytes < metrics.mLedgerReadByte)
            {
                mParentTx.pushSimpleDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-read resources exceeds amount specified",
                    {makeU64SCVal(metrics.mLedgerReadByte),
                     makeU64SCVal(resources.readBytes)});

                this->innerResult().code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }
        }
        return true;
    };

    if (!addReads(footprint.readOnly))
    {
        // Error code set in addReads
        return false;
    }

    if (!addReads(footprint.readWrite))
    {
        // Error code set in addReads
        return false;
    }

    rust::Vec<CxxBuf> authEntryCxxBufs;
    authEntryCxxBufs.reserve(mInvokeHostFunction.auth.size());
    for (auto const& authEntry : mInvokeHostFunction.auth)
    {
        authEntryCxxBufs.emplace_back(toCxxBuf(authEntry));
    }

    InvokeHostFunctionOutput out{};
    out.success = false;
    try
    {
        auto timeScope = metrics.getExecTimer();
        CxxBuf basePrngSeedBuf{};
        basePrngSeedBuf.data = std::make_unique<std::vector<uint8_t>>();
        basePrngSeedBuf.data->assign(sorobanBasePrngSeed.begin(),
                                     sorobanBasePrngSeed.end());

        out = rust_bridge::invoke_host_function(
            cfg.CURRENT_LEDGER_PROTOCOL_VERSION,
            cfg.ENABLE_SOROBAN_DIAGNOSTIC_EVENTS, resources.instructions,
            toCxxBuf(mInvokeHostFunction.hostFunction), toCxxBuf(resources),
            toCxxBuf(getSourceID()), authEntryCxxBufs,
            getLedgerInfo(ltx, app, sorobanConfig), ledgerEntryCxxBufs,
            ttlEntryCxxBufs, basePrngSeedBuf,
            sorobanConfig.rustBridgeRentFeeConfiguration());
        metrics.mCpuInsn = out.cpu_insns;
        metrics.mMemByte = out.mem_bytes;
        metrics.mInvokeTimeNsecs = out.time_nsecs;
        metrics.mCpuInsnExclVm = out.cpu_insns_excluding_vm_instantiation;
        metrics.mInvokeTimeNsecsExclVm =
            out.time_nsecs_excluding_vm_instantiation;
        if (!out.success)
        {
            maybePopulateDiagnosticEvents(cfg, out, metrics);
        }
    }
    catch (std::exception& e)
    {
        // Host invocations should never throw an exception, so encountering
        // one would be an internal error.
        out.is_internal_error = true;
        CLOG_DEBUG(Tx, "Exception caught while invoking host fn: {}", e.what());
    }

    if (!out.success)
    {
        if (out.is_internal_error)
        {
            throw std::runtime_error(
                "Got internal error during Soroban host invocation.");
        }
        if (resources.instructions < out.cpu_insns)
        {
            mParentTx.pushSimpleDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation instructions exceeds amount specified",
                {makeU64SCVal(out.cpu_insns),
                 makeU64SCVal(resources.instructions)});
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else if (sorobanConfig.txMemoryLimit() < out.mem_bytes)
        {
            mParentTx.pushSimpleDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "operation memory usage exceeds network config limit",
                {makeU64SCVal(out.mem_bytes),
                 makeU64SCVal(sorobanConfig.txMemoryLimit())});
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        }
        else
        {
            innerResult().code(INVOKE_HOST_FUNCTION_TRAPPED);
        }
        return false;
    }

    // Create or update every entry returned.
    UnorderedSet<LedgerKey> createdAndModifiedKeys;
    UnorderedSet<LedgerKey> createdKeys;
    for (auto const& buf : out.modified_ledger_entries)
    {
        LedgerEntry le;
        xdr::xdr_from_opaque(buf.data, le);
        if (!validateContractLedgerEntry(LedgerEntryKey(le), buf.data.size(),
                                         sorobanConfig, mParentTx))
        {
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }

        auto lk = LedgerEntryKey(le);
        createdAndModifiedKeys.insert(lk);

        uint32_t keySize = static_cast<uint32_t>(xdr::xdr_size(lk));
        uint32_t entrySize = static_cast<uint32_t>(buf.data.size());

        // ttlEntry write fees come out of refundableFee, already
        // accounted for by the host
        if (lk.type() != TTL)
        {
            metrics.noteWriteEntry(isCodeKey(lk), keySize, entrySize);
            if (resources.writeBytes < metrics.mLedgerWriteByte)
            {
                mParentTx.pushSimpleDiagnosticError(
                    SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                    "operation byte-write resources exceeds amount specified",
                    {makeU64SCVal(metrics.mLedgerWriteByte),
                     makeU64SCVal(resources.writeBytes)});
                innerResult().code(
                    INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
                return false;
            }
        }

        auto ltxe = ltx.load(lk);
        if (ltxe)
        {
            ltxe.current() = le;
        }
        else
        {
            ltx.create(le);
            createdKeys.insert(lk);
        }
    }

    // Check that each newly created ContractCode or ContractData entry also
    // creates an ttlEntry
    for (auto const& key : createdKeys)
    {
        if (isSorobanEntry(key))
        {
            auto ttlKey = getTTLKey(key);
            releaseAssertOrThrow(createdKeys.find(ttlKey) != createdKeys.end());
        }
        else
        {
            releaseAssertOrThrow(key.type() == TTL);
        }
    }

    // Erase every entry not returned.
    // NB: The entries that haven't been touched are passed through
    // from host, so this should never result in removing an entry
    // that hasn't been removed by host explicitly.
    for (auto const& lk : footprint.readWrite)
    {
        if (createdAndModifiedKeys.find(lk) == createdAndModifiedKeys.end())
        {
            auto ltxe = ltx.load(lk);
            if (ltxe)
            {
                releaseAssertOrThrow(isSorobanEntry(lk));
                ltx.erase(lk);

                // Also delete associated ttlEntry
                auto ttlLK = getTTLKey(lk);
                auto ttlLtxe = ltx.load(ttlLK);
                releaseAssertOrThrow(ttlLtxe);
                ltx.erase(ttlLK);
            }
        }
    }

    // Append events to the enclosing TransactionFrame, where
    // they'll be picked up and transferred to the TxMeta.
    InvokeHostFunctionSuccessPreImage success{};
    success.events.reserve(out.contract_events.size());
    for (auto const& buf : out.contract_events)
    {
        metrics.mEmitEvent++;
        uint32_t eventSize = static_cast<uint32_t>(buf.data.size());
        metrics.mEmitEventByte += eventSize;
        metrics.mMaxEmitEventByte =
            std::max(metrics.mMaxEmitEventByte, eventSize);
        if (sorobanConfig.txMaxContractEventsSizeBytes() <
            metrics.mEmitEventByte)
        {
            mParentTx.pushSimpleDiagnosticError(
                SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
                "total events size exceeds network config maximum",
                {makeU64SCVal(metrics.mEmitEventByte),
                 makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
            innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
            return false;
        }
        ContractEvent evt;
        xdr::xdr_from_opaque(buf.data, evt);
        success.events.emplace_back(evt);
    }

    maybePopulateDiagnosticEvents(cfg, out, metrics);

    metrics.mEmitEventByte += static_cast<uint32>(out.result_value.data.size());
    if (sorobanConfig.txMaxContractEventsSizeBytes() < metrics.mEmitEventByte)
    {
        mParentTx.pushSimpleDiagnosticError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "return value pushes events size above network config maximum",
            {makeU64SCVal(metrics.mEmitEventByte),
             makeU64SCVal(sorobanConfig.txMaxContractEventsSizeBytes())});
        innerResult().code(INVOKE_HOST_FUNCTION_RESOURCE_LIMIT_EXCEEDED);
        return false;
    }

    if (!mParentTx.consumeRefundableSorobanResources(
            metrics.mEmitEventByte, out.rent_fee,
            ltx.loadHeader().current().ledgerVersion, sorobanConfig, cfg))
    {
        innerResult().code(INVOKE_HOST_FUNCTION_INSUFFICIENT_REFUNDABLE_FEE);
        return false;
    }

    xdr::xdr_from_opaque(out.result_value.data, success.returnValue);
    innerResult().code(INVOKE_HOST_FUNCTION_SUCCESS);
    innerResult().success() = xdrSha256(success);

    mParentTx.pushContractEvents(std::move(success.events));
    mParentTx.setReturnValue(std::move(success.returnValue));
    metrics.mSuccess = true;
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(SorobanNetworkConfig const& config,
                                        uint32_t ledgerVersion)
{
    // check wasm size if uploading contract
    auto const& hostFn = mInvokeHostFunction.hostFunction;
    if (hostFn.type() == HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM &&
        hostFn.wasm().size() > config.maxContractSizeBytes())
    {
        mParentTx.pushSimpleDiagnosticError(
            SCE_BUDGET, SCEC_EXCEEDED_LIMIT,
            "uploaded WASM size exceeds network config maximum contract size",
            {makeU64SCVal(hostFn.wasm().size()),
             makeU64SCVal(config.maxContractSizeBytes())});
        return false;
    }
    if (hostFn.type() == HOST_FUNCTION_TYPE_CREATE_CONTRACT)
    {
        auto const& preimage = hostFn.createContract().contractIDPreimage;
        if (preimage.type() == CONTRACT_ID_PREIMAGE_FROM_ASSET &&
            !isAssetValid(preimage.fromAsset(), ledgerVersion))
        {
            mParentTx.pushSimpleDiagnosticError(
                SCE_VALUE, SCEC_INVALID_INPUT,
                "invalid asset to create contract from");
            return false;
        }
    }
    return true;
}

bool
InvokeHostFunctionOpFrame::doCheckValid(uint32_t ledgerVersion)
{
    throw std::runtime_error(
        "InvokeHostFunctionOpFrame::doCheckValid needs Config");
}

void
InvokeHostFunctionOpFrame::insertLedgerKeysToPrefetch(
    UnorderedSet<LedgerKey>& keys) const
{
}

bool
InvokeHostFunctionOpFrame::isSoroban() const
{
    return true;
}
}
