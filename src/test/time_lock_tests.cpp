// Copyright (c) 2023 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <optional>

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
    time_locks_a.Update(TimeLock(SEQUENCE, 15)); // add new entry
    time_locks_a.Update(TimeLock(LOCKTIME_HEIGHT, 150)); // update existing entry
    BOOST_CHECK_EQUAL(time_locks_a.GetType(LOCKTIME_HEIGHT).value().value.value(), 150);

    // HasType
    BOOST_REQUIRE(time_locks_a.HasType(LOCKTIME_HEIGHT));
    BOOST_REQUIRE(time_locks_a.HasType(LOCKTIME_MTP));
    BOOST_REQUIRE(time_locks_a.HasType(SEQUENCE));

    // GetType
    BOOST_REQUIRE(time_locks_a.GetType(LOCKTIME_HEIGHT).value() == TimeLock(LOCKTIME_HEIGHT));
    BOOST_REQUIRE(time_locks_a.GetType(LOCKTIME_MTP).value() == TimeLock(LOCKTIME_MTP));
    BOOST_REQUIRE(time_locks_a.GetType(SEQUENCE).value() == TimeLock(SEQUENCE));
    BOOST_REQUIRE(!time_locks_a.GetType(NO_TIMELOCKS).has_value());

    TimeLockManager time_locks_b;

    BOOST_REQUIRE(!time_locks_b.HasSpendingPath());
}

BOOST_AUTO_TEST_CASE(combined_time_lock_man_test)
{
    TimeLockManager time_locks_a({TimeLock(NO_TIMELOCKS), TimeLock(LOCKTIME_HEIGHT, 200)});
    TimeLockManager time_locks_b({TimeLock(LOCKTIME_HEIGHT, 300), TimeLock(LOCKTIME_MTP, LOCKTIME_THRESHOLD + 100)});

    {
        TimeLockManager combined = time_locks_a.Combine(time_locks_b, /*required=*/true);// AND
        BOOST_REQUIRE(combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));
        BOOST_REQUIRE(!combined.HasType(NO_TIMELOCKS));

        int locktime_height = combined.GetType(LOCKTIME_HEIGHT).value().value.value();
        BOOST_CHECK_EQUAL(locktime_height, 300); // the higher locktime should be set
    }

    {
        TimeLockManager combined = time_locks_a.Combine(time_locks_b, /*required=*/false);// OR
        BOOST_REQUIRE(combined.HasType(LOCKTIME_HEIGHT));
        BOOST_REQUIRE(combined.HasType(NO_TIMELOCKS));
        BOOST_REQUIRE(combined.HasType(LOCKTIME_MTP));

        int locktime_height = combined.GetType(LOCKTIME_HEIGHT).value().value.value();
        BOOST_CHECK_EQUAL(locktime_height, 300); // the higher locktime should be set
    }
}
}
BOOST_AUTO_TEST_SUITE_END()
