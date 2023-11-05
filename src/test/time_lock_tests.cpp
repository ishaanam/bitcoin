// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <optional>
#include <vector>

#include <test/util/setup_common.h>
#include <boost/test/unit_test.hpp>
#include <util/strencodings.h>

#include <script/sign.h>

namespace {
BOOST_FIXTURE_TEST_SUITE(time_lock_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(basic_time_lock_man_test)
{
    TimeLockManager time_locks_a({TimeLock(LOCKTIME_HEIGHT, 100), TimeLock(LOCKTIME_MTP, LOCKTIME_THRESHOLD + 100)});

    // HasSpendingPath
    BOOST_REQUIRE(time_locks_a.HasSpendingPath());

    // Update
    time_locks_a.Update(TimeLock(SEQUENCE_DEPTH, 15)); // add new entry
    time_locks_a.Update(TimeLock(LOCKTIME_HEIGHT, 150)); // update existing entry
    BOOST_CHECK_EQUAL(time_locks_a.GetType(LOCKTIME_HEIGHT).value().value.value(), 150);

    // HasType
    BOOST_REQUIRE(time_locks_a.HasType(LOCKTIME_HEIGHT));
    BOOST_REQUIRE(time_locks_a.HasType(LOCKTIME_MTP));
    BOOST_REQUIRE(time_locks_a.HasType(SEQUENCE_DEPTH));

    // GetType
    BOOST_REQUIRE(time_locks_a.GetType(LOCKTIME_HEIGHT).value() == TimeLock(LOCKTIME_HEIGHT));
    BOOST_REQUIRE(time_locks_a.GetType(LOCKTIME_MTP).value() == TimeLock(LOCKTIME_MTP));
    BOOST_REQUIRE(time_locks_a.GetType(SEQUENCE_DEPTH).value() == TimeLock(SEQUENCE_DEPTH));
    BOOST_REQUIRE(!time_locks_a.GetType(NO_TIMELOCKS).has_value());

    TimeLockManager time_locks_b;

    BOOST_REQUIRE(!time_locks_b.HasSpendingPath());
}

BOOST_AUTO_TEST_CASE(basic_combined_time_lock_man_test)
{
    TimeLockManager time_locks_a({TimeLock(NO_TIMELOCKS), TimeLock(LOCKTIME_HEIGHT, 200)});
    TimeLockManager time_locks_b({TimeLock(LOCKTIME_HEIGHT, 300), TimeLock(LOCKTIME_MTP, LOCKTIME_THRESHOLD + 100)});

    std::vector<TimeLockManager> time_lock_managers;

    time_lock_managers.push_back(time_locks_a);
    time_lock_managers.push_back(time_locks_b);

    {
        TimeLockManager combined = time_locks_a.And(time_locks_b);
        BOOST_REQUIRE(combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));
        BOOST_REQUIRE(!combined.HasType(NO_TIMELOCKS));

        int locktime_height = combined.GetType(LOCKTIME_HEIGHT).value().value.value();
        BOOST_CHECK_EQUAL(locktime_height, 300); // the higher locktime should be set
    }

    {
        TimeLockManager combined = time_locks_a.Or(time_locks_b);
        BOOST_REQUIRE(combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(NO_TIMELOCKS));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));

        int locktime_height = combined.GetType(LOCKTIME_HEIGHT).value().value.value();
        BOOST_CHECK_EQUAL(locktime_height, 300); // the higher locktime should be set
    }
}

BOOST_AUTO_TEST_CASE(advanced_combined_time_lock_man_test)
{
    TimeLockManager time_locks_a({TimeLock(NO_TIMELOCKS), TimeLock(LOCKTIME_HEIGHT, 200)});
    TimeLockManager time_locks_b({TimeLock(LOCKTIME_HEIGHT, 300), TimeLock(LOCKTIME_MTP, LOCKTIME_THRESHOLD + 100)});
    TimeLockManager time_locks_c({TimeLock(SEQUENCE_DEPTH, 15), TimeLock(LOCKTIME_MTP, LOCKTIME_THRESHOLD + 200)});

    std::vector<TimeLockManager> time_lock_managers;

    time_lock_managers.push_back(time_locks_a);
    time_lock_managers.push_back(time_locks_b);
    time_lock_managers.push_back(time_locks_c);

    {
        TimeLockManager combined = TimeLockManager::Thresh(time_lock_managers, 1);

        BOOST_REQUIRE(combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));
        BOOST_REQUIRE(combined.HasType(NO_TIMELOCKS));
        BOOST_REQUIRE(combined.HasType(SEQUENCE_DEPTH));

        int locktime_height = combined.GetType(LOCKTIME_HEIGHT).value().value.value();
        BOOST_CHECK_EQUAL(locktime_height, 300); // the higher locktime should be set

        int locktime_mtp = combined.GetType(LOCKTIME_MTP).value().value.value();
        BOOST_CHECK_EQUAL(locktime_mtp, LOCKTIME_THRESHOLD + 200); // the higher locktime should be set

        int sequence = combined.GetType(SEQUENCE_DEPTH).value().value.value();
        BOOST_CHECK_EQUAL(sequence, 15);
    }

    {
        TimeLockManager combined = TimeLockManager::Thresh(time_lock_managers, 2);

        BOOST_REQUIRE(combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));
        BOOST_REQUIRE(!combined.HasType(NO_TIMELOCKS));
        BOOST_REQUIRE(combined.HasType(SEQUENCE_DEPTH));

        int locktime_height = combined.GetType(LOCKTIME_HEIGHT).value().value.value();
        BOOST_CHECK_EQUAL(locktime_height, 300); // the higher locktime should be set

        int locktime_mtp = combined.GetType(LOCKTIME_MTP).value().value.value();
        BOOST_CHECK_EQUAL(locktime_mtp, LOCKTIME_THRESHOLD + 200); // the higher locktime should be set

        int sequence = combined.GetType(SEQUENCE_DEPTH).value().value.value();
        BOOST_CHECK_EQUAL(sequence, 15);
    }

    {
        TimeLockManager combined = TimeLockManager::Thresh(time_lock_managers, 3);

        BOOST_REQUIRE(!combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));
        BOOST_REQUIRE(!combined.HasType(NO_TIMELOCKS));
        BOOST_REQUIRE(!combined.HasType(SEQUENCE_DEPTH));

        int locktime_mtp = combined.GetType(LOCKTIME_MTP).value().value.value();
        BOOST_CHECK_EQUAL(locktime_mtp, LOCKTIME_THRESHOLD + 200); // the higher locktime should be set
    }
}
}
BOOST_AUTO_TEST_SUITE_END()
