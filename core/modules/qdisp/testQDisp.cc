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

// Boost unit test header
#define BOOST_TEST_MODULE Qdisp_1
#include "boost/test/included/unit_test.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/MergingHandler.h"
#include "global/ResourceUnit.h"
#include "global/MsgReceiver.h"
#include "qdisp/Executive.h"
#include "qdisp/JobQuery.h"
#include "qdisp/MessageStore.h"
#include "qdisp/QueryRequest.h"
#include "qdisp/XrdSsiMocks.h"
#include "qproc/ChunkQuerySpec.h"
#include "qproc/TaskMsgFactory.h"
#include "util/threadSafe.h"

namespace test = boost::test_tools;
using namespace lsst::qserv;

namespace {
  LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.testQDisp");
}

typedef util::Sequential<int> SequentialInt;
typedef std::vector<qdisp::ResponseHandler::Ptr> RequesterVector;

class ChunkMsgReceiverMock : public MsgReceiver {
public:
    virtual void operator()(int code, std::string const& msg) {
        LOGS_DEBUG("Mock::operator() mockId=" << _mockId
                   << ", code=" << code << ", msg=" << msg);
    }
    static std::shared_ptr<ChunkMsgReceiverMock> newInstance(int mockId) {
        std::shared_ptr<ChunkMsgReceiverMock> cmr = std::make_shared<ChunkMsgReceiverMock>();
        cmr->_mockId = mockId;
        return cmr;
    }
    int _mockId;
};

namespace lsst {
namespace qserv {
namespace qproc {


// Normally, there's one TaskMsgFactory that all jobs in a user query share.
// In this case, there's one MockTaskMsgFactory per job with a payload specific
// for that job.
class MockTaskMsgFactory : public TaskMsgFactory {
public:
    MockTaskMsgFactory(std::string const& mockPayload_)
        : TaskMsgFactory(0), mockPayload(mockPayload_) {}
    void serializeMsg(ChunkQuerySpec const& s,
                      std::string const& chunkResultName,
                      QueryId queryId, int jobId, int attemptCount, qmeta::CzarId czarId,
                      std::ostream& os) override {
        os << mockPayload;
    }
    std::string mockPayload;
};

}}} // namespace lsst::qserv::qproc


qdisp::JobDescription::Ptr makeMockJobDescription(qdisp::Executive::Ptr const& ex, int sequence,
                                                  ResourceUnit const& ru, std::string msg,
                                                  std::shared_ptr<qdisp::ResponseHandler> const& mHandler) {
    auto mockTaskMsgFactory = std::make_shared<qproc::MockTaskMsgFactory>(msg);
    auto cqs = std::make_shared<qproc::ChunkQuerySpec>(); // dummy, unused in this case.
    std::string chunkResultName = "dummyResultTableName";
    qmeta::CzarId const czarId = 1;
    auto job = qdisp::JobDescription::create(czarId, ex->getId(), sequence, ru, mHandler,
                                             mockTaskMsgFactory, cqs, chunkResultName, true);
    return job;
}


// Add mock requests to an executive corresponding to the requesters. Note
// that we return a shared pointer to the last constructed JobQuery object.
// This only makes sense for single query jobs.
//
std::shared_ptr<qdisp::JobQuery> addMockRequests(
                                 qdisp::Executive::Ptr const& ex,
                                 SequentialInt &sequence, int chunkID,
                                 std::string msg, RequesterVector& rv) {
    std::shared_ptr<qdisp::JobQuery> jobQuery;
    int copies = rv.size();
    for(int j=0; j < copies; ++j) {
        ResourceUnit ru;
        ru.setAsDbChunk("Mock", chunkID++);

        // The job copies the JobDescription.
        qdisp::JobDescription::Ptr job = makeMockJobDescription(
                                             ex, sequence.incr(), ru,
                                             msg, rv[j]);
        jobQuery = ex->add(job);
        jobQuery->runJob();
    }
    return jobQuery;
}

/** Start adds 'copies' number of test requests that each sleep for 'millisecs' time
 * before signaling to 'ex' that they are done.
 * Returns time to complete in seconds.
 */
std::shared_ptr<qdisp::JobQuery> executiveTest(
                                 qdisp::Executive::Ptr const& ex,
                                 SequentialInt &sequence, int chunkId,
                                 std::string msg, int copies) {
    // Test class Executive::add
    // Modeled after ccontrol::UserQuery::submit()
    ResourceUnit ru;
    std::string chunkResultName = "mock";
    std::shared_ptr<rproc::InfileMerger> infileMerger;
    std::shared_ptr<ChunkMsgReceiverMock> cmr = ChunkMsgReceiverMock::newInstance(chunkId);
    ccontrol::MergingHandler::Ptr mh = std::make_shared<ccontrol::MergingHandler>(cmr, infileMerger, chunkResultName);
    RequesterVector rv;
    for (int j=0; j < copies; ++j) {
        rv.push_back(mh);
    }
    return addMockRequests(ex, sequence, chunkId, msg, rv);
}


/** This function is run in a separate thread to fail the test if it takes too long
 * for the jobs to complete.
 */
void timeoutFunc(std::atomic<bool>& flagDone, int millisecs) {
    LOGS_DEBUG("timeoutFunc");
    int total = 0;
    bool done = flagDone;
    int maxTime = millisecs*1000;
    while (!done && total < maxTime) {
        int sleepTime = 1000000;
        total += sleepTime;
        usleep(sleepTime);
        done = flagDone;
        LOGS_DEBUG("timeoutFunc done=" << done << " total=" << total);
    }
    LOGS_ERROR("timeoutFunc done=" << done << " total=" << total <<
               " timedOut=" << (total >= maxTime));
    BOOST_REQUIRE(done == true);
}

// The following sets up the environment to do a test and is modeled after
// ccontrol::UserQuery::submit() (note that we cannot reuse an executive).
//
class SetupTest {
    public:
    std::string qrMsg;
    std::string str;
    qdisp::ExecutiveConfig::Ptr conf;
    std::shared_ptr<qdisp::MessageStore> ms;
    qdisp::QdispPool::Ptr qdispPool;
    qdisp::Executive::Ptr ex;
    std::shared_ptr<qdisp::JobQuery> jqTest; // used only when needed

    SetupTest(const char *request) {
         qrMsg = request;
         qdisp::XrdSsiServiceMock::Reset();
         str = qdisp::ExecutiveConfig::getMockStr();
         conf = std::make_shared<qdisp::ExecutiveConfig>(str, 0); // No updating of QMeta.
         ms = std::make_shared<qdisp::MessageStore>();
         qdispPool = std::make_shared<qdisp::QdispPool>(true);
         std::shared_ptr<qmeta::QStatus> qStatus; // No updating QStatus, nullptr
         ex = qdisp::Executive::create(*conf, ms, qdispPool, qStatus);
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

BOOST_AUTO_TEST_CASE(Executive) {
    // Variables for all executive sub-tests. Note that all executive tests
    // are full roundtrip tests. So, if these succeed then it's likely all
    // other query tests will succeed. So, much of this is redundant.
    //
    std::atomic<bool> done(false);
    int jobs = 0;
    _log.setLevel(LOG_LVL_DEBUG); // Ugly but boost test suite forces this
    std::thread timeoutT(&timeoutFunc, std::ref(done), millisInt);
    //qdisp::XrdSsiServiceMock::setRName("/chk/Mock/1234");
    qdisp::XrdSsiServiceMock::setRName("/chk/Mock/");

    // Test single instance
    {
        LOGS_DEBUG("Executive single query test");
        SetupTest tEnv("respdata");
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        jobs = 1;
        LOGS_DEBUG("jobs=1");
        tEnv.ex->join();
        LOGS_DEBUG("Executive single query test checking");
        BOOST_CHECK(tEnv.jqTest->getStatus()->getInfo().state ==
                    qdisp::JobStatus::COMPLETE);
        BOOST_CHECK(tEnv.ex->getEmpty() == true);
    }

    // Test 4 jobs
    {
        LOGS_DEBUG("Executive four parallel jobs test");
        SetupTest tEnv("respdata");
        SequentialInt sequence(0);
        executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 4);
        jobs += 4;
        LOGS_DEBUG("ex->joining()");
        tEnv.ex->join();
        LOGS_DEBUG("Executive four parallel jobs test checking");
        BOOST_CHECK(tEnv.ex->getEmpty() == true);
    }

    // Test that we can detect ex._empty == false.
    {
        LOGS_DEBUG("Executive detect non-empty job queue test");
        SetupTest tEnv("respdata");
        SequentialInt sequence(0);
        qdisp::XrdSsiServiceMock::setGo(false);
        executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 5);
        jobs += 5;
        while (qdisp::XrdSsiServiceMock::getCount() < jobs) {
            LOGS_DEBUG("waiting for _count(" << qdisp::XrdSsiServiceMock::getCount()
                       << ") == jobs(" << jobs << ")");
            usleep(10000);
        }

        BOOST_CHECK(tEnv.ex->getEmpty() == false);
        qdisp::XrdSsiServiceMock::setGo(true);
        LOGS_DEBUG("ex->joining()");
        tEnv.ex->join();
        LOGS_DEBUG("ex->join() joined");
        BOOST_CHECK(tEnv.ex->getEmpty() == true);
    }
    done = true;
    timeoutT.join();
    LOGS_DEBUG("Executive test end");
}

BOOST_AUTO_TEST_CASE(MessageStore) {
    LOGS_DEBUG("MessageStore test start");
    qdisp::MessageStore ms;
    BOOST_CHECK(ms.messageCount() == 0);
    ms.addMessage(123, 456, "test1");
    std::string str("test2");
    ms.addMessage(124, -12, str);
    ms.addMessage(86, -12, "test3");
    BOOST_CHECK(ms.messageCount() == 3);
    BOOST_CHECK(ms.messageCount(-12) == 2);
    qdisp::QueryMessage qm = ms.getMessage(1);
    BOOST_CHECK(qm.chunkId == 124 && qm.code == -12 && str.compare(qm.description) == 0);
    LOGS_DEBUG("MessageStore test end");
}

BOOST_AUTO_TEST_CASE(QueryRequest) {
    {
        LOGS_DEBUG("QueryRequest error retry test");
        // Setup Executive and for retry test when receiving an error
        // Note executive maps RESPONSE_ERROR to RESULT_ERROR
        SetupTest tEnv("resperror");
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        tEnv.ex->join();
        BOOST_CHECK(tEnv.jqTest->getStatus()->getInfo().state == qdisp::JobStatus::RESULT_ERROR);
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getFinCount() > 1); // Retried, eh?
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getFinCount() ==
                    qdisp::XrdSsiServiceMock::getReqCount());
    }

    {
        LOGS_DEBUG("QueryRequest error noretry test 2");
        // Setup Executive and for no retry test when receiving an error
        // Note executive maps RESPONSE_ERROR to RESULT_ERROR
        SetupTest tEnv("resperrnr");
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        tEnv.ex->join();
        BOOST_CHECK(tEnv.jqTest->getStatus()->getInfo().state == qdisp::JobStatus::RESULT_ERROR);
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getFinCount() == 1);
    }

    {
        LOGS_DEBUG("QueryRequest stream with data error test");
        // Setup Executive and for no retry test when receiving an error
        // Note executive maps RESPONSE_DATA_NACK to RESULT_ERROR
        SetupTest tEnv("respstrerr");
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        tEnv.ex->join();
        LOGS_DEBUG("tEnv.jqTest->...state = " << tEnv.jqTest->getStatus()->getInfo().state);
        BOOST_CHECK(tEnv.jqTest->getStatus()->getInfo().state == qdisp::JobStatus::RESULT_ERROR);
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getFinCount() == 1); // No retries!
    }

    // We wish we could do the stream response with no results test but the
    // needed information is too complex to figure out (well, one day we will).
    // So, we've commented this out but the framework exists modulo the needed
    // responses (see XrdSsiMocks::Agent). So, this gets punted into the
    // integration test (too bad).
/*
    {
        LOGS_DEBUG("QueryRequest stream with no results test");
        SetupTest tEnv("respstream");
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        tEnv.ex->join();
        BOOST_CHECK(tEnv.jqTest->getStatus()->getInfo().state ==
                    qdisp::JobStatus::COMPLETE);
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getFinCount() == 1);
    }
*/
    LOGS_DEBUG("QueryRequest test end");
}

BOOST_AUTO_TEST_CASE(ExecutiveCancel) {
    // Test that aJobQuery can be cancelled and ends in correct state
    //
    {
        LOGS_DEBUG("ExecutiveCancel: squash it test");
        SetupTest tEnv("respdata");
        qdisp::XrdSsiServiceMock::setGo(false); // Can't let jobs run or they are untracked before squash
        SequentialInt sequence(0);
        tEnv.jqTest = executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 1);
        tEnv.ex->squash();
        qdisp::XrdSsiServiceMock::setGo(true);
        usleep(250000); // Give mock threads a quarter second to complete.
        tEnv.ex->join();
        BOOST_CHECK(tEnv.jqTest->isQueryCancelled() == true);
        // Note that the query might not have actually called ProcessRequest()
        // but if it did, then it must have called Finished() with cancel.
        //
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getCanCount() ==
                    qdisp::XrdSsiServiceMock::getReqCount());
    }

    // Test that multiple JobQueries are cancelled.
    {
        LOGS_DEBUG("ExecutiveCancel: squash 20 test");
        SetupTest tEnv("respdata");
        qdisp::XrdSsiServiceMock::setGo(false); // Can't let jobs run or they are untracked before squash
        SequentialInt sequence(0);
        executiveTest(tEnv.ex, sequence, chunkId, tEnv.qrMsg, 20);
        tEnv.ex->squash();
        tEnv.ex->squash(); // check that squashing twice doesn't cause issues.
        qdisp::XrdSsiServiceMock::setGo(true);
        usleep(250000); // Give mock threads a quarter second to complete.
        tEnv.ex->join();
        // Note that the cancel count might not be 20 as some queries will cancel
        // themselves before they get around to issuing ProcessRequest().
        //
        BOOST_CHECK(qdisp::XrdSsiServiceMock::getCanCount() ==
                    qdisp::XrdSsiServiceMock::getReqCount());
    }
}

BOOST_AUTO_TEST_CASE(ServiceMock) {
    // Verify that our service object did not see anything unusual.
    BOOST_CHECK(qdisp::XrdSsiServiceMock::isAOK());
}

BOOST_AUTO_TEST_SUITE_END()
