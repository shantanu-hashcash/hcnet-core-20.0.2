#include "xdr/Hcnet-ledger-entries.h"
#include "xdr/Hcnet-ledger.h"
#include <utility>

namespace hcnet
{

std::pair<TransactionEnvelope, LedgerKey>
getUploadTx(PublicKey const& publicKey, SequenceNumber seqNum);

std::tuple<TransactionEnvelope, LedgerKey, Hash>
getCreateTx(PublicKey const& publicKey, LedgerKey const& contractCodeLedgerKey,
            std::string const& networkPassphrase, SequenceNumber seqNum);

std::pair<TransactionEnvelope, ConfigUpgradeSetKey>
getInvokeTx(PublicKey const& publicKey, LedgerKey const& contractCodeLedgerKey,
            LedgerKey const& contractSourceRefLedgerKey, Hash const& contractID,
            ConfigUpgradeSet const& upgradeSet, SequenceNumber seqNum);

}