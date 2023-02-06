#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the transaction state transitions are performed correctly."""

from enum import Enum

from test_framework.test_framework import BitcoinTestFramework

from test_framework.util import (
    assert_equal,
    assert_greater_than,
)

class TxState(Enum):
    CONFIRMED = 0
    INMEMPOOL = 1
    INACTIVE = 2
    CONFLICTED = 3
    ABANDONED = 4

def cleanup(func):
    def wrapper(self):
        try:
            func(self)
        finally:
            self.alice.invalidateblock(self.reset_block)
            self.bob.invalidateblock(self.reset_block)

            for txid in self.added_txs:
                self.alice.abandontransaction(txid)
            self.added_txs = []

            balances = self.bob.getbalances()["mine"]
            for balance_type in ["trusted", "untrusted_pending", "immature"]:
                assert_equal(balances[balance_type], 0)

            balances = self.alice.getbalances()["mine"]

            assert_equal(balances["trusted"], 0)
            assert_equal(balances["untrusted_pending"], 0)
            assert_equal(balances["immature"], 5000)

            self.disconnect_nodes(0, 1)

            self.alice.reconsiderblock(self.reset_block)
            self.bob.reconsiderblock(self.reset_block)

            balances = self.alice.getbalances()["mine"]
            assert_equal(balances["trusted"], 50)
            assert_equal(balances["untrusted_pending"], 0)

    return wrapper

class TxStatesTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            [
                "-txindex",
            ],
            [
                "-txindex",
            ],
        ]

    def run_test(self):
        alice = self.nodes[0]
        alice.createwallet("alice")
        self.alice = alice.get_wallet_rpc("alice")

        bob = self.nodes[1]
        bob.createwallet("bob")
        self.bob = bob.get_wallet_rpc("bob")

        self.added_txs = []

        self.generatetoaddress(alice, 101, self.alice.getnewaddress(), sync_fun=self.sync_all())

        self.reset_block = self.alice.getbestblockhash()

        assert_equal(alice.getbestblockhash(), bob.getbestblockhash())

        self.disconnect_nodes(0, 1)

        self.test_confirmed_tx_state()

    def check_state(self, txid, expected_state, num_conflicts = 0):
        """
            - confirmations
            - trusted
            - walletconflicts
        """

        prev_balances = self.bob.getbalances()

        self.connect_nodes(0, 1)

        txinfo = self.bob.gettransaction(txid)
        decoded_tx = self.bob.decoderawtransaction(txinfo["hex"])
        added_value = 0

        for output in decoded_tx["vout"]:
            added_value += output["value"]

        if (expected_state == TxState.CONFIRMED):
            assert_greater_than(txinfo["confirmations"], 0)
            prev_balances["mine"]["trusted"] += added_value
            assert_equal(self.bob.getbalances(), prev_balances)
        elif (expected_state == TxState.INMEMPOOL):
            assert_equal(txinfo["confirmations"], 0)
            prev_balances["mine"]["untrusted_pending"] += added_value
            assert_equal(self.bob.getbalances(), prev_balances)
        elif (expected_state == TxState.INACTIVE):
            pass
            # confirmations = 0
            # trusted = false
            # all inputs removed from balance calculation
        elif (expected_state == TxState.CONFLICTED):
            pass
            # confirmations < 0
            # trusted = false
            # all inputs present in balance calculation
        elif (expected_state == TxState.ABANDONED):
            pass
            # confirmations = 0 
            # trusted = false
            # all inputs present in balance calculation

    @cleanup
    def test_confirmed_tx_state(self):
        txid1 = self.alice.sendall([self.bob.getnewaddress()])["txid"]
        self.added_txs.append(txid1)
        self.generate(self.nodes[0], 2, sync_fun=self.no_op)
        self.check_state(txid1, TxState.CONFIRMED)

if __name__ == '__main__':
    TxStatesTest().main()
