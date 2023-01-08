// Copyright (c) 2017-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_TRANSACTION_H
#define BITCOIN_NODE_TRANSACTION_H

#include <optional>
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

class NonceScanManager;

/* Next Stuff: 
- finish this on tuesday
- have part 0 written by wednesday
- figure out << >> nonsense
- write the RPC and stuff
- figure out how to turn off as many background processes as possible
*/

struct LevelDBWrapper
{
private:
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

class ReuseScanner
{
private:
    friend class NonceScanManager;
    std::optional<int> m_current_height; // in thousands
public:
    NonceScanManager* manager;
    ReuseScanner()
    {}
    void RunScan(const NodeContext& node);
    ~ReuseScanner() {}
};

struct PublicKeyEntry
{
    std::string txid;
    int vin;
    int block_height;
    std::string public_key;
    bool segwit;

    std::string get_raw() const {
        std::stringstream ss{""};
        ss << txid;
        ss << ":";
        ss << std::to_string(vin);
        ss << ":";
        ss << public_key;
        ss << ":";
        ss << std::to_string(block_height);
        ss << ":";
        ss << (segwit ? "1" : "0");
        ss << ":";
        
        return ss.str();
    }

    PublicKeyEntry(const std::string& txid, const int& vin, const int& block_height, const std::string& public_key, bool segwit)
      : txid(txid),
        vin(vin),
        block_height(block_height),
        public_key(public_key),
        segwit(segwit)
        {}
    
    PublicKeyEntry() {};
};

std::istream& operator>>(std::istream& is, PublicKeyEntry& pk_entry);

std::ostream& operator<<(std::ostream& os, const PublicKeyEntry& pk_entry);

bool operator==(const PublicKeyEntry& a, const PublicKeyEntry& b);

struct NonceEntry
{
    std::vector<PublicKeyEntry> pk_entries;

    NonceEntry(const PublicKeyEntry& pk_entry) {
        pk_entries.push_back(pk_entry);
    }

    NonceEntry(const std::string& raw_entry) {
        char c;
        std::stringstream ss{raw_entry};
        while (ss >> c) {
            if (c != '_') throw std::runtime_error("findnoncereuse: Incorrect data format");
            if (!ss.get()) break;
            PublicKeyEntry pk;
            ss >> pk;
            pk_entries.push_back(pk);
        }
    }

    NonceEntry() {}

    std::string get_raw() const {
        std::string ret;
        for (const PublicKeyEntry& pk : pk_entries) {
            ret += pk.get_raw() + "_";
        }
        return ret;
    }
};

std::ostream& operator<<(std::ostream& os, const NonceEntry& nonce_entry);

std::istream& operator>>(std::istream& is, NonceEntry& nonce_entry);

class NonceScanManager
{
public:
    Mutex m_rescan_mutex;

private:
    leveldb::DB* m_db GUARDED_BY(m_rescan_mutex);
    std::vector<ReuseScanner*> m_rescanners;
    // changes when an m_rescanner's height is set to the current next_height
    int next_height = 0; // in thousands
    const int last_height = 770;

public:
    explicit NonceScanManager(const int& start_height, std::string db_path, size_t cache_size) : next_height(start_height) {
        // nonce_db = std::make_shared<CDBWrapper>(db_path, cache_size);
        leveldb::Options options;
        options.create_if_missing = true;

        leveldb::Status db_status = leveldb::DB::Open(options, db_path, &m_db);

        if (!db_status.ok()) {
            throw std::runtime_error(strprintf("Unable to open database in: %s \nFatal error: %s", db_path, db_status.ToString()));
        }
    }

    void ProcessPKEntry(const std::string& nonce, const PublicKeyEntry& pk_entry);
    void WriteEntry(const std::string nonce, const NonceEntry& entry);

    void reserve(ReuseScanner& scanner)
    {
        LOCK(m_rescan_mutex);
        scanner.m_current_height = next_height * 1000;
        next_height += 1;
        m_rescanners.push_back(&scanner);
    }

    bool get_next_batch(ReuseScanner& scanner) {
        LOCK(m_rescan_mutex);
        if (next_height < last_height) {
            scanner.m_current_height = next_height * 1000;
            next_height += 1;
            return true;
        } else {
            for (std::vector<ReuseScanner*>::iterator it = m_rescanners.begin(); it != m_rescanners.end();) {
                if (*it == &scanner) m_rescanners.erase(it);
            }
            return false;
        }
    }

    ~NonceScanManager()
    {
        LOCK(m_rescan_mutex);
        // LogPrintf the heights that each of the scanners left off on
        for (long unsigned int i = 0; i < m_rescanners.size(); i++) {
            const ReuseScanner& scanner = *m_rescanners.at(i);
            LogPrintf("FINDNONCEREUSE: Scanner %i left off at batch %i", i, scanner.m_current_height.value() / 1000);
        }
		delete m_db;
		m_db = nullptr;
    }
};

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
