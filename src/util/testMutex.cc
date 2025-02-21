// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */
/**
 * @brief test Mutex
 */

// System headers
#include <atomic>
#include <mutex>
#include <sstream>
#include <thread>

// LSST headers
#include "lsst/log/Log.h"

#define MUTEX_UNITTEST

// Qserv headers
#include "util/BlockPost.h"
#include "util/Mutex.h"

// Boost unit test header
#define BOOST_TEST_MODULE Mutex
#include <boost/test/unit_test.hpp>

using namespace std;
namespace test = boost::test_tools;
using namespace lsst::qserv::util;

namespace {
string thisThreadId2str() {
    ostringstream ss;
    ss << this_thread::get_id();
    return ss.str();
}
}  // namespace

BOOST_AUTO_TEST_SUITE(Suite)

BOOST_AUTO_TEST_CASE(MutexTest) {
    // Test the interface of class Mutex to comply with expectations
    // of the standard std::lock_guard<T>.
    LOGS_INFO("MutexTest begins");

    // The mutex won't be locked by anyone
    Mutex mtx1;
    BOOST_CHECK(!mtx1.lockedByThread());

    // The mutex will be locked by the current thread
    Mutex mtx2;
    lock_guard<Mutex> const lockGuard2(mtx2);
    BOOST_CHECK(mtx2.lockedByThread());

    // Lock this mutex in each of two separate threads. Let each thread
    // to wait for a random period of time within some interval before
    // grabbing a lock. This would ensure threads would attempt locking
    // at random order.
    //
    // Note the wait interval for each thread is a random
    // number of milliseconds within the same interval of time.
    // The average run time of the test is the average wait time
    // multiplied by the number of iterations of the loop.
    for (int i = 0; i < 100; ++i) {
        Mutex mtx;
        bool wasLockedBeforeBy1 = false;
        bool wasLockedAfterBy1 = false;
        thread thr1([&mtx, &wasLockedBeforeBy1, &wasLockedAfterBy1]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            wasLockedBeforeBy1 = mtx.lockedByThread();
            lock_guard<Mutex> const lock(mtx);
            wasLockedAfterBy1 = mtx.lockedByThread();
        });
        bool wasLockedBeforeBy2 = false;
        bool wasLockedAfterBy2 = false;
        thread thr2([&mtx, &wasLockedBeforeBy2, &wasLockedAfterBy2]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            wasLockedBeforeBy2 = mtx.lockedByThread();
            lock_guard<Mutex> const lock(mtx);
            wasLockedAfterBy2 = mtx.lockedByThread();
        });
        thr1.join();
        BOOST_CHECK(!wasLockedBeforeBy1);
        BOOST_CHECK(wasLockedAfterBy1);
        thr2.join();
        BOOST_CHECK(!wasLockedBeforeBy2);
        BOOST_CHECK(wasLockedAfterBy2);
    }
    // Test the correctness of the Mutex implementation by using a non-atomic
    // counter to be incremented after acquiring a lock.
    {
        Mutex mtx;
        unsigned int counter = 0;
        unsigned int const steps = 1024;
        unsigned int const numThreads = min(2U, thread::hardware_concurrency());
        vector<unique_ptr<thread>> threads(numThreads);
        for (auto&& t : threads) {
            t = make_unique<thread>([&mtx, &counter]() {
                for (unsigned int i = 0; i < steps; ++i) {
                    lock_guard<Mutex> const lock(mtx);
                    ++counter;
                }
            });
        }
        for (auto&& t : threads) {
            t->join();
        }
        BOOST_CHECK_EQUAL(counter, steps * numThreads);
    }
    LOGS_INFO("MutexTest ends");
}

BOOST_AUTO_TEST_CASE(VMutexTest) {
    // Test the interface of class Mutex to comply with expectations
    // of the standard std::lock_guard<T>.
    LOGS_INFO("VMutexTest begins");

    // The mutex won't be locked by anyone
    VMutex mtx1;
    BOOST_CHECK(!mtx1.lockedByThread());
    BOOST_CHECK_THROW(VMUTEX_HELD(mtx1), lsst::qserv::util::Bug);
    BOOST_REQUIRE_NO_THROW(VMUTEX_NOT_HELD(mtx1));

    // The mutex will be locked by the current thread
    VMutex mtx2;
    lock_guard<VMutex> const lockGuard2(mtx2);
    BOOST_CHECK(mtx2.lockedByThread());
    BOOST_REQUIRE_NO_THROW(VMUTEX_HELD(mtx2));
    BOOST_CHECK_THROW(VMUTEX_NOT_HELD(mtx2), lsst::qserv::util::Bug);

    // Lock this mutex in each of two separate threads. Let each thread
    // to wait for a random period of time within some interval before
    // grabbing a lock. This would ensure threads would attempt locking
    // at random order.
    //
    // Note the wait interval for each thread is a random
    // number of milliseconds within the same interval of time.
    // The average run time of the test is the average wait time
    // multiplied by the number of iterations of the loop.
    for (int i = 0; i < 100; ++i) {
        VMutex mtx;
        bool wasLockedBeforeBy1 = false;
        bool wasLockedAfterBy1 = false;
        thread thr1([&mtx, &wasLockedBeforeBy1, &wasLockedAfterBy1]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            wasLockedBeforeBy1 = mtx.lockedByThread();
            lock_guard<VMutex> const lock(mtx);
            wasLockedAfterBy1 = mtx.lockedByThread();
        });
        bool wasLockedBeforeBy2 = false;
        bool wasLockedAfterBy2 = false;
        thread thr2([&mtx, &wasLockedBeforeBy2, &wasLockedAfterBy2]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            wasLockedBeforeBy2 = mtx.lockedByThread();
            lock_guard<VMutex> const lock(mtx);
            wasLockedAfterBy2 = mtx.lockedByThread();
        });
        thr1.join();
        BOOST_CHECK(!wasLockedBeforeBy1);
        BOOST_CHECK(wasLockedAfterBy1);
        thr2.join();
        BOOST_CHECK(!wasLockedBeforeBy2);
        BOOST_CHECK(wasLockedAfterBy2);
    }
    // Test the correctness of the Mutex implementation by using a non-atomic
    // counter to be incremented after acquiring a lock.
    {
        VMutex mtx;
        unsigned int counter = 0;
        unsigned int const steps = 1024;
        unsigned int const numThreads = min(2U, thread::hardware_concurrency());
        vector<unique_ptr<thread>> threads(numThreads);
        for (auto&& t : threads) {
            t = make_unique<thread>([&mtx, &counter]() {
                for (unsigned int i = 0; i < steps; ++i) {
                    lock_guard<VMutex> const lock(mtx);
                    ++counter;
                }
            });
        }
        for (auto&& t : threads) {
            t->join();
        }
        BOOST_CHECK_EQUAL(counter, steps * numThreads);
    }

    LOGS_INFO("VMutexTest ends");
}

BOOST_AUTO_TEST_CASE(LockTest1) {
    // Test locking a mutex created on stack using a special class util::Lock.
    LOGS_INFO("LockTest1 begins");

    // The mutex won't be locked by anyone
    Mutex mtx1;
    BOOST_CHECK(not mtx1.lockedByThread());

    // The mutex will be locked by the current thread
    Mutex mtx2;
    {
        // Do this in a nested block to ensure that lock object
        // gets destructed before the mutex.
        Lock const lock(mtx2, "LockTes1t: main thread");
        BOOST_CHECK(mtx2.lockedByThread());
    }
    LOGS_INFO(!mtx2.lockedByThread());

    // Lock this mutex in each of two separate threads. Let each thread
    // to wait for a random period of time within some interval before
    // grabbing a lock. This would ensure threads would attempt locking
    // at random order.
    //
    // Note the wait interval for each thread is a random
    // number of milliseconds within the same interval of time.
    // The average run time of the test is the average wait time
    // multiplied by the number of iterations of the loop.
    for (int i = 0; i < 100; ++i) {
        Mutex mtx;
        thread thr1([&mtx]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            Lock const lock(mtx, "LockTest1: thread 1");
        });
        thread thr2([&mtx]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            Lock const lock(mtx, "LockTest1: thread 2");
        });
        BOOST_CHECK(!mtx.lockedByThread());
        thr1.join();
        thr2.join();
    }
    // Test the correctness of the Mutex implementation by using a non-atomic
    // counter to be incremented after acquiring a lock.
    {
        Mutex mtx;
        unsigned int counter = 0;
        unsigned int const steps = 1024;
        unsigned int const numThreads = min(2U, thread::hardware_concurrency());
        vector<unique_ptr<thread>> threads(numThreads);
        for (auto&& t : threads) {
            t = make_unique<thread>([&mtx, &counter]() {
                for (unsigned int i = 0; i < steps; ++i) {
                    Lock const lock(mtx, "LockTest1: thread " + thisThreadId2str());
                    ++counter;
                }
            });
        }
        for (auto&& t : threads) {
            t->join();
        }
        BOOST_CHECK_EQUAL(counter, steps * numThreads);
    }
    LOGS_INFO("LockTest1 ends");
}

BOOST_AUTO_TEST_CASE(LockTest2) {
    // Test locking a mutex created in dynamic memory and owned by
    // a shared pointer using a special class util::Lock. The test implements
    // the same testing algorithm as the previous test, except it will be testing
    // a different way of constructing the lock.
    LOGS_INFO("LockTest2 begins");

    // The mutex won't be locked by anyone
    shared_ptr<Mutex> const mtx1 = make_shared<Mutex>();
    BOOST_CHECK(!mtx1->lockedByThread());

    // The mutex will be locked by the current thread
    shared_ptr<Mutex> const mtx2 = make_shared<Mutex>();
    {
        // Do this in a nested block to ensure that lock object
        // gets destructed before the mutex.
        Lock const lock(mtx2, "LockTes1t: main thread");
        BOOST_CHECK(mtx2->lockedByThread());
    }
    BOOST_CHECK(!mtx2->lockedByThread());

    // Lock this mutex in each of two separate threads. Let each thread
    // to wait for a random period of time within some interval before
    // grabbing a lock. This would ensure threads would attempt locking
    // at random order.
    //
    // Note the wait interval for each thread is a random
    // number of milliseconds within the same interval of time.
    // The average run time of the test is the average wait time
    // multiplied by the number of iterations of the loop.
    for (int i = 0; i < 100; ++i) {
        shared_ptr<Mutex> const mtx = make_shared<Mutex>();
        thread thr1([mtx]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            Lock const lock(mtx, "LockTest1: thread 1");
        });
        thread thr2([mtx]() {
            BlockPost blockPost(10, 20);
            blockPost.wait();
            Lock const lock(mtx, "LockTest1: thread 2");
        });
        BOOST_CHECK(!mtx->lockedByThread());
        thr1.join();
        thr2.join();
    }
    // Test the correctness of the Mutex implementation by using a non-atomic
    // counter to be incremented after acquiring a lock.
    {
        shared_ptr<Mutex> const mtx = make_shared<Mutex>();
        unsigned int counter = 0;
        unsigned int const steps = 1024;
        unsigned int const numThreads = min(2U, thread::hardware_concurrency());
        vector<unique_ptr<thread>> threads(numThreads);
        for (auto&& t : threads) {
            t = make_unique<thread>([mtx, &counter]() {
                for (unsigned int i = 0; i < steps; ++i) {
                    Lock const lock(mtx, "LockTest2: thread " + thisThreadId2str());
                    ++counter;
                }
            });
        }
        for (auto&& t : threads) {
            t->join();
        }
        BOOST_CHECK_EQUAL(counter, steps * numThreads);
    }
    LOGS_INFO("LockTest2 ends");
}

BOOST_AUTO_TEST_SUITE_END()
