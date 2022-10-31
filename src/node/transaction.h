// Copyright (c) 2017-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_TRANSACTION_H
#define BITCOIN_NODE_TRANSACTION_H

#include <atomic>

#include <dbwrapper.h>
#include <sync.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <primitives/transaction.h>
#include <util/error.h>

class CBlockIndex;
class CTxMemPool;
namespace Consensus {
struct Params;
}

namespace node {
    class RescanManager;
}

struct LevelDBWrapper
{
protected:
	leveldb::DB* m_db;

public:
	LevelDBWrapper(const std::string& path)
	{
		leveldb::Options options;
		options.create_if_missing = true;

		leveldb::Status db_status = leveldb::DB::Open(options, path, &m_db);

		if (!db_status.ok()) {
            throw std::runtime_error(strprintf("Unable to open database in: %s \nFatal error: %s", path, db_status.ToString()));
		}
	}

    LevelDBWrapper(LevelDBWrapper&& other) = delete;
    LevelDBWrapper(const LevelDBWrapper&) = delete;

	~LevelDBWrapper()
	{
		delete m_db;
		m_db = nullptr;
	}
};

// New databse format: 
// DB #1: [{nonce : [{txid: vin_pubkey}]}..]
// DB #2: [{height: [nonce..]}..]

class NonceDB : public LevelDBWrapper
{
private:
    struct NonceInfo
    {
        std::string txid {""};
        std::string vin {""};
        std::string pub_key {""};

        void reset() {
            txid = "";
            vin = "";
            pub_key = "";
        }
    };
    // Example: txid:vin_pubkey*txid:vin_pubkey...
    std::vector<NonceInfo> ParseNonceValueList(const std::string& list_string) 
    {
        std::vector<NonceInfo> nonce_info_vector;
        std::stringstream ss{list_string};

        char c;
        NonceInfo tmp_nonce_info;
        std::string* next_string = &tmp_nonce_info.txid;

        while (ss>>c) {
            switch (c) {
                case ('*') : {
                    nonce_info_vector.push_back(tmp_nonce_info);
                    tmp_nonce_info.reset();
                    next_string = &tmp_nonce_info.txid;
                    break;
                } case (':') : {
                    next_string = &tmp_nonce_info.vin;
                    break;
                } case ('_') : {
                    next_string = &tmp_nonce_info.pub_key;
                    break;
                } default : {
                    *next_string += c;
                    break;
                }
            }
        }

        nonce_info_vector.push_back(tmp_nonce_info);

        return nonce_info_vector;
    }
public:
    NonceDB(const std::string& path) 
        : LevelDBWrapper(path)
    {}

    bool Process(const uint256& txid, const int& vin, const std::string& nonce, const std::string& public_key);
};

namespace node {

class NonceRescanReserver;

class RescanManager
{
private:
    std::atomic<bool> m_rescanning{false};
    std::atomic<bool> m_abort_rescan{false};
    friend class NonceRescanReserver;

    std::unique_ptr<NonceDB> m_nonce_db GUARDED_BY(m_rescan_mutex);

public:
    Mutex m_rescan_mutex;

    bool IsRescanning() {
        return m_rescanning;
    }

    RescanManager(const std::string& nonce_db_path)
    {
        LOCK(m_rescan_mutex);
        m_nonce_db = std::make_unique<NonceDB>(nonce_db_path);
    }

    // Returns the height of the last successfully scanned block
    int RunScan(const uint256& start_block, int start_height, const NonceRescanReserver& reserver, const NodeContext& node) EXCLUSIVE_LOCKS_REQUIRED(m_rescan_mutex);

    bool IsScanning() const { return m_rescanning; }
    bool IsAbortingRescan() const { return m_abort_rescan; }

    void AbortRescan() {
        m_abort_rescan.exchange(true);
    }
};

// Modeled after WalletRescanReserver
class NonceRescanReserver
{
private:
    RescanManager& m_rescan_manager;
    bool m_could_reserve;
public:
    explicit NonceRescanReserver(RescanManager& rescan_manager) : m_rescan_manager(rescan_manager), m_could_reserve(false) {}

    bool reserve()
    {
        assert(!m_could_reserve);
        m_could_reserve = true;
        return !m_rescan_manager.m_rescanning.exchange(true);
    }

    bool isReserved() const
    {
        return (m_could_reserve && m_rescan_manager.m_rescanning);
    }

    ~NonceRescanReserver()
    {
        if (m_could_reserve) {
            m_rescan_manager.m_rescanning = false;
        }
    }
};

struct NodeContext;

/** Maximum fee rate for sendrawtransaction and testmempoolaccept RPC calls.
 * Also used by the GUI when broadcasting a completed PSBT.
 * By default, a transaction with a fee rate higher than this will be rejected
 * by these RPCs and the GUI. This can be overridden with the maxfeerate argument.
 */
static const CFeeRate DEFAULT_MAX_RAW_TX_FEE_RATE{COIN / 10};

/**
 * Submit a transaction to the mempool and (optionally) relay it to all P2P peers.
 *
 * Mempool submission can be synchronous (will await mempool entry notification
 * over the CValidationInterface) or asynchronous (will submit and not wait for
 * notification), depending on the value of wait_callback. wait_callback MUST
 * NOT be set while cs_main, cs_mempool or cs_wallet are held to avoid
 * deadlock.
 *
 * @param[in]  node reference to node context
 * @param[in]  tx the transaction to broadcast
 * @param[out] err_string reference to std::string to fill with error string if available
 * @param[in]  max_tx_fee reject txs with fees higher than this (if 0, accept any fee)
 * @param[in]  relay flag if both mempool insertion and p2p relay are requested
 * @param[in]  wait_callback wait until callbacks have been processed to avoid stale result due to a sequentially RPC.
 * return error
 */
[[nodiscard]] TransactionError BroadcastTransaction(NodeContext& node, CTransactionRef tx, std::string& err_string, const CAmount& max_tx_fee, bool relay, bool wait_callback);

/**
 * Return transaction with a given hash.
 * If mempool is provided and block_index is not provided, check it first for the tx.
 * If -txindex is available, check it next for the tx.
 * Finally, if block_index is provided, check for tx by reading entire block from disk.
 *
 * @param[in]  block_index     The block to read from disk, or nullptr
 * @param[in]  mempool         If provided, check mempool for tx
 * @param[in]  hash            The txid
 * @param[in]  consensusParams The params
 * @param[out] hashBlock       The block hash, if the tx was found via -txindex or block_index
 * @returns                    The tx if found, otherwise nullptr
 */
CTransactionRef GetTransaction(const CBlockIndex* const block_index, const CTxMemPool* const mempool, const uint256& hash, const Consensus::Params& consensusParams, uint256& hashBlock);
} // namespace node

#endif // BITCOIN_NODE_TRANSACTION_H
