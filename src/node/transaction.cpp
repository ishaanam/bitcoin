// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <index/txindex.h>
#include <net.h>
#include <net_processing.h>
#include <node/blockstorage.h>
#include <txmempool.h>
#include <validation.h>
#include <validationinterface.h>
#include <node/transaction.h>

#include <future>

//  - Has this nonce already been used with this public key? If so, add this info to the DB
//  - Has this nonce never been seen with this public key before? If so, add this info
//  - return: bool, true if we can now calculate a private key we previously could not
bool NonceDB::Process(const uint256& txid, const int& vin, const std::string& nonce, const std::string& public_key)
{
    // Has this nonce already been seen?
    std::string old_value;
    leveldb::Status db_status = m_db->Get(leveldb::ReadOptions(), nonce, &old_value);

    if (!db_status.ok()) {
        db_status = m_db->Put(leveldb::WriteOptions(), nonce, strprintf("%s:%i_%s", txid.GetHex(), vin, public_key));
        if (!db_status.ok()) throw std::runtime_error("Error: unable to write to the nonce DB");
        return false;
    } else {
        std::vector<NonceInfo> nonce_info_vector =  ParseNonceValueList(old_value);
        int reuse_counter = 0;
        for (const NonceInfo& nonce_info : nonce_info_vector) {
            if (nonce_info.pub_key == public_key && !(txid.GetHex() == nonce_info.txid && nonce_info.vin == std::to_string(vin))) {
                reuse_counter += 1;
            }
        }
        if (reuse_counter == 1) {
            leveldb::Status db_status = m_db->Put(leveldb::WriteOptions(), nonce, strprintf("%s*%s:%i_%s", old_value, txid.GetHex(), vin, public_key));
            if (!db_status.ok()) throw std::runtime_error("Error: unable to write to the nonce DB");
            return true;
        } else {
            // This means the existing databse entry is the same as what we have now, or
            // we can already calculate this, return false
            return false;
        }
    }
}

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

int HexToInt(const std::string hex_string)
{
	int x;
	std::stringstream ss;
	ss << std::hex << hex_string;
	ss >> x;
	return x;
}

std::stringstream GetHexScript(const NodeContext& node, const CTxIn& tx_in)
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
            return ss;
        }
		case TxoutType::PUBKEYHASH: {
			std::stringstream ss{HexStr(tx_in.scriptSig)};
            return ss;
        } default : {
			std::stringstream ss{""};
            return ss;
        }
    }
}

std::vector<std::string> GetKeyAndNonce(const NodeContext& node, const CTxIn& tx_in)
{
	std::vector<std::string> ret;

    std::stringstream ss = GetHexScript(node, tx_in);
    if (ss.eof()) return ret;

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

    ret.push_back(r_value);

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
    std::string public_key;
    std::getline(ss, public_key);

    const std::string pubkey_prefix =  public_key.substr(0, 2);

    if (pubkey_prefix == "04" || pubkey_prefix == "02" || pubkey_prefix == "03") ret.push_back(public_key);
	return ret;
}

int RescanManager::RunScan(const uint256& start_block, int start_height, const NonceRescanReserver& reserver, const NodeContext& node)
{
    AssertLockHeld(m_rescan_mutex);
    assert(reserver.isReserved());

    uint256 block_hash = start_block;
    m_abort_rescan = false;

    // The tip hash being updated here is not handled because this is more
    // about gathering historical data rather than 
    const std::optional<int> tip_height = node.chain->getHeight();
    uint256 tip_hash;
    if (tip_height) {
        tip_hash = node.chain->getBlockHash(*tip_height);
    } else {
        // This is very bad
        throw std::runtime_error("The blockchain has no blocks");
    }

    int block_height = start_height;
    LogPrintf("FINDNONCEREUSE: Starting scan at height %i\n", start_height);

    while (!m_abort_rescan && !node.chain->shutdownRequested()) {
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
                return block_height - 1;
            }
            for (const auto& tx : block.vtx) {
                if (tx->IsCoinBase()) continue;
                for (unsigned int i = 0; i < tx->vin.size(); ++i) {
                    const CTxIn& tx_in = tx->vin.at(i);
                    std::vector<std::string> key_and_nonce = GetKeyAndNonce(node, tx_in);
                    if (key_and_nonce.size() != 2) continue;
                    if (m_nonce_db->Process(tx->GetHash(), i, key_and_nonce.at(0), key_and_nonce.at(1)))
                    {
                        LogPrintf("FINDNONCEREUSE: found a reused nonce %s at block %s at height %i in tx %s:%i\n", key_and_nonce.at(0), block_hash.GetHex(), block_height, tx->GetHash().GetHex(), i);
                    }
                }
            }
            if (block_height % 1000 == 0) {
                LogPrintf("FINDNONCEREUSE: Scan reached height %i\n", block_height);
            }
        } else {
            // could not scan block
            LogPrintf("FINDNONCEREUSE: Scan aborted due to being unable to scan a block: %s at height: %i\n", block_hash.GetHex(), block_height);
            return block_height - 1;
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
    if (block_height && m_abort_rescan) {
        LogPrintf("FINDNONCEREUSE: Scan aborted at block %s at height: %i\n", block_hash.GetHex(), block_height);
    } else if (block_height && node.chain->shutdownRequested()) {
        LogPrintf("FINDNONCEREUSE: Scan aborted at block %s at height: %i\n", block_hash.GetHex(), block_height);
    } else {
        LogPrintf("FINDNONCEREUSE: Scan completed successfully\n");
    }

    m_rescanning.exchange(false);
    m_abort_rescan.exchange(false);
    return block_height;
}
} // namespace node
