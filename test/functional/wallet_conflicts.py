#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test that wallet correctly tracks transactions that have been conflicted by blocks, particularly during reorgs.
"""

from decimal import Decimal

from test_framework.blocktools import COINBASE_MATURITY
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
        assert_equal,
)

class TxConflicts(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 3

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.test_block_conflicts()
        self.test_mempool_conflict()
        self.test_mempool_and_block_conflicts()

    def test_block_conflicts(self):
        self.log.info("Send tx from which to conflict outputs later")
        txid_conflict_from_1 = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), Decimal("10"))
        txid_conflict_from_2 = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), Decimal("10"))
        self.generate(self.nodes[0], 1)
        self.sync_blocks()

        self.log.info("Disconnect nodes to broadcast conflicts on their respective chains")
        self.disconnect_nodes(0, 1)
        self.disconnect_nodes(2, 1)

        self.log.info("Create a transaction with conflicted transactions spending outpoints A & B")
        nA = next(tx_out["vout"] for tx_out in self.nodes[0].gettransaction(txid_conflict_from_1)["details"] if tx_out["amount"] == Decimal("10"))
        nB = next(tx_out["vout"] for tx_out in self.nodes[0].gettransaction(txid_conflict_from_2)["details"] if tx_out["amount"] == Decimal("10"))
        inputs_tx_AB_0_parent = [{"txid": txid_conflict_from_1, "vout": nA}, {"txid": txid_conflict_from_2, "vout": nB}]
        inputs_tx_A_1 = [{"txid": txid_conflict_from_1, "vout": nA}]
        inputs_tx_B_1 = [{"txid": txid_conflict_from_2, "vout": nB}]

        outputs_tx_AB_0_parent = {self.nodes[0].getnewaddress(): Decimal("19.99998")}
        outputs_tx_A_1 = {self.nodes[0].getnewaddress(): Decimal("9.99998")}
        outputs_tx_B_1 = {self.nodes[0].getnewaddress(): Decimal("9.99998")}

        tx_AB_0_parent = self.nodes[0].signrawtransactionwithwallet(self.nodes[0].createrawtransaction(inputs_tx_AB_0_parent, outputs_tx_AB_0_parent))
        tx_A_1 = self.nodes[0].signrawtransactionwithwallet(self.nodes[0].createrawtransaction(inputs_tx_A_1, outputs_tx_A_1))
        tx_B_1 = self.nodes[0].signrawtransactionwithwallet(self.nodes[0].createrawtransaction(inputs_tx_B_1, outputs_tx_B_1))

        self.log.info("Broadcast conflicted transaction")
        conflicted_txid = self.nodes[0].sendrawtransaction(tx_AB_0_parent["hex"])
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        # Build child conflicted tx
        C = next(tx_out["vout"] for tx_out in self.nodes[0].gettransaction(conflicted_txid)["details"] if tx_out["amount"] == Decimal("19.99998"))
        inputs_tx_C_child = [({"txid": conflicted_txid, "vout": C})]
        outputs_tx_C_child = {self.nodes[0].getnewaddress() : Decimal("19.99996")}
        tx_C_child = self.nodes[0].signrawtransactionwithwallet(self.nodes[0].createrawtransaction(inputs_tx_C_child, outputs_tx_C_child))
        tx_C_child_txid = self.nodes[0].sendrawtransaction(tx_C_child["hex"])
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        self.log.info("Broadcast conflicting tx to node 1")
        conflicting_txid_A = self.nodes[1].sendrawtransaction(tx_A_1["hex"])
        self.generate(self.nodes[1], 4, sync_fun=self.no_op)
        conflicting_txid_B = self.nodes[1].sendrawtransaction(tx_B_1["hex"])
        self.generate(self.nodes[1], 4, sync_fun=self.no_op)

        # Reconnect node0 and node1 and check that conflicted_txid is effectively conflicted
        self.log.info("Connect nodes 0 and 1, and ensure that the tx is effectively conflicted")
        self.connect_nodes(0, 1)
        self.sync_blocks([self.nodes[0], self.nodes[1]])
        conflicted = self.nodes[0].gettransaction(conflicted_txid)
        tx_C_child = self.nodes[0].gettransaction(tx_C_child_txid)
        conflicting = self.nodes[0].gettransaction(conflicting_txid_A)
        # Conflicted tx should have confirmations set to the confirmations of the most conflicting tx
        assert_equal(conflicted["confirmations"], -conflicting["confirmations"])
        # Child should inherit conflicted state from parent
        assert_equal(tx_C_child["confirmations"], -conflicting["confirmations"])
        # Check the confirmations of the conflicting transactions
        assert_equal(conflicting["confirmations"], 8)
        assert_equal(self.nodes[0].gettransaction(conflicting_txid_B)["confirmations"], 4)

        # Node2 chain without conflicts
        self.generate(self.nodes[2], 15, sync_fun=self.no_op)

        # Connect node0 and node2 and wait reorg
        self.connect_nodes(0, 2)
        self.sync_blocks()
        conflicted = self.nodes[0].gettransaction(conflicted_txid)
        tx_C_child = self.nodes[0].gettransaction(tx_C_child_txid)

        self.log.info("Test that formerly conflicted transaction are inactive after a reorg")
        # Former conflicted tx should be unconfirmed as it hasn't been yet rebroadcast
        assert_equal(conflicted["confirmations"], 0)
        # Former conflicted child tx should be unconfirmed as it hasn't been rebroadcast
        assert_equal(tx_C_child["confirmations"], 0)
        # Rebroadcast former conflicted tx and check it confirms smoothly
        self.nodes[2].sendrawtransaction(conflicted["hex"])
        self.generate(self.nodes[2], 1)
        self.sync_blocks()
        former_conflicted = self.nodes[0].gettransaction(conflicted_txid)
        assert_equal(former_conflicted["confirmations"], 1)
        assert_equal(former_conflicted["blockheight"], 217)

    def test_mempool_conflict(self):
        self.nodes[0].createwallet("alice")
        alice = self.nodes[0].get_wallet_rpc("alice")

        self.nodes[1].createwallet("bob")
        bob = self.nodes[1].get_wallet_rpc("bob")

        self.generatetoaddress(self.nodes[0], COINBASE_MATURITY + 3, alice.getnewaddress())

        self.log.info("Test a scenario where a transaction has a mempool conflict")

        unspents = [{"txid" : element["txid"], "vout" : element["vout"]} for element in alice.listunspent()]
        assert_equal(len(unspents), 3)

        raw_tx = alice.createrawtransaction(inputs=[unspents[0], unspents[1]], outputs=[{bob.getnewaddress() : 49.9999}])
        tx1 = alice.signrawtransactionwithwallet(raw_tx)['hex']

        raw_tx = alice.createrawtransaction(inputs=[unspents[1], unspents[2]], outputs=[{bob.getnewaddress() : 49.99}])
        tx2 = alice.signrawtransactionwithwallet(raw_tx)['hex']

        raw_tx = alice.createrawtransaction(inputs=[unspents[2]], outputs=[{bob.getnewaddress() : 24.9899}])
        tx3 = alice.signrawtransactionwithwallet(raw_tx)['hex']

        tx1 = alice.sendrawtransaction(tx1)

        assert_equal(alice.listunspent()[0]["txid"], unspents[2]["txid"])
        assert_equal(alice.getbalance(), 25)

        tx2 = alice.sendrawtransaction(tx2)

        # Check that the 0th unspent is now available because the transaction spending it has been replaced in the mempool
        assert_equal(alice.listunspent()[0]["txid"], unspents[0]["txid"])
        assert_equal(alice.getbalance(), 25)

        self.log.info("Test scenario where a mempool conflict is removed")

        tx3 = alice.sendrawtransaction(tx3)

        # now all of alice's outputs should be considered spent
        assert_equal(alice.listunspent(), [])
        assert_equal(alice.getbalance(), 0)

        alice.sendrawtransaction(alice.gettransaction(tx1)['hex'])
        self.generate(self.nodes[0], 3)
        assert_equal(alice.getbalance(), 75)

    def test_mempool_and_block_conflicts(self):
        alice = self.nodes[0].get_wallet_rpc("alice")
        bob = self.nodes[1].get_wallet_rpc("bob")

        self.log.info("Test a scenario where a transaction has both a block conflict and a mempool conflict")
        unspents = [{"txid" : element["txid"], "vout" : element["vout"]} for element in alice.listunspent()]

        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], 0)

        raw_tx = alice.createrawtransaction(inputs=[unspents[0]], outputs=[{bob.getnewaddress() : 24.99999}])
        raw_tx1 = alice.signrawtransactionwithwallet(raw_tx)['hex']
        tx1 = bob.sendrawtransaction(raw_tx1)

        raw_tx = alice.createrawtransaction(inputs=[unspents[0], unspents[2]], outputs=[{alice.getnewaddress() : 49.999}])
        tx1_conflict = alice.signrawtransactionwithwallet(raw_tx)['hex']

        raw_tx = alice.createrawtransaction(inputs=[unspents[2]], outputs=[{alice.getnewaddress() : 24.99}])
        tx1_conflict_conflict = alice.signrawtransactionwithwallet(raw_tx)['hex']

        raw_tx = alice.createrawtransaction(inputs=[unspents[1]], outputs=[{bob.getnewaddress() : 24.9999}])
        raw_tx2 = alice.signrawtransactionwithwallet(raw_tx)['hex']
        tx2 = bob.sendrawtransaction(raw_tx2)

        raw_tx = alice.createrawtransaction(inputs=[unspents[1]], outputs=[{alice.getnewaddress() : 24.9999}])
        tx2_conflict = alice.signrawtransactionwithwallet(raw_tx)['hex']

        bob_unspents = [{"txid" : element, "vout" : 0} for element in [tx1, tx2]]

        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], Decimal("49.99989000"))

        raw_tx = bob.createrawtransaction(inputs=[bob_unspents[0], bob_unspents[1]], outputs=[{bob.getnewaddress() : 49.999}])
        raw_tx3 = bob.signrawtransactionwithwallet(raw_tx)['hex']
        tx3 = bob.sendrawtransaction(raw_tx3)

        self.disconnect_nodes(0, 1)

        # alice has all 0 txs, bob has 3
        assert_equal(len(alice.getrawmempool()), 0)
        assert_equal(len(bob.getrawmempool()), 3)

        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], Decimal("49.99900000"))

        # bob broadcasts tx_1 conflict
        tx1_conflict = bob.sendrawtransaction(tx1_conflict)
        assert_equal(len(alice.getrawmempool()), 0)
        assert_equal(len(bob.getrawmempool()), 2)

        assert tx2 in bob.getrawmempool()
        assert tx1_conflict in bob.getrawmempool()

        # check that tx3 is now conflicted, so the output from tx2 can now be spent
        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], Decimal("24.99990000"))

        # we will be disconnecting this block in the future
        tx2_conflict = alice.sendrawtransaction(tx2_conflict)
        assert_equal(len(alice.getrawmempool()), 1)
        blk = self.generate(self.nodes[0], 11, sync_fun=self.no_op)[0]
        assert_equal(len(alice.getrawmempool()), 0)

        # check that tx3 and tx1 are now conflicted
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(alice.getbestblockhash(), bob.getbestblockhash())

        assert tx1_conflict in bob.getrawmempool()
        assert_equal(len(bob.getrawmempool()), 1)

        assert_equal(bob.gettransaction(tx3)["confirmations"], -11)
        # bob has no pending funds, since tx1, tx2, and tx3 are all conflicted
        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], 0)
        bob.invalidateblock(blk)
        # bob should still have no pending funds because tx1 and tx3 are still conflicted, and tx2 has not been re-broadcast
        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], 0)
        assert_equal(len(bob.getrawmempool()), 1)
        assert_equal(bob.gettransaction(tx3)["confirmations"], 0)

        bob.sendrawtransaction(raw_tx2)
        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], Decimal("24.99990000"))

        tx1_conflict_conflict = bob.sendrawtransaction(tx1_conflict_conflict)
        bob.sendrawtransaction(raw_tx1)

        # Now bob has no pending funds because tx1 and tx2 are spent by tx3, which hasn't been re-broadcast yet
        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], 0)

        bob.sendrawtransaction(raw_tx3)
        assert_equal(len(bob.getrawmempool()), 4) # The mempool contains: tx1, tx2, tx1_conflict_conflict, tx3
        assert_equal(bob.getbalances()["mine"]["untrusted_pending"], Decimal("49.99900000"))

if __name__ == '__main__':
    TxConflicts().main()
