// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SCRIPT_SIGN_H
#define BITCOIN_SCRIPT_SIGN_H

#include <attributes.h>
#include <coins.h>
#include <hash.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <script/keyorigin.h>
#include <script/signingprovider.h>
#include <uint256.h>

class CKey;
class CKeyID;
class CScript;
class CTransaction;
class SigningProvider;

struct bilingual_str;
struct CMutableTransaction;

/** Interface for signature creators. */
class BaseSignatureCreator {
public:
    virtual ~BaseSignatureCreator() {}
    virtual const BaseSignatureChecker& Checker() const =0;

    /** Create a singular (non-script) signature. */
    virtual bool CreateSig(const SigningProvider& provider, std::vector<unsigned char>& vchSig, const CKeyID& keyid, const CScript& scriptCode, SigVersion sigversion) const =0;
    virtual bool CreateSchnorrSig(const SigningProvider& provider, std::vector<unsigned char>& sig, const XOnlyPubKey& pubkey, const uint256* leaf_hash, const uint256* merkle_root, SigVersion sigversion) const =0;
};

/** A signature creator for transactions. */
class MutableTransactionSignatureCreator : public BaseSignatureCreator
{
    const CMutableTransaction& m_txto;
    unsigned int nIn;
    int nHashType;
    CAmount amount;
    const MutableTransactionSignatureChecker checker;
    const PrecomputedTransactionData* m_txdata;

public:
    MutableTransactionSignatureCreator(const CMutableTransaction& tx LIFETIMEBOUND, unsigned int input_idx, const CAmount& amount, int hash_type);
    MutableTransactionSignatureCreator(const CMutableTransaction& tx LIFETIMEBOUND, unsigned int input_idx, const CAmount& amount, const PrecomputedTransactionData* txdata, int hash_type);
    const BaseSignatureChecker& Checker() const override { return checker; }
    bool CreateSig(const SigningProvider& provider, std::vector<unsigned char>& vchSig, const CKeyID& keyid, const CScript& scriptCode, SigVersion sigversion) const override;
    bool CreateSchnorrSig(const SigningProvider& provider, std::vector<unsigned char>& sig, const XOnlyPubKey& pubkey, const uint256* leaf_hash, const uint256* merkle_root, SigVersion sigversion) const override;
};

/** A signature checker that accepts all signatures */
extern const BaseSignatureChecker& DUMMY_CHECKER;
/** A signature creator that just produces 71-byte empty signatures. */
extern const BaseSignatureCreator& DUMMY_SIGNATURE_CREATOR;
/** A signature creator that just produces 72-byte empty signatures. */
extern const BaseSignatureCreator& DUMMY_MAXIMUM_SIGNATURE_CREATOR;

typedef std::pair<CPubKey, std::vector<unsigned char>> SigPair;

// This struct contains information from a transaction input and also contains signatures for that input.
// The information contained here can be used to create a signature and is also filled by ProduceSignature
// in order to construct final scriptSigs and scriptWitnesses.
struct SignatureData {
    bool complete = false; ///< Stores whether the scriptSig and scriptWitness are complete
    bool witness = false; ///< Stores whether the input this SigData corresponds to is a witness input
    CScript scriptSig; ///< The scriptSig of an input. Contains complete signatures or the traditional partial signatures format
    CScript redeem_script; ///< The redeemScript (if any) for the input
    CScript witness_script; ///< The witnessScript (if any) for the input. witnessScripts are used in P2WSH outputs.
    CScriptWitness scriptWitness; ///< The scriptWitness of an input. Contains complete signatures or the traditional partial signatures format. scriptWitness is part of a transaction input per BIP 144.
    TaprootSpendData tr_spenddata; ///< Taproot spending data.
    std::optional<TaprootBuilder> tr_builder; ///< Taproot tree used to build tr_spenddata.
    std::map<CKeyID, SigPair> signatures; ///< BIP 174 style partial signatures for the input. May contain all signatures necessary for producing a final scriptSig or scriptWitness.
    std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>> misc_pubkeys;
    std::vector<unsigned char> taproot_key_path_sig; /// Schnorr signature for key path spending
    std::map<std::pair<XOnlyPubKey, uint256>, std::vector<unsigned char>> taproot_script_sigs; ///< (Partial) schnorr signatures, indexed by XOnlyPubKey and leaf_hash.
    std::map<XOnlyPubKey, std::pair<std::set<uint256>, KeyOriginInfo>> taproot_misc_pubkeys; ///< Miscellaneous Taproot pubkeys involved in this input along with their leaf script hashes and key origin data. Also includes the Taproot internal key (may have no leaf script hashes).
    std::map<CKeyID, XOnlyPubKey> tap_pubkeys; ///< Misc Taproot pubkeys involved in this input, by hash. (Equivalent of misc_pubkeys but for Taproot.)
    std::vector<CKeyID> missing_pubkeys; ///< KeyIDs of pubkeys which could not be found
    std::vector<CKeyID> missing_sigs; ///< KeyIDs of pubkeys for signatures which could not be found
    uint160 missing_redeem_script; ///< ScriptID of the missing redeemScript (if any)
    uint256 missing_witness_script; ///< SHA256 of the missing witnessScript (if any)
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> sha256_preimages; ///< Mapping from a SHA256 hash to its preimage provided to solve a Script
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> hash256_preimages; ///< Mapping from a HASH256 hash to its preimage provided to solve a Script
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> ripemd160_preimages; ///< Mapping from a RIPEMD160 hash to its preimage provided to solve a Script
    std::map<std::vector<uint8_t>, std::vector<uint8_t>> hash160_preimages; ///< Mapping from a HASH160 hash to its preimage provided to solve a Script

    SignatureData() {}
    explicit SignatureData(const CScript& script) : scriptSig(script) {}
    void MergeSignatureData(SignatureData sigdata);
};

/** Produce a script signature using a generic signature creator. */
bool ProduceSignature(const SigningProvider& provider, const BaseSignatureCreator& creator, const CScript& scriptPubKey, SignatureData& sigdata);

/**
 * Produce a satisfying script (scriptSig or witness).
 *
 * @param provider   Utility containing the information necessary to solve a script.
 * @param fromPubKey The script to produce a satisfaction for.
 * @param txTo       The spending transaction.
 * @param nIn        The index of the input in `txTo` referring the output being spent.
 * @param amount     The value of the output being spent.
 * @param nHashType  Signature hash type.
 * @param sig_data   Additional data provided to solve a script. Filled with the resulting satisfying
 *                   script and whether the satisfaction is complete.
 *
 * @return           True if the produced script is entirely satisfying `fromPubKey`.
 **/
bool SignSignature(const SigningProvider &provider, const CScript& fromPubKey, CMutableTransaction& txTo,
                   unsigned int nIn, const CAmount& amount, int nHashType, SignatureData& sig_data);
bool SignSignature(const SigningProvider &provider, const CTransaction& txFrom, CMutableTransaction& txTo,
                   unsigned int nIn, int nHashType, SignatureData& sig_data);

/** Extract signature data from a transaction input, and insert it. */
SignatureData DataFromTransaction(const CMutableTransaction& tx, unsigned int nIn, const CTxOut& txout);
void UpdateInput(CTxIn& input, const SignatureData& data);

/** Check whether a scriptPubKey is known to be segwit. */
bool IsSegWitOutput(const SigningProvider& provider, const CScript& script);

/** Sign the CMutableTransaction */
bool SignTransaction(CMutableTransaction& mtx, const SigningProvider* provider, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, bilingual_str>& input_errors);

enum TimeLockType : int {
    NO_TIMELOCKS = 0,
    SEQUENCE_DEPTH = 1,
    SEQUENCE_MTP = 2,
    LOCKTIME_HEIGHT = 3,
    LOCKTIME_MTP = 4
};

struct TimeLock {
    TimeLockType type;
    std::optional<uint32_t> value; // This will only contain a value if type is NO_TIMELOCKS

    TimeLock(TimeLockType type, std::optional<uint32_t> value = std::nullopt) 
      : type{type},
        value{value}
        {}

    bool operator<(const TimeLock& other) const {
        return type < other.type;
    }

    // It is important to note that two TimeLocks are considered equal
    // if they are of the same type, regardless of their values
    bool operator==(const TimeLock& other) const {
        return type == other.type;
    }

};

struct TimeLockManager {
protected:
    std::set<TimeLock> time_locks;
public:
    TimeLockManager() {}

    TimeLockManager(std::set<TimeLock> time_locks)
      : time_locks{time_locks}
      {}

    bool HasSpendingPath() const {
        return !time_locks.empty();
    }

    std::optional<TimeLock> GetType(TimeLockType type) const {
        auto it = std::find(time_locks.begin(), time_locks.end(), TimeLock(type));

        if (it != std::end(time_locks)) {
            return *it;
        }
        return std::nullopt;
    }

    bool HasType(TimeLockType type) const{
        return GetType(type).has_value();
    }

    void Update(TimeLock time_lock) {
        auto it = std::find(time_locks.begin(), time_locks.end(), TimeLock(time_lock.type));
        if (it != std::end(time_locks)) {
            if (it->value >= time_lock.value) return; // should not update as new value is smaller
            time_locks.erase(it);
        }
        time_locks.insert(time_lock);
    }

    void Update(const TimeLockManager& time_lock_manager) {
        for (const TimeLock& time_lock : time_lock_manager.time_locks) {
            Update(time_lock);
        }
    }

    TimeLockManager And (const TimeLockManager& other) const {
        std::vector<TimeLockManager> managers;
        managers.push_back(*this);
        managers.push_back(other);
        return Thresh(managers, 2);
    }

    TimeLockManager Or(const TimeLockManager& other) const {
        std::vector<TimeLockManager> managers;
        managers.push_back(*this);
        managers.push_back(other);
        return Thresh(managers, 1);
    }

    static TimeLockManager Thresh(const std::vector<TimeLockManager> managers, int m) {
        TimeLockManager time_lock_manager;
        TimeLockManager temp_manager;

        int num_no_timelocks{0};
        int num_sequence_depth{0};
        int num_sequence_mtp{0};
        int num_locktime_height{0};
        int num_locktime_mtp{0};

        for (const TimeLockManager& manager : managers) {
            for (const TimeLock& time_lock : manager.time_locks) {
                temp_manager.Update(time_lock);
                switch (time_lock.type) {
                    case NO_TIMELOCKS :
                        num_no_timelocks += 1;
                        break;
                    case SEQUENCE_DEPTH :
                        num_sequence_depth += 1;
                        break;
                    case SEQUENCE_MTP :
                        num_sequence_mtp += 1;
                        break;
                    case LOCKTIME_HEIGHT : 
                        num_locktime_height += 1;
                        break;
                    case LOCKTIME_MTP : 
                        num_locktime_mtp += 1;
                        break;
                    default:
                        assert(false);
                }
            }
        }

        for (const TimeLockManager& manager : managers) {
            if (manager.HasType(NO_TIMELOCKS)) {
                if (num_sequence_depth > 0 && !manager.HasType(SEQUENCE_DEPTH)) {
                    num_sequence_depth++;
                }
                if (num_sequence_mtp > 0 && !manager.HasType(SEQUENCE_MTP)) {
                    num_sequence_mtp++;
                }
                if (num_locktime_height > 0 && !manager.HasType(LOCKTIME_HEIGHT)) {
                    num_locktime_height++;
                }
                if (num_locktime_mtp > 0 && !manager.HasType(LOCKTIME_MTP)) {
                    num_locktime_mtp++;
                }
            }
        }

        if (num_no_timelocks >= m) {
            time_lock_manager.Update(TimeLock(NO_TIMELOCKS));
        }
        if (num_sequence_depth >= m) {
            time_lock_manager.Update(temp_manager.GetType(SEQUENCE_DEPTH).value());
        }
        if (num_sequence_mtp >= m) {
            time_lock_manager.Update(temp_manager.GetType(SEQUENCE_MTP).value());
        }
        if (num_locktime_height >= m) {
            time_lock_manager.Update(temp_manager.GetType(LOCKTIME_HEIGHT).value());
        }
        if (num_locktime_mtp >= m) {
            time_lock_manager.Update(temp_manager.GetType(LOCKTIME_MTP).value());
        }

        return time_lock_manager;
    }
};

#endif // BITCOIN_SCRIPT_SIGN_H
