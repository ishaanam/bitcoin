#!/usr/bin/env python3
# Copyright (c) 2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test signrawtransactionwithdescriptor RPC."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.descriptors import descsum_create
from test_framework.key import ECKey
from test_framework.wallet import DEFAULT_FEE
from test_framework.wallet_util import get_generate_key
from test_framework.util import assert_equal

class SignRawTransactionWithDescriptorTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def test_simple_signrawtransactionwithdescriptor(self):
        self.log.info("Test basic signrawtransactionwithdescriptor use case")
        key_info = get_generate_key()
        recv_key = key_info[0]
        recv_address = key_info[3]

        self.generatetoaddress(self.recv_node, 101, recv_address)
        self.recv_node.importprivkey(recv_key)

        address = self.send_node.getnewaddress()
        amount = self.recv_node.getbalance() - DEFAULT_FEE
        tx_out = {address: amount}

        tx_in = self.recv_node.listunspent()[0]

        raw_tx = self.recv_node.createrawtransaction([tx_in], [tx_out])

        self.descriptor1 = descsum_create(f"wpkh({recv_key})")
        res_desc = self.recv_node.signrawtransactionwithdescriptor(raw_tx, [self.descriptor1])
        assert res_desc['complete'] == True
        self.recv_node.sendrawtransaction(res_desc['hex'])

    def test_multisig_signrawtransctionwithdescriptor(self):
        self.log.info("Test signrawtransactionwithdescriptor using a p2sh 2-of-3 multisig descriptor")

        # 2-of-3 multisig p2sh descriptor
        self.descriptor2 = descsum_create("sh(multi(2,022f01e5e15cca351daff3843fb70f3c2f0a1bdd05e5af888a67784ef3e10a2a01,cU7wuepRRARNoPgQiZS8nirByxQWaGToHvs8K6ZsG7AaRHwD9eEb, cQfcCFAbgS8XyjrqdA3LmBmd5dDX1jNbe4mUB37FthQYwMr4q718))")

        multisig_address = self.recv_node.deriveaddresses(self.descriptor2)[0]
        self.send_node.importaddress(multisig_address)
        self.generatetoaddress(self.send_node, 101, multisig_address)

        tx_in = self.send_node.listunspent()[0]

        self.to_address = self.recv_node.getnewaddress()
        amount = tx_in['amount'] - DEFAULT_FEE
        tx_out = {self.to_address: amount}

        raw_tx = self.send_node.createrawtransaction([tx_in], [tx_out])
        res_desc = self.recv_node.signrawtransactionwithdescriptor(raw_tx, [self.descriptor2])
        assert res_desc['complete'] == True

    def test_signrawtransaction_with_multiple_inputs(self):
        self.log.info("Test signing multiple inputs with multiple descriptors")
        tx_in1 = self.send_node.listunspent()[0]
        tx_in2 = self.recv_node.listunspent()[0]

        amount = tx_in1['amount'] + tx_in2['amount'] - DEFAULT_FEE
        tx_out = {self.to_address: amount}

        raw_tx = self.send_node.createrawtransaction([tx_in1, tx_in2], [tx_out])
        res_desc = self.recv_node.signrawtransactionwithdescriptor(raw_tx, [self.descriptor1, self.descriptor2])
        assert res_desc['complete'] == True

    def run_test(self):
        self.recv_node, self.send_node = self.nodes

        self.test_simple_signrawtransactionwithdescriptor()
        self.test_multisig_signrawtransctionwithdescriptor()
        self.test_signrawtransaction_with_multiple_inputs()

if __name__ == '__main__':
    SignRawTransactionWithDescriptorTest().main()
