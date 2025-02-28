// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 AURA/LSST.
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

// System headers
#include <string>
#include <unistd.h>

// Third-party headers
#include "boost/asio.hpp"

// Boost unit test header
#define BOOST_TEST_MODULE Qdisp_1
#include <boost/test/unit_test.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/MergingHandler.h"
#include "global/ResourceUnit.h"
#include "qdisp/CzarStats.h"
#include "qdisp/Executive.h"
#include "qdisp/JobQuery.h"
#include "qmeta/MessageStore.h"
#include "qproc/ChunkQuerySpec.h"
#include "util/QdispPool.h"
#include "util/threadSafe.h"

namespace test = boost::test_tools;
using namespace lsst::qserv;
using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.testQDisp");
}

typedef util::Sequential<int> SequentialInt;
typedef vector<qdisp::ResponseHandler::Ptr> RequesterVector;

namespace lsst::qserv::qdisp {

class ExecutiveUT;

class TestInfo : public ResponseHandler {
public:
    using Ptr = std::shared_ptr<TestInfo>;

    TestInfo() {}
    virtual ~TestInfo() {}

    bool goWait() {
        unique_lock ulock(_infoMtx);
        _infoCV.wait(ulock, [this]() { return _go == true; });
        return _ok;
    }

    void setGo(bool val) {
        lock_guard lg(_infoMtx);
        _go = val;
        _infoCV.notify_all();
    }

    // virtual function that won't be needed
    std::tuple<bool, bool> flushHttp(std::string const& fileUrl, uint64_t expectedRows,
                                     uint64_t& resultRows) override {
        return {true, false};
    }
    void flushHttpError(int errorCode, std::string const& errorMsg, int status) override {}
    void errorFlush(std::string const& msg, int code) override {};
    Error getError() const override { return util::Error(); }
    void processCancel() override {};
    void prepScrubResults(int jobId, int attempt) override {};

    /// Print a string representation of the receiver to an ostream
    std::ostream& print(std::ostream& os) const override {
        os << "TestInfo ujCount=" << ujCount;
        return os;
    }

    atomic<int> ujCount = 0;

private:
    bool _ok = true;
    bool _go = true;
    mutex _infoMtx;
    condition_variable _infoCV;
};

/// Version of UberJob specifically for this unit test.
class UberJobUT : public UberJob {
public:
    using PtrUT = std::shared_ptr<UberJobUT>;

    UberJobUT(std::shared_ptr<Executive> const& executive,
              std::shared_ptr<ResponseHandler> const& respHandler, int queryId, int uberJobId,
              qmeta::CzarId czarId, int rowLimit, czar::CzarChunkMap::WorkerChunksData::Ptr const& workerData,
              TestInfo::Ptr const& testInfo_)
            : UberJob(executive, respHandler, queryId, uberJobId, czarId, rowLimit, workerData),
              testInfo(testInfo_) {}

    void runUberJob() override {
        LOGS(_log, LOG_LVL_INFO, "runUberJob() chunkId=" << chunkId);
        bool ok = testInfo->goWait();
        int c = -1;
        if (ok) {
            c = ++(testInfo->ujCount);
        }
        callMarkCompleteFunc(ok);
        LOGS(_log, LOG_LVL_INFO, "runUberJob() end chunkId=" << chunkId << " c=" << c);
    }

    TestInfo::Ptr testInfo;
    int chunkId = -1;
};

/// Version of Executive specifically for this unit test.
class ExecutiveUT : public Executive {
public:
    using PtrUT = shared_ptr<ExecutiveUT>;

    ~ExecutiveUT() override = default;

    ExecutiveUT(int qmetaTimeBetweenUpdates, shared_ptr<qmeta::MessageStore> const& ms,
                util::QdispPool::Ptr const& qdispPool, shared_ptr<qmeta::QStatus> const& qStatus,
                shared_ptr<qproc::QuerySession> const& querySession, TestInfo::Ptr const& testInfo_)
            : Executive(qmetaTimeBetweenUpdates, ms, qdispPool, qStatus, querySession), testInfo(testInfo_) {}

    void assignJobsToUberJobs() override {
        vector<qdisp::UberJob::Ptr> ujVect;

        // Make an UberJobUnitTest for each job
        qdisp::Executive::ChunkIdJobMapType unassignedChunks = unassignedChunksInQuery();
        for (auto const& [chunkId, jqPtr] : unassignedChunks) {
            auto exec = shared_from_this();
            PtrUT execUT = dynamic_pointer_cast<ExecutiveUT>(exec);
            auto uJob = UberJobUT::PtrUT(new UberJobUT(execUT, testInfo, getId(), ujId++, czarId, rowLimit,
                                                       targetWorker, testInfo));
            uJob->chunkId = chunkId;
            uJob->addJob(jqPtr);
            ujVect.push_back(uJob);
        }

        for (auto const& ujPtr : ujVect) {
            addAndQueueUberJob(ujPtr);
        }
        LOGS(_log, LOG_LVL_INFO, "assignJobsToUberJobs() end");
    }

    CzarIdType czarId = 1;
    UberJobId ujId = 1;
    int rowLimit = 0;
    czar::CzarChunkMap::WorkerChunksData::Ptr targetWorker = nullptr;

    TestInfo::Ptr testInfo;
};

}  // namespace lsst::qserv::qdisp

qdisp::JobDescription::Ptr makeMockJobDescription(qdisp::Executive::Ptr const& ex, int sequence,
                                                  ResourceUnit const& ru, std::string msg,
                                                  std::shared_ptr<qdisp::ResponseHandler> const& mHandler) {
    auto cqs = std::make_shared<qproc::ChunkQuerySpec>();  // dummy, unused in this case.
    std::string chunkResultName = "dummyResultTableName";
    qmeta::CzarId const czarId = 1;
    auto job = qdisp::JobDescription::create(czarId, ex->getId(), sequence, ru, mHandler, cqs,
                                             chunkResultName, true);
    return job;
}

// Add mock requests to an executive corresponding to the requesters. Note
// that we return a shared pointer to the last constructed JobQuery object.
// This only makes sense for single query jobs.
//

std::shared_ptr<qdisp::JobQuery> addMockRequests(qdisp::Executive::Ptr const& ex, SequentialInt& sequence,
                                                 int startingChunkId, std::string msg, RequesterVector& rv) {
    std::shared_ptr<qdisp::JobQuery> jobQuery;
    int copies = rv.size();
    for (int j = 0; j < copies; ++j) {
        ResourceUnit ru;
        int chunkId = startingChunkId + j;
        ru.setAsDbChunk("Mock", chunkId);
        // The job copies the JobDescription.
        qdisp::JobDescription::Ptr job = makeMockJobDescription(ex, sequence.incr(), ru, msg, rv[j]);
        jobQuery = ex->add(job);
    }
    ex->setAllJobsCreated();
    return jobQuery;
}

std::shared_ptr<qdisp::JobQuery> executiveTest(qdisp::ExecutiveUT::PtrUT const& ex, SequentialInt& sequence,
                                               int chunkId, std::string msg, int copies) {
    LOGS(_log, LOG_LVL_INFO, "executiveTest start");
    // Test class Executive::add
    // Modeled after ccontrol::UserQuery::submit()
    ResourceUnit ru;
    std::string chunkResultName = "mock";
    std::shared_ptr<rproc::InfileMerger> infileMerger;
    ccontrol::MergingHandler::Ptr mh =
            std::make_shared<ccontrol::MergingHandler>(infileMerger, chunkResultName);
    RequesterVector rv;
    for (int j = 0; j < copies; ++j) {
        rv.push_back(mh);
    }
    auto ret = addMockRequests(ex, sequence, chunkId, msg, rv);
    ex->assignJobsToUberJobs();
    LOGS(_log, LOG_LVL_INFO, "executiveTest end");
    return ret;
}

/** This function is run in a separate thread to fail the test if it takes too long
 * for the jobs to complete.
 */
void timeoutFunc(std::atomic<bool>& flagDone, int millisecs) {
    LOGS_INFO("timeoutFunc");
    int total = 0;
    bool done = flagDone;
    int maxTime = millisecs * 1000;
    while (!done && total < maxTime) {
        int sleepTime = 1000000;
        total += sleepTime;
        usleep(sleepTime);
        done = flagDone;
        LOGS_INFO("timeoutFunc done=" << done << " total=" << total);
    }
    LOGS_ERROR("timeoutFunc done=" << done << " total=" << total << " timedOut=" << (total >= maxTime));
    BOOST_REQUIRE(done == true);
}

// The following sets up the environment to do a test and is modeled after
// ccontrol::UserQuery::submit() (note that we cannot reuse an executive).
//
class SetupTest {
public:
    std::string qrMsg;
    std::shared_ptr<qmeta::MessageStore> ms;
    util::QdispPool::Ptr qdispPool;
    qdisp::ExecutiveUT::PtrUT ex;
    std::shared_ptr<qdisp::JobQuery> jqTest;  // used only when needed
    qdisp::TestInfo::Ptr testInfo = qdisp::TestInfo::Ptr(new qdisp::TestInfo());

    SetupTest(const char* request, util::QdispPool::Ptr const& qPool_) : qdispPool(qPool_) {
        LOGS(_log, LOG_LVL_INFO, "SetupTest start");
        qrMsg = request;
        ms = std::make_shared<qmeta::MessageStore>();
        auto tInfo = qdisp::TestInfo::Ptr(new qdisp::TestInfo());
        std::shared_ptr<qmeta::QStatus> qStatus;  // No updating QStatus, nullptr
        ex = qdisp::ExecutiveUT::PtrUT(new qdisp::ExecutiveUT(60, ms, qdispPool, qStatus, nullptr, testInfo));
        LOGS(_log, LOG_LVL_INFO, "SetupTest end");
    }
    ~SetupTest() {}
};

BOOST_AUTO_TEST_SUITE(Suite)

// Variables for all subsequent tests. Note that all tests verify that the
// resource object for all chuncks has been properly constructed. We use
// the same chunkID for all tests (see setRName() below).
//
int chunkId = 1234;
int millisInt = 50000;

util::QdispPool::Ptr globalQdispPool;
qdisp::CzarStats::Ptr globalCzarStats;

BOOST_AUTO_TEST_CASE(Executive) {
    int qPoolSize = 1000;
    int maxPriority = 2;
    vector<int> vectRunSizes = {50, 50, 50, 50};
    vector<int> vectMinRunningSizes = {0, 1, 3, 3};
    globalQdispPool = util::QdispPool::Ptr(
            new util::QdispPool(qPoolSize, maxPriority, vectRunSizes, vectMinRunningSizes));
    qdisp::CzarStats::setup(globalQdispPool);
    globalCzarStats = qdisp::CzarStats::get();

    // Variables for all executive sub-tests. Note that all executive tests
    // are full roundtrip tests. So, if these succeed then it's likely all
    // other query tests will succeed. So, much of this is redundant.
    //
    std::atomic<bool> done(false);
    int jobs = 0;
    _log.setLevel(LOG_LVL_DEBUG);  // Ugly but boost test suite forces this
    std::thread timeoutT(&timeoutFunc, std::ref(done), millisInt);

    // Test single instance
    {
        LOGS_INFO("Executive single query test");
        SetupTest tEnv("respdata", globalQdispPool);
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        jobs = 1;
        LOGS_INFO("jobs=1");
        tEnv.ex->join();
        LOGS_INFO("Executive single query test checking");
        BOOST_CHECK(tEnv.jqTest->getStatus()->getInfo().state == qmeta::JobStatus::COMPLETE);
        BOOST_CHECK(tEnv.ex->getEmpty() == true);
    }

    // Test 4 jobs
    {
        LOGS_INFO("Executive four parallel jobs test");
        SetupTest tEnv("respdata", globalQdispPool);
        SequentialInt sequence(0);
        executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 4);
        jobs += 4;
        LOGS_INFO("ex->joining()");
        tEnv.ex->join();
        LOGS_INFO("Executive four parallel jobs test checking");
        BOOST_CHECK(tEnv.ex->getEmpty() == true);
    }

    // Test that we can detect ex._empty == false.
    {
        LOGS_INFO("Executive detect non-empty job queue test");
        SetupTest tEnv("respdata", globalQdispPool);
        SequentialInt sequence(0);
        executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 5);
        jobs += 5;

        BOOST_CHECK(tEnv.ex->getEmpty() == false);
        LOGS_INFO("ex->joining()");
        tEnv.ex->join();
        LOGS_INFO("ex->join() joined");
        BOOST_CHECK(tEnv.ex->getEmpty() == true);
    }
    done = true;
    timeoutT.join();
    LOGS_INFO("Executive test end");
}

BOOST_AUTO_TEST_CASE(MessageStore) {
    LOGS_INFO("MessageStore test start");
    qmeta::MessageStore ms;
    BOOST_CHECK(ms.messageCount() == 0);
    ms.addMessage(123, "EXECUTIVE", 456, "test1");
    std::string str("test2");
    ms.addMessage(124, "EXECUTIVE", -12, str);
    ms.addMessage(86, "EXECUTIVE", -12, "test3");
    BOOST_CHECK(ms.messageCount() == 3);
    BOOST_CHECK(ms.messageCount(-12) == 2);
    qmeta::QueryMessage qm = ms.getMessage(1);
    BOOST_CHECK(qm.chunkId == 124 && qm.code == -12 && str.compare(qm.description) == 0);
    LOGS_INFO("MessageStore test end");
}

BOOST_AUTO_TEST_CASE(ExecutiveCancel) {
    // Test that aJobQuery can be cancelled and ends in correct state
    //
    {
        LOGS_INFO("ExecutiveCancel: squash it test");
        SetupTest tEnv("respdata", globalQdispPool);
        tEnv.testInfo->setGo(false);  // Can't let jobs run or they are untracked before
        // squash
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        tEnv.ex->squash("test");
        usleep(250000);  // Give mock threads a quarter second to complete.
        tEnv.ex->join();
        BOOST_CHECK(tEnv.jqTest->isQueryCancelled() == true);
    }

    // Test that multiple JobQueries are cancelled.
    {
        LOGS_INFO("ExecutiveCancel: squash 20 test");
        SetupTest tEnv("respdata", globalQdispPool);
        // squash
        SequentialInt sequence(0);
        executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 20);
        tEnv.ex->squash("test");
        tEnv.ex->squash("test");  // check that squashing twice doesn't cause issues.
        usleep(250000);           // Give mock threads a quarter second to complete.
        tEnv.ex->join();
    }
}

BOOST_AUTO_TEST_SUITE_END()
