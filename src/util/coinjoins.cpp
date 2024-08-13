// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/coinjoins.h>

bool WhirlpoolTransactions::isWhirlpool(const CTransactionRef& tx) {
    if (tx->vin.size() == 5 && tx->vout.size() == 5) {
        CAmount amount = tx->vout.at(0).nValue;

        // These are the only whirlpool pools
        if (amount != 5000000 && amount != 1000000 && amount != 50000000) {
            return false;
        }

        for (const auto& tx_out : tx->vout) {
            if (tx_out.nValue != amount) return false;
        }

        for (const CTxIn& tx_in : tx->vin) {
            if (cj_transactions.contains(tx_in.prevout.hash)) return true;
        }
        return false;
    }

    return false;
}

void WhirlpoolTransactions::Update(const CTransactionRef& tx) {
    if (isWhirlpool(tx)) {
        cj_transactions.insert(tx->GetHash());
        for (const CTxIn& tx_in : tx->vin) {
            if (!cj_transactions.contains(tx_in.prevout.hash)) {
                tx0s.Update(tx_in.prevout.hash);
            }
        }
    }
}

