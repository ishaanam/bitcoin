#!/usr/bin/env python3
# Copyright (c) 2023 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the wallet's interactions with time-locked UTXOs"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.descriptors import descsum_create


class TimeLocksTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
    
    def run_test(self):
        self.nodes[0].createwallet("alice", descriptors=True, blank=True)
        alice = self.nodes[0].get_wallet_rpc("alice")

        self.nodes[1].createwallet("bob", descriptors=True)
        bob = self.nodes[1].get_wallet_rpc("bob")

        tprv = "tprv8ZgxMBicQKsPerQj6m35no46amfKQdjY7AhLnmatHYXs8S4MTgeZYkWAn4edSGwwL3vkSiiGqSZQrmy5D3P5gBoqgvYP2fCUpBwbKTMTAkL/*"
        ms = f"and_v(v:pk({tprv}),after(110))"
        desc = descsum_create(f"wsh({ms})")
        alice.importdescriptors(
            [
                {
                    "desc": desc,
                    "active": True,
                    "range": 1000,
                    "next_index": 0,
                    "timestamp": "now",
                }
            ]
        )
        self.generatetoaddress(self.nodes[1], 101, bob.getnewaddress())

        bob.sendall([alice.getnewaddress()])
        self.generatetoaddress(self.nodes[1], 1, bob.getnewaddress())

        print(alice.listunspent())

if __name__ == '__main__':
    TimeLocksTest().main()
