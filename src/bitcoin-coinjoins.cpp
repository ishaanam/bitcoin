// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// The bitcoin-chainstate executable serves to surface the dependencies required
// by a program wishing to use Bitcoin Core's consensus engine as it is right
// now.
//
// DEVELOPER NOTE: Since this is a "demo-only", experimental, etc. executable,
//                 it may diverge from Bitcoin Core's coding style.
//
// It is part of the libbitcoinkernel project.

#include <kernel/chainparams.h>
#include <kernel/chainstatemanager_opts.h>
#include <kernel/checks.h>
#include <kernel/context.h>
#include <kernel/validation_cache_sizes.h>
#include <kernel/warning.h>

#include <consensus/validation.h>
#include <core_io.h>
#include <node/blockstorage.h>
#include <node/caches.h>
#include <node/chainstate.h>
#include <random.h>
#include <script/sigcache.h>
#include <util/chaintype.h>
#include <util/fs.h>
#include <util/signalinterrupt.h>
#include <util/task_runner.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>

int main(int argc, char* argv[])
{
    // SETUP: Argument parsing and handling
    if (argc != 2) {
        std::cerr
            << "Usage: " << argv[0] << " DATADIR" << std::endl
            << "Display DATADIR information, and process hex-encoded blocks on standard input." << std::endl
            << std::endl
            << "IMPORTANT: THIS EXECUTABLE IS EXPERIMENTAL, FOR TESTING ONLY, AND EXPECTED TO" << std::endl
            << "           BREAK IN FUTURE VERSIONS. DO NOT USE ON YOUR ACTUAL DATADIR." << std::endl;
        return 1;
    }
    fs::path abs_datadir{fs::absolute(argv[1])};
    fs::create_directories(abs_datadir);


    // SETUP: Context
    kernel::Context kernel_context{};
    // We can't use a goto here, but we can use an assert since none of the
    // things instantiated so far requires running the epilogue to be torn down
    // properly
    assert(kernel::SanityChecks(kernel_context));

    // Necessary for CheckInputScripts (eventually called by ProcessNewBlock),
    // which will try the script cache first and fall back to actually
    // performing the check with the signature cache.
    kernel::ValidationCacheSizes validation_cache_sizes{};
    Assert(InitSignatureCache(validation_cache_sizes.signature_cache_bytes));
    Assert(InitScriptExecutionCache(validation_cache_sizes.script_execution_cache_bytes));

    ValidationSignals validation_signals{std::make_unique<util::ImmediateTaskRunner>()};

    class KernelNotifications : public kernel::Notifications
    {
    public:
        kernel::InterruptResult blockTip(SynchronizationState, CBlockIndex&) override
        {
            std::cout << "Block tip changed" << std::endl;
            return {};
        }
        void headerTip(SynchronizationState, int64_t height, int64_t timestamp, bool presync) override
        {
            std::cout << "Header tip changed: " << height << ", " << timestamp << ", " << presync << std::endl;
        }
        void progress(const bilingual_str& title, int progress_percent, bool resume_possible) override
        {
            std::cout << "Progress: " << title.original << ", " << progress_percent << ", " << resume_possible << std::endl;
        }
        void warningSet(kernel::Warning id, const bilingual_str& message) override
        {
            std::cout << "Warning " << static_cast<int>(id) << " set: " << message.original << std::endl;
        }
        void warningUnset(kernel::Warning id) override
        {
            std::cout << "Warning " << static_cast<int>(id) << " unset" << std::endl;
        }
        void flushError(const bilingual_str& message) override
        {
            std::cerr << "Error flushing block data to disk: " << message.original << std::endl;
        }
        void fatalError(const bilingual_str& message) override
        {
            std::cerr << "Error: " << message.original << std::endl;
        }
    };
    auto notifications = std::make_unique<KernelNotifications>();


    // SETUP: Chainstate
    auto chainparams = CChainParams::Main();
    const ChainstateManager::Options chainman_opts{
        .chainparams = *chainparams,
        .datadir = abs_datadir,
        .notifications = *notifications,
        .signals = &validation_signals,
    };
    const node::BlockManager::Options blockman_opts{
        .chainparams = chainman_opts.chainparams,
        .blocks_dir = abs_datadir / "blocks",
        .notifications = chainman_opts.notifications,
    };
    util::SignalInterrupt interrupt;
    ChainstateManager chainman{interrupt, chainman_opts, blockman_opts};

    node::CacheSizes cache_sizes;
    cache_sizes.block_tree_db = 2 << 20;
    cache_sizes.coins_db = 2 << 22;
    cache_sizes.coins = (450 << 20) - (2 << 20) - (2 << 22);
    node::ChainstateLoadOptions options;
    auto [status, error] = node::LoadChainstate(chainman, cache_sizes, options);
    if (status != node::ChainstateLoadStatus::SUCCESS) {
        std::cerr << "Failed to load Chain state from your datadir." << std::endl;
        goto epilogue;
    } else {
        std::tie(status, error) = node::VerifyLoadedChainstate(chainman, options);
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            std::cerr << "Failed to verify loaded Chain state from your datadir." << std::endl;
            goto epilogue;
        }
    }

    for (Chainstate* chainstate : WITH_LOCK(::cs_main, return chainman.GetAll())) {
        BlockValidationState state;
        if (!chainstate->ActivateBestChain(state, nullptr)) {
            std::cerr << "Failed to connect best block (" << state.ToString() << ")" << std::endl;
            goto epilogue;
        }
    }

    // Main program logic starts here

    CBlockIndex* current_height;

    {
        LOCK(::cs_main);
        current_height = chainman.m_blockman.LookupBlockIndex(uint256S("0000000000000000000682164224dc4979662b3824ccad634d52f1bd1a232b00"));
    }

    if (current_height) {
        for (int i = 0; i < 10; i++) {
            std::cout << "In Block: #" << current_height->nHeight << "\n";

            int num_whirlpool = 0;
            CBlock block;
            chainman.m_blockman.ReadBlockFromDisk(block, *current_height);

            // eventually replace with some sort of CoinJoin criteria

            for (const CTransactionRef& tx : block.vtx) {
                if (tx->vin.size() == 5 && tx->vout.size() == 5) {
                    int amount = tx->vout.at(0).nValue;
                    bool same_amount = true;

                    for_each(tx->vout.begin(), tx->vout.end(), [amount, &same_amount](CTxOut tx_out) {
                        same_amount &= amount == tx_out.nValue;
                    });

                    if (same_amount) {
                        num_whirlpool += 1;
                        std::cout << "\tWhirlpool Transaction: " << tx->GetHash().ToString() << "\n";
                    }
                }
            }

            std::cout << "\t" << num_whirlpool << " Whirlpool transactions\n";

            // move to next block
            current_height = current_height->pprev;

            assert(current_height);
        }
    }

epilogue:
    // Without this precise shutdown sequence, there will be a lot of nullptr
    // dereferencing and UB.
    if (chainman.m_thread_load.joinable()) chainman.m_thread_load.join();

    validation_signals.FlushBackgroundCallbacks();
    {
        LOCK(cs_main);
        for (Chainstate* chainstate : chainman.GetAll()) {
            if (chainstate->CanFlushToDisk()) {
                chainstate->ForceFlushStateToDisk();
                chainstate->ResetCoinsViews();
            }
        }
    }
}
