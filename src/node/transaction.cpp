// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <index/txindex.h>
#include <net.h>
#include <net_processing.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>
#include <node/transaction.h>

#include <future>

namespace node {
static TransactionError HandleATMPError(const TxValidationState& state, std::string& err_string_out)
{
    err_string_out = state.ToString();
    if (state.IsInvalid()) {
        if (state.GetResult() == TxValidationResult::TX_MISSING_INPUTS) {
            return TransactionError::MISSING_INPUTS;
        }
        return TransactionError::MEMPOOL_REJECTED;
    } else {
        return TransactionError::MEMPOOL_ERROR;
    }
}

TransactionError BroadcastTransaction(NodeContext& node, const CTransactionRef tx, std::string& err_string, const CAmount& max_tx_fee, bool relay, bool wait_callback)
{
    // BroadcastTransaction can be called by either sendrawtransaction RPC or the wallet.
    // chainman, mempool and peerman are initialized before the RPC server and wallet are started
    // and reset after the RPC sever and wallet are stopped.
    assert(node.chainman);
    assert(node.mempool);
    assert(node.peerman);

    std::promise<void> promise;
    uint256 txid = tx->GetHash();
    uint256 wtxid = tx->GetWitnessHash();
    bool callback_set = false;

    {
        LOCK(cs_main);

        // If the transaction is already confirmed in the chain, don't do anything
        // and return early.
        CCoinsViewCache &view = node.chainman->ActiveChainstate().CoinsTip();
        for (size_t o = 0; o < tx->vout.size(); o++) {
            const Coin& existingCoin = view.AccessCoin(COutPoint(txid, o));
            // IsSpent doesn't mean the coin is spent, it means the output doesn't exist.
            // So if the output does exist, then this transaction exists in the chain.
            if (!existingCoin.IsSpent()) return TransactionError::ALREADY_IN_CHAIN;
        }

        if (auto mempool_tx = node.mempool->get(txid); mempool_tx) {
            // There's already a transaction in the mempool with this txid. Don't
            // try to submit this transaction to the mempool (since it'll be
            // rejected as a TX_CONFLICT), but do attempt to reannounce the mempool
            // transaction if relay=true.
            //
            // The mempool transaction may have the same or different witness (and
            // wtxid) as this transaction. Use the mempool's wtxid for reannouncement.
            wtxid = mempool_tx->GetWitnessHash();
        } else {
            // Transaction is not already in the mempool.
            if (max_tx_fee > 0) {
                // First, call ATMP with test_accept and check the fee. If ATMP
                // fails here, return error immediately.
                const MempoolAcceptResult result = node.chainman->ProcessTransaction(tx, /*test_accept=*/ true);
                if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                    return HandleATMPError(result.m_state, err_string);
                } else if (result.m_base_fees.value() > max_tx_fee) {
                    return TransactionError::MAX_FEE_EXCEEDED;
                }
            }
            // Try to submit the transaction to the mempool.
            const MempoolAcceptResult result = node.chainman->ProcessTransaction(tx, /*test_accept=*/ false);
            if (result.m_result_type != MempoolAcceptResult::ResultType::VALID) {
                return HandleATMPError(result.m_state, err_string);
            }

            // Transaction was accepted to the mempool.

            if (relay) {
                // the mempool tracks locally submitted transactions to make a
                // best-effort of initial broadcast
                node.mempool->AddUnbroadcastTx(txid);
            }

            if (wait_callback) {
                // For transactions broadcast from outside the wallet, make sure
                // that the wallet has been notified of the transaction before
                // continuing.
                //
                // This prevents a race where a user might call sendrawtransaction
                // with a transaction to/from their wallet, immediately call some
                // wallet RPC, and get a stale result because callbacks have not
                // yet been processed.
                CallFunctionInValidationInterfaceQueue([&promise] {
                    promise.set_value();
                });
                callback_set = true;
            }
        }
    } // cs_main

    if (callback_set) {
        // Wait until Validation Interface clients have been notified of the
        // transaction entering the mempool.
        promise.get_future().wait();
    }

    if (relay) {
        node.peerman->RelayTransaction(txid, wtxid);
    }

    return TransactionError::OK;
}

CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxMemPool* const mempool, const uint256& hash, const Consensus::Params& consensusParams, uint256& hashBlock)
{
    if (mempool && !block_index) {
        CTransactionRef ptx = mempool->get(hash);
        if (ptx) return ptx;
    }
    if (g_txindex) {
        CTransactionRef tx;
        uint256 block_hash;
        if (g_txindex->FindTx(hash, block_hash, tx)) {
            if (!block_index || block_index->GetBlockHash() == block_hash) {
                // Don't return the transaction if the provided block hash doesn't match.
                // The case where a transaction appears in multiple blocks (e.g. reorgs or
                // BIP30) is handled by the block lookup below.
                hashBlock = block_hash;
                return tx;
            }
        }
    }
    if (block_index) {
        CBlock block;
        if (ReadBlockFromDisk(block, block_index, consensusParams)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    hashBlock = block_index->GetBlockHash();
                    return tx;
                }
            }
        }
    }
    return nullptr;
}

std::istream& operator>>(std::istream& is, PublicKeyEntry& pk_entry) {
    std::getline(is, pk_entry.txid, ':');

    std::string temp_str;
    std::getline(is, temp_str, ':');
    pk_entry.vin = std::stoi(temp_str);

    std::getline(is, temp_str, ':');
    pk_entry.block_height = std::stoi(temp_str);

    std::getline(is, pk_entry.public_key, ':');

    std::getline(is, temp_str, ':');
    pk_entry.segwit = temp_str == "1";

    return is;
}

std::ostream& operator<<(std::ostream& os, const PublicKeyEntry& pk_entry) {
    os << pk_entry.txid;
    os << ":";
    os << std::to_string(pk_entry.vin);
    os << ":";
    os << pk_entry.public_key;
    os << ":";
    os << std::to_string(pk_entry.block_height);
    os << ":";
    os << (pk_entry.segwit ? "1" : "0");
    os << ":";
    return os;
}

std::ostream& operator<<(std::ostream& os, const NonceEntry& nonce_entry) {
    for (const PublicKeyEntry& pk : nonce_entry.pk_entries) {
        os << pk << "_";
    }
    return os;
}

std::istream& operator>>(std::istream& is, NonceEntry& nonce_entry) {
    auto& pk_entries = nonce_entry.pk_entries;
    char c;
    while (is >> c) {
        if (c != '_') throw std::runtime_error("findnoncereuse: Incorrect data format");
        if (!is.get()) break;
        PublicKeyEntry pk;
        is >> pk;
        pk_entries.push_back(pk);
    }
    return is;
}

bool operator==(const PublicKeyEntry& a, const PublicKeyEntry& b) {
    return a.txid == b.txid &&
           a.vin == b.vin &&
           a.public_key == b.public_key;
}

void NonceScanManager::ProcessPKEntry(const std::string& nonce, const PublicKeyEntry& pk_entry) {
    LOCK(m_rescan_mutex);
    std::string old_value;
    leveldb::Status db_status = m_db->Get(leveldb::ReadOptions(), nonce, &old_value);

    if (!db_status.ok()) {
        WriteEntry(nonce, NonceEntry(pk_entry));
    } else {
        NonceEntry nonce_entry(old_value);

        // only with the same private key
        for (const PublicKeyEntry& other_entry : nonce_entry.pk_entries) {
            // found a duplicate
            if (other_entry == pk_entry) return;
        }
        nonce_entry.pk_entries.push_back(pk_entry);
        WriteEntry(nonce, nonce_entry);
    }
}

void NonceScanManager::WriteEntry(const std::string nonce, const NonceEntry& entry) {
    AssertLockHeld(m_rescan_mutex);
    m_db->Put(leveldb::WriteOptions(), nonce, entry.get_raw());
}

int HexToInt(const std::string hex_string)
{
    int x;
    std::stringstream ss;
    ss << std::hex << hex_string;
    ss >> x;
    return x;
}

std::stringstream GetHexScript(const NodeContext& node, const CTxIn& tx_in, bool& segwit)
{
	assert(g_txindex);

	CTransactionRef prev_tx;
	uint256 block_hash;

	if (!g_txindex->FindTx(tx_in.prevout.hash, block_hash, prev_tx)) {
        throw std::runtime_error("Previous transaction not found");
	}

	CTxOut previous_output = prev_tx->vout.at(tx_in.prevout.n);

	std::vector<std::vector<uint8_t>> return_values_unused;

	TxoutType type = Solver(previous_output.scriptPubKey, return_values_unused);

	switch (type) {
		case TxoutType::WITNESS_V0_KEYHASH: {
            std::stringstream ss{tx_in.scriptWitness.ToString()};
            segwit = true;
            return ss;
        }
		case TxoutType::PUBKEYHASH: {
			std::stringstream ss{HexStr(tx_in.scriptSig)};
            segwit = false;
            return ss;
        } default : {
			std::stringstream ss{""};
            return ss;
        }
    }
}

bool GetKeyAndNonce(const NodeContext& node, const CTxIn& tx_in, std::optional<std::string>& public_key, std::optional<std::string>& nonce)
{
    bool segwit = false;
    std::stringstream ss = GetHexScript(node, tx_in, segwit);
    if (ss.eof()) return false;

    for (int i = 0; i < 8; i++) {
        ss.get();
    }

    // obtain rvalue length
    std::string raw_r_len = std::string(1, ss.get());
    raw_r_len += std::string(1, ss.get());

    std::string r_value{""};

    int r_len = HexToInt(raw_r_len) * 2;

    for (int i = 0; i < r_len; i++) {
        r_value += ss.get();
    }

    nonce = r_value;

    // Get the marker for the s value
    ss.get();
    ss.get();

    // obtain svalue length
    std::string raw_s_len = std::string(1, ss.get());
    raw_s_len += std::string(1, ss.get());

    std::string s_value{""};

    int s_len = HexToInt(raw_s_len) * 2;

    for (int i = 0; i < s_len; i++) {
        s_value += ss.get();
    }

    for (int i = 0; i < 4; i++) {
        ss.get();
    }

    // Whatever is left should be the public key
    std::string raw_pk;
    std::getline(ss, raw_pk);

    const std::string pubkey_prefix =  raw_pk.substr(0, 2);

    if (pubkey_prefix == "04" || pubkey_prefix == "02" || pubkey_prefix == "03") public_key = raw_pk;
    return segwit;
}

void ReuseScanner::RunScan(const NodeContext& node) {
    uint256 block_hash = node.chain->getBlockHash(*m_current_height);

    int block_height = *m_current_height;
    LogPrintf("FINDNONCEREUSE: Starting scan at height %i\n", block_height);

    while (!node.chain->shutdownRequested()) {
        // Read block data
        CBlock block;
        node.chain->findBlock(block_hash, interfaces::FoundBlock().data(block));

        // Find next block separately from reading data above, because reading
        // is slow and there might be a reorg while it is read.
        bool block_still_active = false;
        bool next_block = false;
        uint256 next_block_hash;
        node.chain->findBlock(block_hash, interfaces::FoundBlock().inActiveChain(block_still_active).nextBlock(interfaces::FoundBlock().inActiveChain(next_block).hash(next_block_hash)));

        if (!block.IsNull()) {
            if (!block_still_active) {
                // Abort scan if current block is no longer active, to prevent
                // marking transactions as coming from the wrong block.
                LogPrintf("FINDNONCEREUSE: Scan aborted due to inactive block hash: %s at height: %i\n", block_hash.GetHex(), block_height);
            }
            for (const auto& tx : block.vtx) {
                if (tx->IsCoinBase()) continue;
                for (unsigned int i = 0; i < tx->vin.size(); ++i) {
                    const CTxIn& tx_in = tx->vin.at(i);
                    std::optional<std::string> public_key;
                    std::optional<std::string> nonce;

                    bool segwit = GetKeyAndNonce(node, tx_in, public_key, nonce);
                    if (!nonce || !public_key) continue;
                    node.rescan_man->ProcessPKEntry(*nonce, PublicKeyEntry(tx->GetHash().GetHex(), i, block_height, *public_key, segwit));
                }
            }
            if (block_height % 1000 == 0) {
                LogPrintf("FINDNONCEREUSE: Scan reached height %i\n", block_height);
            }
        } else {
            // could not scan block
            LogPrintf("FINDNONCEREUSE: Scan aborted due to being unable to scan a block: %s at height: %i\n", block_hash.GetHex(), block_height);
        }
        {
            if (!next_block) {
                // break successfully when rescan has reached the tip, or
                // previous block is no longer on the chain due to a reorg
                break;
            }

            // increment block and verification progress
            block_hash = next_block_hash;
            ++block_height;
        }
    }
    if (block_height) {
        LogPrintf("FINDNONCEREUSE: Scan aborted at block %s at height: %i\n", block_hash.GetHex(), block_height);
    } else if (block_height && node.chain->shutdownRequested()) {
        LogPrintf("FINDNONCEREUSE: Scan aborted at block %s at height: %i\n", block_hash.GetHex(), block_height);
    } else {
        LogPrintf("FINDNONCEREUSE: Scan completed successfully\n");
    }
}

} // namespace node
