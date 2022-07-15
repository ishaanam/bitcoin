from decimal import Decimal

from test_framework.messages import (
    MAX_BIP125_RBF_SEQUENCE,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
)
DESCRIPTOR = "tr(tprv8ZgxMBicQKsPen4PGtDwURYnCtVMDejyE8vVwMGhQWfVqB2FBPdekhTacDW4vmsKTsgC1wsncVqXiZdX2YFGAnKoLXYf42M78fQJFzuDYFN/*)#uutrcd5m"

class BumpFeeTaprootTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[
            "-walletrbf={}".format(i),
            "-mintxfee=0.00002",
        ] for i in range(self.num_nodes)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def clear_mempool(self):
        # Clear mempool between subtests. The subtests may only depend on chainstate (utxos)
        self.generate(self.nodes[1], 1)

    def test_taproot_psbtbumpfee_notmine(self, rbf_node, peer_node):
        self.log.info("Test psbtbumpfee with external taproot outputs")
        rbf_wallet = rbf_node.get_wallet_rpc("rbf_wallet")
        utxos = []
        utxos.append(rbf_wallet.listunspent()[0])
        utxos.append(peer_node.listunspent()[0])

        inputs = [{
            "txid": utxo["txid"],
            "vout": utxo["vout"],
            "address": utxo["address"],
            "sequence": MAX_BIP125_RBF_SEQUENCE
        } for utxo in utxos]

        output_val = 49.99
        rawtx = peer_node.createrawtransaction(inputs, {peer_node.getnewaddress(): output_val})
        signedtx = rbf_wallet.signrawtransactionwithwallet(rawtx)
        final = peer_node.signrawtransactionwithwallet(signedtx['hex'])
        rbfid = peer_node.sendrawtransaction(final['hex'])
        old_fee = peer_node.getmempoolentry(rbfid)["fees"]["base"]
        psbt = peer_node.psbtbumpfee(rbfid)
        psbt = peer_node.walletprocesspsbt(psbt['psbt'])
        psbt = rbf_wallet.walletprocesspsbt(psbt['psbt'])
        final = peer_node.finalizepsbt(psbt['psbt'])
        res = peer_node.testmempoolaccept([final["hex"]])

        assert res[0]["allowed"]
        assert_greater_than(res[0]["fees"]["base"], old_fee)

        self.clear_mempool()

    def run_test(self):
        peer_node, rbf_node = self.nodes
        rbf_node.createwallet(wallet_name="rbf_wallet", descriptors=True, blank=True)
        rbf_node.createwallet(wallet_name="peer_wallet", descriptors=True, blank=True)
        rbf_wallet = rbf_node.get_wallet_rpc("rbf_wallet")
        rbf_wallet.importdescriptors([{"desc": DESCRIPTOR, "active": True, "timestamp": "now"}])

        rbf_node_address = rbf_wallet.getnewaddress(address_type="bech32m")

        self.log.info("Mining blocks...")
        self.generate(peer_node, 110)
        for _ in range(25):
            peer_node.sendtoaddress(rbf_node_address, 0.001)
        self.sync_all()
        self.generate(peer_node, 1)
        assert_equal(rbf_wallet.getbalance(), Decimal("0.025"))

        self.test_taproot_psbtbumpfee_notmine(rbf_node, peer_node)

if __name__ == "__main__":
    BumpFeeTaprootTest().main()
