#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the setting and updating of transaction states."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.blocktools import COINBASE_MATURITY
from test_framework.util import (
    assert_equal,
)

class TxStatesTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True

    def run_test(self):
        self.log.info("Setting up wallets...")

        self.nodes[0].createwallet("alice")
        alice = self.nodes[0].get_wallet_rpc("alice")

        self.nodes[1].createwallet("bob")
        bob = self.nodes[1].get_wallet_rpc("bob")

        self.generatetoaddress(self.nodes[0], COINBASE_MATURITY + 3, alice.getnewaddress())

        self.log.info("Test a scenario where a transaction has a mempool conflict")

        unspents = [{"txid" : element["txid"], "vout" : element["vout"]} for element in alice.listunspent()]

        raw_tx = alice.createrawtransaction(inputs=[unspents[0], unspents[1]], outputs=[{bob.getnewaddress() : 99.9999}])
        tx1 = alice.signrawtransactionwithwallet(raw_tx)['hex']

        raw_tx = alice.createrawtransaction(inputs=[unspents[1], unspents[2]], outputs=[{bob.getnewaddress() : 99.99}])
        tx2 = alice.signrawtransactionwithwallet(raw_tx)['hex']

        raw_tx = alice.createrawtransaction(inputs=[unspents[2]], outputs=[{bob.getnewaddress() : 49.9899}])
        tx3 = alice.signrawtransactionwithwallet(raw_tx)['hex']

        tx1 = alice.sendrawtransaction(tx1)

        assert_equal(alice.listunspent()[0]["txid"], unspents[2]["txid"])
        assert_equal(alice.getbalance(), 50)

        tx2 = alice.sendrawtransaction(tx2)

        # Check that the 0th unspent is now available because the transaction spending it has been replaced in the mempool
        assert_equal(alice.listunspent()[0]["txid"], unspents[0]["txid"])
        assert_equal(alice.getbalance(), 50)

        self.log.info("Test scenario where a mempool conflict is removed")

        tx3 = alice.sendrawtransaction(tx3)

        # now all of alice's outputs should be considered spent
        assert_equal(alice.listunspent(), [])
        assert_equal(alice.getbalance(), 0)

        alice.sendrawtransaction(alice.gettransaction(tx1)['hex'])
        self.generate(self.nodes[0], 2)
        assert_equal(alice.getbalance(), 100)

        self.log.info("Test a scenario where a transaction has both a block conflict and a mempool conflict")
        unspents = [{"txid" : element["txid"], "vout" : element["vout"]} for element in alice.listunspent()]

        raw_tx = alice.createrawtransaction(inputs=[unspents[0]], outputs=[{bob.getnewaddress() : 49.9999}])
        tx1 = alice.signrawtransactionwithwallet(raw_tx)['hex']
        tx1 = alice.sendrawtransaction(tx1)

        raw_tx = alice.createrawtransaction(inputs=[unspents[0]], outputs=[{alice.getnewaddress() : 49.999}])
        tx1_conflict = alice.signrawtransactionwithwallet(raw_tx)['hex']

        self.sync_mempools()
        self.disconnect_nodes(0, 1)

        raw_tx = alice.createrawtransaction(inputs=[unspents[1]], outputs=[{bob.getnewaddress() : 49.9999}])
        tx2 = alice.signrawtransactionwithwallet(raw_tx)['hex']
        tx2 = alice.sendrawtransaction(tx2)

        raw_tx = alice.createrawtransaction(inputs=[unspents[1]], outputs=[{alice.getnewaddress() : 49.9999}])
        tx2_conflict = alice.signrawtransactionwithwallet(raw_tx)['hex']

        bob_unspents = [{"txid" : element["txid"], "vout" : element["vout"]} for element in bob.listunspent()]

        raw_tx = bob.createrawtransaction(inputs=[unspents[0], unspents[1]], outputs=[{alice.getnewaddress() : 99.9999}])
        tx3 = bob.signrawtransactionwithwallet(raw_tx)['hex']

        assert_equal(len(alice.getrawmempool()), 2)
        assert_equal(len(bob.getrawmempool()), 1)

        # alice has all 3 txs, bob has none
        # bob mines tx_1 conflict
        # check that tx3 and tx1 are now conflicted
        # broadcast tx_2 conflict, make sure all the og is now conflicted
        # undo the tx_1 conflict (using disconnect block w/ reorg of over 10 blocks)
        # check that tx3 and tx2 are still conflicted
        # somehow undo the tx_2 conflict
        # check that tx1, tx2, tx3 are now unconflicted, rebroadcast them and mine

        self.log.info("Test a scenario where a transaction has two mempool conflicts which get removed")
        self.log.info("Test a scenario where a transaction has two mempool conflicts sequentially")

if __name__ == '__main__':
    TxStatesTest().main()
