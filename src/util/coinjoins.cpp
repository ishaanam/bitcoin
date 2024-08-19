// Copyright (c) 2024 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/coinjoins.h>

bool WhirlpoolTransactions::isWhirlpool(const CTransactionRef& tx) {
    if (tx->vin.size() == 5 && tx->vout.size() == 5) {
        int amount = tx->vout.at(0).nValue;
        bool same_amount = true;

        int denomination = tx->vout.at(0).nValue;

        // These are the only whirlpool pools
        if (denomination != 5000000 && denomination != 1000000 && denomination != 50000000) {
            return false;
        }

        for_each(tx->vout.begin(), tx->vout.end(), [amount, &same_amount](CTxOut tx_out) {
            same_amount &= amount == tx_out.nValue;
        });

        if (same_amount) {
            for (const CTxIn& tx_in : tx->vin) {
                if (cj_transactions.contains(tx_in.prevout.hash)) {
                    // do things
                    return true;
                }
            }
        }
    }

    return false;
}
