#pragma once

// Copyright 2023 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionFrameBase.h"
#include "transactions/TransactionUtils.h"

namespace hcnet
{

namespace txtest
{

SCAddress makeContractAddress(Hash const& hash);
SCAddress makeAccountAddress(AccountID const& accountID);
SCVal makeContractAddressSCVal(SCAddress const& address);
SCVal makeI32(int32_t i32);
SCVal makeI128(uint64_t u64);
SCSymbol makeSymbol(std::string const& str);
SCVal makeU64(uint64_t u64);
SCVal makeU32(uint32_t u32);
SCVal makeBytes(SCBytes bytes);

ContractIDPreimage makeContractIDPreimage(TestAccount& source, uint256 salt);
ContractIDPreimage makeContractIDPreimage(Asset const& asset);
HashIDPreimage
makeFullContractIdPreimage(Hash const& networkID,
                           ContractIDPreimage const& contractIDPreimage);

LedgerKey makeContractInstanceKey(SCAddress const& contractAddress);

ContractExecutable makeWasmExecutable(Hash const& wasmHash);
ContractExecutable makeAssetExecutable(Asset const& asset);

SorobanResources
defaultUploadWasmResourcesWithoutFootprint(RustBuf const& wasm);

// Creates a valid transaction for uploading provided Wasm.
// Fills in the valid footprint automatically in case if `uploadResources`
// doesn't contain it.
TransactionFrameBasePtr
makeSorobanWasmUploadTx(Application& app, TestAccount& source,
                        RustBuf const& wasm, SorobanResources& uploadResources,
                        uint32_t inclusionFee);

// Creates a valid transaction for creating a contract.
// Fills in the valid footprint automatically in case if `createResources`
// doesn't contain it.
TransactionFrameBasePtr makeSorobanCreateContractTx(
    Application& app, TestAccount& source, ContractIDPreimage const& idPreimage,
    ContractExecutable const& executable, SorobanResources& createResources,
    uint32_t inclusionFee);

// Builder-style configuration necessary for building a Soroban
// transaction invocation.
// The specs can be built using method chaining
// (`spec.setInstructions(...).setReadBytes(...)`).
// Note, that the spec itself is immutable, so that it's not possible
// to mess up the test setup by accidentally modifying the spec
// that is also used by another part of the test.
class SorobanInvocationSpec
{
  private:
    SorobanResources mResources;
    // Set reasonable defaults for the fees so that tests that don't
    // care about fees could completely omit them.
    uint32_t mNonRefundableResourceFee = DEFAULT_TEST_RESOURCE_FEE;
    uint32_t mRefundableResourceFee = DEFAULT_TEST_RESOURCE_FEE;
    uint32_t mInclusionFee = 100;

  public:
    SorobanInvocationSpec() = default;
    SorobanInvocationSpec(SorobanResources const& resources,
                          uint32_t nonRefundableResourceFee,
                          uint32_t refundableResourceFee,
                          uint32_t inclusionFee);

    SorobanResources const& getResources() const;
    uint32_t getFee() const;
    uint32_t getResourceFee() const;
    uint32_t getInclusionFee() const;

    SorobanInvocationSpec setInstructions(int64_t instructions) const;
    SorobanInvocationSpec
    setReadOnlyFootprint(xdr::xvector<LedgerKey> const& keys) const;
    SorobanInvocationSpec
    setReadWriteFootprint(xdr::xvector<LedgerKey> const& keys) const;
    SorobanInvocationSpec
    extendReadOnlyFootprint(xdr::xvector<LedgerKey> const& keys) const;
    SorobanInvocationSpec
    extendReadWriteFootprint(xdr::xvector<LedgerKey> const& keys) const;
    SorobanInvocationSpec setReadBytes(uint32_t readBytes) const;
    SorobanInvocationSpec setWriteBytes(uint32_t writeBytes) const;

    SorobanInvocationSpec setRefundableResourceFee(uint32_t fee) const;
    SorobanInvocationSpec setNonRefundableResourceFee(uint32_t fee) const;
    SorobanInvocationSpec setInclusionFee(uint32_t fee) const;
};

TransactionFrameBasePtr sorobanTransactionFrameFromOps(
    Hash const& networkID, TestAccount& source,
    std::vector<Operation> const& ops, std::vector<SecretKey> const& opKeys,
    SorobanInvocationSpec const& spec,
    std::optional<std::string> memo = std::nullopt,
    std::optional<SequenceNumber> seq = std::nullopt);

class SorobanTest;

// Test wrapper for a deployed contract, owned by the `SorobanTest`.
// Normally this should be created with `SorobanTest::deployWasmContract` or
// `SorobanTest::deployAssetContract` methods.
class TestContract
{
  private:
    SorobanTest& mTest;
    xdr::xvector<LedgerKey> mContractKeys;
    SCAddress mAddress;

  public:
    // Wrapper for a single invocation of a contract.
    // The wrapper can be used to modify the invocations in generic
    // fashion (e.g. compute the exact fee or authorize top invocation),
    // and to either build/invoke the respective transaction.
    class Invocation
    {
      private:
        SorobanTest& mTest;
        SorobanInvocationSpec mSpec;
        Operation mOp;

        std::optional<InvokeHostFunctionResultCode> mResultCode;
        std::optional<TransactionMetaFrame> mTxMeta;

      public:
        Invocation(TestContract const& contract,
                   std::string const& functionName,
                   std::vector<SCVal> const& args,
                   SorobanInvocationSpec const& spec,
                   bool addContractKeys = true);

        Invocation& withAuthorizedTopCall();
        Invocation& withExactNonRefundableResourceFee();

        TransactionFrameBasePtr createTx(TestAccount* source = nullptr);
        bool invoke(TestAccount* source = nullptr);

        SCVal getReturnValue() const;
        TransactionMetaFrame const& getTxMeta() const;
        InvokeHostFunctionResultCode getResultCode() const;
    };

    TestContract(SorobanTest& test, SCAddress const& address,
                 xdr::xvector<LedgerKey> const& contractKeys);

    xdr::xvector<LedgerKey> const& getKeys() const;
    SCAddress const& getAddress() const;
    SorobanTest& getTest() const;
    LedgerKey getDataKey(SCVal const& key, ContractDataDurability durability);

    Invocation prepareInvocation(std::string const& functionName,
                                 std::vector<SCVal> const& args,
                                 SorobanInvocationSpec const& spec,
                                 bool addContractKeys = true) const;
};

// Helper for building Soroban tests.
// Encapsulates the necessary state and helpers for deploying the
// contracts and invoking the common Soroban transactions.
class SorobanTest
{
  private:
    VirtualClock mClock;
    Application::pointer mApp;
    TestAccount mRoot;
    TestAccount mDummyAccount;
    std::vector<std::unique_ptr<TestContract>> mContracts;

    static int64_t computeFeePerIncrement(int64_t resourceVal, int64_t feeRate,
                                          int64_t increment);

    void invokeArchivalOp(TransactionFrameBasePtr tx,
                          int64_t expectedRefundableFeeCharged);

    Hash uploadWasm(RustBuf const& wasm, SorobanResources& uploadResources);
    SCAddress createContract(ContractIDPreimage const& idPreimage,
                             ContractExecutable const& executable,
                             SorobanResources& createResources);

    int64_t getRentFeeForExtension(xdr::xvector<LedgerKey> const& keys,
                                   uint32_t newLifetime);

  public:
    SorobanTest(
        Config cfg = getTestConfig(), bool useTestLimits = true,
        std::function<void(SorobanNetworkConfig&)> cfgModifyFn =
            [](SorobanNetworkConfig&) {});

    Application& getApp() const;

    TestContract& deployWasmContract(
        RustBuf const& wasm,
        std::optional<SorobanResources> uploadResources = std::nullopt,
        std::optional<SorobanResources> createResources = std::nullopt);
    TestContract& deployAssetContract(Asset const& asset);

    TestAccount& getRoot();
    TestAccount& getDummyAccount();
    SorobanNetworkConfig const& getNetworkCfg();
    uint32_t getLedgerSeq() const;
    uint32_t getLedgerVersion() const;

    TransactionFrameBasePtr createExtendOpTx(SorobanResources const& resources,
                                             uint32_t extendTo, uint32_t fee,
                                             uint32_t refundableFee,
                                             TestAccount* source = nullptr);
    TransactionFrameBasePtr createRestoreTx(SorobanResources const& resources,
                                            uint32_t fee,
                                            uint32_t refundableFee,
                                            TestAccount* source = nullptr);

    bool isTxValid(TransactionFrameBasePtr tx);

    bool invokeTx(TransactionFrameBasePtr tx,
                  TransactionMetaFrame* txMeta = nullptr);

    uint32_t getTTL(LedgerKey const& k);
    bool isEntryLive(LedgerKey const& k, uint32_t ledgerSeq);

    void invokeRestoreOp(xdr::xvector<LedgerKey> const& readWrite,
                         int64_t expectedRefundableFeeCharged);
    void invokeExtendOp(
        xdr::xvector<LedgerKey> const& readOnly, uint32_t extendTo,
        std::optional<int64_t> expectedRefundableFeeCharged = std::nullopt);
};

class AssetContractTestClient
{
  private:
    TestContract& mContract;
    Asset mAsset;
    Application& mApp;

    LedgerKey makeBalanceKey(AccountID const& acc);
    LedgerKey makeBalanceKey(SCAddress const& addr);
    LedgerKey makeIssuerKey(Asset const& mAsset);
    LedgerKey makeContractDataBalanceKey(SCAddress const& addr);
    int64_t getBalance(SCAddress const& addr);

    SorobanInvocationSpec defaultSpec() const;

  public:
    AssetContractTestClient(SorobanTest& test, Asset const& asset);

    bool transfer(TestAccount& from, SCAddress const& toAddr, int64_t amount);
    bool mint(TestAccount& admin, SCAddress const& toAddr, int64_t amount);
    bool burn(TestAccount& from, int64_t amount);
    bool clawback(TestAccount& admin, SCAddress const& fromAddr,
                  int64_t amount);
};

class ContractStorageTestClient
{
  private:
    TestContract& mContract;

  public:
    ContractStorageTestClient(SorobanTest& test);

    TestContract& getContract() const;

    SorobanInvocationSpec defaultSpecWithoutFootprint() const;

    SorobanInvocationSpec readKeySpec(std::string const& key,
                                      ContractDataDurability durability) const;

    SorobanInvocationSpec writeKeySpec(std::string const& key,
                                       ContractDataDurability durability) const;

    uint32_t getTTL(std::string const& key, ContractDataDurability durability);
    bool isEntryLive(std::string const& key, ContractDataDurability durability,
                     uint32_t ledgerSeq);

    InvokeHostFunctionResultCode
    put(std::string const& key, ContractDataDurability durability, uint64_t val,
        std::optional<SorobanInvocationSpec> spec = std::nullopt);

    InvokeHostFunctionResultCode
    get(std::string const& key, ContractDataDurability durability,
        std::optional<uint64_t> expectValue,
        std::optional<SorobanInvocationSpec> spec = std::nullopt);

    InvokeHostFunctionResultCode
    has(std::string const& key, ContractDataDurability durability,
        std::optional<bool> expectHas,
        std::optional<SorobanInvocationSpec> spec = std::nullopt);

    InvokeHostFunctionResultCode
    del(std::string const& key, ContractDataDurability durability,
        std::optional<SorobanInvocationSpec> spec = std::nullopt);

    InvokeHostFunctionResultCode
    extend(std::string const& key, ContractDataDurability durability,
           uint32_t threshold, uint32_t extendTo,
           std::optional<SorobanInvocationSpec> spec = std::nullopt);

    InvokeHostFunctionResultCode resizeStorageAndExtend(
        std::string const& key, uint32_t numKiloBytes, uint32_t thresh,
        uint32_t extendTo,
        std::optional<SorobanInvocationSpec> spec = std::nullopt);
};
}
}
