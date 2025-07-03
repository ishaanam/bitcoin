#!/usr/bin/env python3
# Copyright (c) 2025 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test how the wallet deals with v3 transactions"""

from decimal import Decimal, getcontext
import time

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
)

class WalletV3Test(BitcoinTestFramework):
    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        getcontext().prec=10
        self.num_nodes = 2
        self.setup_clean_chain = True

    def run_test(self):
        self.connect_nodes(0, 1)

        self.nodes[0].createwallet("alice")
        self.alice = self.nodes[0].get_wallet_rpc("alice")

        self.nodes[1].createwallet("bob")
        self.bob = self.nodes[1].get_wallet_rpc("bob")

        self.generatetoaddress(self.nodes[0], 100, self.alice.getnewaddress())

        self.truc_tx_with_conflicting_sibling()

    def truc_tx_with_conflicting_sibling(self):
        # unconfirmed v3 tx to alice & bob
        self.log.info("Test v3 transaction with conflicting sibling")
        self.generate(self.nodes[0], 1)

        inputs=[]
        outputs = {self.bob.getnewaddress() : 2.0, self.alice.getnewaddress() : 2.0}
        parent_tx = self.alice.createrawtransaction(inputs=inputs, outputs=outputs)
        parent_tx = list(parent_tx)
        parent_tx[1] = '3'
        parent_tx = "".join(parent_tx)
        parent_tx = self.alice.fundrawtransaction(parent_tx)
        parent_tx = self.alice.signrawtransactionwithwallet(parent_tx["hex"])
        self.alice.sendrawtransaction(parent_tx["hex"])
        self.sync_mempools()
        parent_txid = self.alice.getrawmempool()[0]

        # alice spends her output with a v3 transaction
        inputs=[{'txid' : parent_txid, 'vout' : 1},]
        outputs = {self.alice.getnewaddress() : Decimal("1.99999")}
        alice_tx = self.alice.createrawtransaction(inputs=inputs, outputs=outputs)
        alice_tx = list(alice_tx)
        alice_tx[1] = '3'
        alice_tx = "".join(alice_tx)
        alice_tx = self.alice.signrawtransactionwithwallet(alice_tx)
        print(alice_tx)
        self.sync_mempools()

if __name__ == '__main__':
    WalletV3Test(__file__).main()
