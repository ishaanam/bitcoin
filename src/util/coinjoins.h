// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_COINJOINS_H
#define BITCOIN_UTIL_COINJOINS_H

#include <primitives/transaction.h>


class Tx0s {
    std::set<uint256> tx0_set;

public:
    Tx0s() {
        tx0_set = {};
    }

    void Update(const uint256& txid) {
        if (tx0_set.insert(txid).second) {
            // To-Do: write the tx0 to the csv file
        }
    }

    int Size() {
        return tx0_set.size();
    }
};

class WhirlpoolTransactions {
    Tx0s tx0s;
    std::set<uint256> cj_transactions;

    bool isWhirlpool(const CTransactionRef& tx);

public:
    WhirlpoolTransactions() {
        cj_transactions = {};
        tx0s = {};

        // Add all Genesis Whirlpool transactions
        cj_transactions.insert(uint256S("c6c27bef217583cca5f89de86e0cd7d8b546844f800da91d91a74039c3b40fba"));
        cj_transactions.insert(uint256S("94b0da89431d8bd74f1134d8152ed1c7c4f83375e63bc79f19cf293800a83f52"));
        cj_transactions.insert(uint256S("b42df707a3d876b24a22b0199e18dc39aba2eafa6dbeaaf9dd23d925bb379c59"));
        cj_transactions.insert(uint256S("4c906f897467c7ed8690576edfcaf8b1fb516d154ef6506a2c4cab2c48821728"));
        cj_transactions.insert(uint256S("a42596825352055841949a8270eda6fb37566a8780b2aec6b49d8035955d060e"));
        cj_transactions.insert(uint256S("a554db794560458c102bab0af99773883df13bc66ad287c29610ad9bac138926"));
        cj_transactions.insert(uint256S("792c0bfde7f6bf023ff239660fb876315826a0a52fd32e78ea732057789b2be0"));
    }

    void Update(const CTransactionRef& tx);

    int GetNumTx0s() {
        return tx0s.Size();
    }
};

#endif // BITCOIN_UTIL_COINJOINS_H
