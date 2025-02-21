// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2016 LSST Corporation.
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
 *
 * @brief Simple testing for class FifoScheduler
 *
 * @author Daniel L. Wang, SLAC
 */

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "memman/MemManNone.h"
#include "mysql/MySqlConfig.h"
#include "proto/worker.pb.h"
#include "protojson/ScanTableInfo.h"
#include "util/Command.h"
#include "util/EventThread.h"
#include "wbase/FileChannelShared.h"
#include "wbase/Task.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/SqlConnMgr.h"
#include "wpublish/QueriesAndChunks.h"
#include "wsched/ChunkTasksQueue.h"
#include "wsched/BlendScheduler.h"
#include "wsched/FifoScheduler.h"
#include "wsched/GroupScheduler.h"
#include "wsched/ScanScheduler.h"

// Boost unit test header
#define BOOST_TEST_MODULE WorkerScheduler
#include "boost/test/unit_test.hpp"

namespace test = boost::test_tools;
namespace wsched = lsst::qserv::wsched;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wsched.testSchedulers");
}

using namespace std;
using lsst::qserv::mysql::MySqlConfig;
using lsst::qserv::proto::TaskMsg;
using lsst::qserv::wbase::FileChannelShared;
using lsst::qserv::wbase::SendChannel;
using lsst::qserv::wbase::Task;
using lsst::qserv::wconfig::WorkerConfig;
using lsst::qserv::wcontrol::SqlConnMgr;
using lsst::qserv::wdb::ChunkResourceMgr;
using lsst::qserv::wpublish::QueriesAndChunks;

double const oneHr = 60.0;

bool const resetForTestingC = true;
int const maxBootedC = 5;
int const maxDarkTasksC = 25;

shared_ptr<ChunkResourceMgr> crm;  // not used in this test, required by Task::createTasks
MySqlConfig mySqlConfig;           // not used in this test, required by Task::createTasks
SqlConnMgr::Ptr sqlConnMgr;        // not used in this test, required by Task::createTasks

auto workerCfg = lsst::qserv::wconfig::WorkerConfig::create();

std::vector<FileChannelShared::Ptr> locSendSharedPtrs;

struct SchedulerFixture {
    typedef std::shared_ptr<TaskMsg> TaskMsgPtr;

    SchedulerFixture(void) { counter = 20; }
    ~SchedulerFixture(void) {}

    /* &&& Instead of using messages, make a Task::createTasksForUnitTest() function
    void addSomeFragments(TaskMsgPtr const& t, int numberOfFragments) {
        for (int i = 0; i < numberOfFragments; ++i) {
            TaskMsg::Fragment* f = t->add_fragment();
            f->add_query("Hello, this is a query.");
            f->mutable_subchunks()->add_id(100 + i);
            f->set_resulttable("r_341");
        }
    }


    TaskMsgPtr newTaskMsg(int seq, lsst::qserv::QueryId qId, int jobId) {
        TaskMsgPtr t = std::make_shared<TaskMsg>();
        t->set_queryid(qId);
        t->set_jobid(jobId);
        t->set_chunkid(seq);
        t->set_czarid(1);
        t->set_db("elephant");
        //&&&addSomeFragments(t, 3);
        t->set_scaninteractive(false);
        t->set_attemptcount(0);
        ++counter;
        return t;
    }

    TaskMsgPtr newTaskMsgSimple(int seq, lsst::qserv::QueryId qId, int jobId) {
        TaskMsgPtr t = std::make_shared<TaskMsg>();
        t->set_queryid(qId);
        t->set_jobid(jobId);
        t->set_chunkid(seq);
        t->set_czarid(1);
        t->set_db("moose");
        t->set_scaninteractive(false);
        t->set_attemptcount(0);
        //&&&addSomeFragments(t, 1);
        ++counter;
        return t;
    }

    TaskMsgPtr newTaskMsgScan(int seq, int priority, lsst::qserv::QueryId qId, int jobId,
                              std::string const& tableName = "whatever") {
        auto taskMsg = newTaskMsg(seq, qId, jobId);
        taskMsg->set_scanpriority(priority);
        auto sTbl = taskMsg->add_scantable();
        sTbl->set_db("elephant");
        sTbl->set_table(tableName);
        sTbl->set_scanrating(priority);
        sTbl->set_lockinmemory(true);
        return taskMsg;
    }

    Task::Ptr queMsgWithChunkId(wsched::GroupScheduler& gs, int chunkId, lsst::qserv::QueryId qId, int jobId,
                                shared_ptr<QueriesAndChunks> const& queries) {
        Task::Ptr t = makeTask(newTaskMsg(chunkId, qId, jobId), queries);
        gs.queCmd(t);
        return t;
    }
    */

    int counter;
};

BOOST_FIXTURE_TEST_SUITE(SchedulerSuite, SchedulerFixture)

struct SchedFixture {
    SchedFixture(double maxScanTimeFast, bool examinAllSleep)
            : _maxScanTimeFast{maxScanTimeFast}, _examineAllSleep{examinAllSleep} {
        setupQueriesBlend();
    }
    ~SchedFixture() {}

    void setupQueriesBlend() {
        queries = lsst::qserv::wpublish::QueriesAndChunks::setupGlobal(
                std::chrono::seconds(1), std::chrono::seconds(_examineAllSleep), maxBootedC, maxDarkTasksC,
                resetForTestingC);
        blend = std::make_shared<wsched::BlendScheduler>("blendSched", queries, maxThreads, group, scanSlow,
                                                         scanSchedulers);
        group->setDefaultPosition(0);
        scanFast->setDefaultPosition(1);
        scanMed->setDefaultPosition(2);
        scanSlow->setDefaultPosition(3);
        queries->setBlendScheduler(blend);
        queries->setRequiredTasksCompleted(1);  // Make it easy to set a baseline.
    }

    int const fastest = lsst::qserv::protojson::ScanInfo::Rating::FASTEST;
    int const fast = lsst::qserv::protojson::ScanInfo::Rating::FAST;
    int const medium = lsst::qserv::protojson::ScanInfo::Rating::MEDIUM;
    int const slow = lsst::qserv::protojson::ScanInfo::Rating::SLOW;

    lsst::qserv::QueryId qIdInc{1};

    int maxThreads{9};
    int maxActiveChunks{20};
    int priority{2};

private:
    double _maxScanTimeFast{oneHr};  ///< Don't hit time limit in tests.
    int _examineAllSleep{0};         ///< Don't run _examineThread when 0

public:
    lsst::qserv::memman::MemManNone::Ptr memMan{std::make_shared<lsst::qserv::memman::MemManNone>(1, true)};
    wsched::GroupScheduler::Ptr group{
            std::make_shared<wsched::GroupScheduler>("GroupSched", maxThreads, 2, 3, priority++)};
    wsched::ScanScheduler::Ptr scanSlow{std::make_shared<wsched::ScanScheduler>(
            "ScanSlow", maxThreads, 2, priority++, maxActiveChunks, memMan, medium + 1, slow, oneHr)};
    wsched::ScanScheduler::Ptr scanMed{std::make_shared<wsched::ScanScheduler>(
            "ScanMed", maxThreads, 2, priority++, maxActiveChunks, memMan, fast + 1, medium, oneHr)};
    wsched::ScanScheduler::Ptr scanFast{std::make_shared<wsched::ScanScheduler>(
            "ScanFast", maxThreads, 3, priority++, maxActiveChunks, memMan, fastest, fast, _maxScanTimeFast)};
    std::vector<wsched::ScanScheduler::Ptr> scanSchedulers{scanFast, scanMed};

    lsst::qserv::wpublish::QueriesAndChunks::Ptr queries;
    wsched::BlendScheduler::Ptr blend;
};

// TODO: DM-33302 replace this test case
BOOST_AUTO_TEST_CASE(Grouping) {
#if 0   // &&& fix and re-enable
    SchedFixture f(60.0, 1);  // Values to keep QueriesAndChunk from triggering.

    LOGS(_log, LOG_LVL_DEBUG, "Test_case grouping");
    // Test grouping by chunkId. Max entries added to a single group set to 3.
    wsched::GroupScheduler gs{"GroupSchedA", 100, 0, 3, 0};
    // chunk Ids
    int a = 50;
    int b = 11;
    int c = 75;
    int d = 4;

    BOOST_CHECK(gs.empty() == true);
    BOOST_CHECK(gs.ready() == false);

    lsst::qserv::QueryId qIdInc = 1;
    Task::Ptr a1 = queMsgWithChunkId(gs, a, qIdInc++, 0, f.queries);
    BOOST_CHECK(gs.empty() == false);
    BOOST_CHECK(gs.ready() == true);

    Task::Ptr b1 = queMsgWithChunkId(gs, b, qIdInc++, 0, f.queries);
    Task::Ptr c1 = queMsgWithChunkId(gs, c, qIdInc++, 0, f.queries);
    Task::Ptr b2 = queMsgWithChunkId(gs, b, qIdInc++, 0, f.queries);
    Task::Ptr b3 = queMsgWithChunkId(gs, b, qIdInc++, 0, f.queries);
    Task::Ptr b4 = queMsgWithChunkId(gs, b, qIdInc++, 0, f.queries);
    Task::Ptr a2 = queMsgWithChunkId(gs, a, qIdInc++, 0, f.queries);
    Task::Ptr a3 = queMsgWithChunkId(gs, a, qIdInc++, 0, f.queries);
    Task::Ptr b5 = queMsgWithChunkId(gs, b, qIdInc++, 0, f.queries);
    Task::Ptr d1 = queMsgWithChunkId(gs, d, qIdInc++, 0, f.queries);
    BOOST_CHECK(gs.getSize() == 5);
    BOOST_CHECK(gs.ready() == true);
    // Should get all the first 3 'a' commands in order
    auto aa1 = gs.getCmd(false);
    auto aa2 = gs.getCmd(false);
    Task::Ptr a4 = queMsgWithChunkId(gs, a, qIdInc++, 0, f.queries);  // this should get its own group
    auto aa3 = gs.getCmd(false);
    BOOST_CHECK(a1.get() == aa1.get());
    BOOST_CHECK(a2.get() == aa2.get());
    BOOST_CHECK(a3.get() == aa3.get());
    BOOST_CHECK(gs.getInFlight() == 3);
    BOOST_CHECK(gs.ready() == true);

    // Should get the first 3 'b' commands in order
    auto bb1 = gs.getCmd(false);
    auto bb2 = gs.getCmd(false);
    auto bb3 = gs.getCmd(false);
    BOOST_CHECK(b1.get() == bb1.get());
    BOOST_CHECK(b2.get() == bb2.get());
    BOOST_CHECK(b3.get() == bb3.get());
    BOOST_CHECK(gs.getInFlight() == 6);
    BOOST_CHECK(gs.ready() == true);

    // Verify that commandFinish reduces in flight count.
    gs.commandFinish(a1);
    BOOST_CHECK(gs.getInFlight() == 5);

    // Should get the only 'c' command
    auto cc1 = gs.getCmd(false);
    BOOST_CHECK(c1.get() == cc1.get());
    BOOST_CHECK(gs.getInFlight() == 6);

    // Should get the last 2 'b' commands
    auto bb4 = gs.getCmd(false);
    auto bb5 = gs.getCmd(false);
    BOOST_CHECK(b4.get() == bb4.get());
    BOOST_CHECK(b5.get() == bb5.get());
    BOOST_CHECK(gs.getInFlight() == 8);
    BOOST_CHECK(gs.ready() == true);

    // Get the 'd' command
    auto dd1 = gs.getCmd(false);
    BOOST_CHECK(d1.get() == d1.get());
    BOOST_CHECK(gs.getInFlight() == 9);
    BOOST_CHECK(gs.ready() == true);

    // Get the last 'a' command
    auto aa4 = gs.getCmd(false);
    BOOST_CHECK(a4.get() == aa4.get());
    BOOST_CHECK(gs.getInFlight() == 10);
    BOOST_CHECK(gs.ready() == false);
    BOOST_CHECK(gs.empty() == true);
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(GroupMaxThread) {
#if 0   // &&& fix and re-enable
    // Test that maxThreads is meaningful.
    LOGS(_log, LOG_LVL_WARN, "Test_case GroupMaxThread");
    auto queries = QueriesAndChunks::setupGlobal(chrono::seconds(1), chrono::seconds(300), maxBootedC,
                                                 maxDarkTasksC, resetForTestingC);
    wsched::GroupScheduler gs{"GroupSchedB", 3, 0, 100, 0};
    lsst::qserv::QueryId qIdInc = 1;
    int a = 42;
    Task::Ptr a1 = queMsgWithChunkId(gs, a, qIdInc++, 0, queries);
    Task::Ptr a2 = queMsgWithChunkId(gs, a, qIdInc++, 0, queries);
    Task::Ptr a3 = queMsgWithChunkId(gs, a, qIdInc++, 0, queries);
    Task::Ptr a4 = queMsgWithChunkId(gs, a, qIdInc++, 0, queries);
    BOOST_CHECK(gs.ready() == true);
    auto aa1 = gs.getCmd(false);
    BOOST_CHECK(a1.get() == aa1.get());

    BOOST_CHECK(gs.ready() == true);
    auto aa2 = gs.getCmd(false);
    BOOST_CHECK(a2.get() == aa2.get());

    BOOST_CHECK(gs.ready() == true);
    auto aa3 = gs.getCmd(false);
    BOOST_CHECK(a3.get() == aa3.get());
    BOOST_CHECK(gs.getInFlight() == 3);
    BOOST_CHECK(gs.ready() == false);

    gs.commandFinish(a3);
    BOOST_CHECK(gs.ready() == true);
    auto aa4 = gs.getCmd(false);
    BOOST_CHECK(a4.get() == aa4.get());
    BOOST_CHECK(gs.ready() == false);
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(ScanScheduleTest) {
#if 0   // &&& fix and re-enable
    LOGS(_log, LOG_LVL_DEBUG, "Test_case ScanScheduleTest");
    auto queries = QueriesAndChunks::setupGlobal(chrono::seconds(1), chrono::seconds(300), maxBootedC,
                                                 maxDarkTasksC, resetForTestingC);
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, false);
    wsched::ScanScheduler sched{"ScanSchedA", 2, 1, 0, 20, memMan, 0, 100, oneHr};

    lsst::qserv::QueryId qIdInc = 1;

    // Test ready state as Tasks added and removed.
    BOOST_CHECK(sched.ready() == false);

    Task::Ptr a38 = makeTask(newTaskMsgScan(38, 0, qIdInc++, 0), queries);
    sched.queCmd(a38);
    // Calling read swaps active and pending heaps, putting a38 at the top of the active.
    BOOST_CHECK(sched.ready() == true);

    Task::Ptr a40 = makeTask(newTaskMsgScan(40, 0, qIdInc++, 0), queries);  // goes on active
    sched.queCmd(a40);

    // Making a non-scan message so MemManNone will grant it an empty Handle
    Task::Ptr b41 = makeTask(newTaskMsg(41, qIdInc++, 0), queries);  // goes on active
    sched.queCmd(b41);

    // Making a non-scan message so MemManNone will grant it an empty Handle
    Task::Ptr a33 = makeTask(newTaskMsg(33, qIdInc++, 0), queries);  // goes on pending.
    sched.queCmd(a33);

    BOOST_CHECK(sched.ready() == true);
    auto aa38 = sched.getCmd(false);
    BOOST_CHECK(aa38.get() == a38.get());
    BOOST_CHECK(sched.getInFlight() == 1);
    sched.commandStart(aa38);
    BOOST_CHECK(sched.getInFlight() == 1);
    BOOST_CHECK(sched.ready() == false);
    sched.commandFinish(aa38);
    BOOST_CHECK(sched.getInFlight() == 0);

    BOOST_CHECK(sched.ready() == true);
    auto tsk1 = sched.getCmd(false);
    BOOST_CHECK(sched.getInFlight() == 1);
    sched.commandStart(tsk1);
    BOOST_CHECK(sched.ready() == true);
    auto tsk2 = sched.getCmd(false);
    BOOST_CHECK(sched.getInFlight() == 2);
    sched.commandStart(tsk2);
    // Test max of 2 tasks running at a time
    BOOST_CHECK(sched.ready() == false);
    sched.commandFinish(tsk2);
    BOOST_CHECK(sched.getInFlight() == 1);
    BOOST_CHECK(sched.ready() == true);
    auto tsk3 = sched.getCmd(false);
    BOOST_CHECK(sched.getInFlight() == 2);
    BOOST_CHECK(sched.ready() == false);
    sched.commandStart(tsk3);
    sched.commandFinish(tsk3);
    BOOST_CHECK(sched.getInFlight() == 1);
    BOOST_CHECK(sched.ready() == false);
    sched.commandFinish(tsk1);
    BOOST_CHECK(sched.getInFlight() == 0);
    BOOST_CHECK(sched.ready() == false);
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(BlendScheduleTest) {
#if 0   // &&& fix and re-enable
    LOGS(_log, LOG_LVL_DEBUG, "Test_case BlendScheduleTest");
    // Test that space is appropriately reserved for each scheduler as Tasks are started and finished.
    // In this case, memMan->lock(..) always returns true (really HandleType::ISEMPTY).
    // ChunkIds matter as they control the order Tasks come off individual schedulers.
    SchedFixture f(60.0, 1);  // Values to keep QueriesAndChunk from triggering.

    BOOST_CHECK(f.blend->ready() == false);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 5);

    // Put one message on each scheduler except ScanFast, which gets 2.
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-1 add Tasks");
    Task::Ptr g1 = makeTask(newTaskMsgSimple(40, f.qIdInc++, 0), f.queries);
    f.blend->queCmd(g1);
    BOOST_CHECK(f.group->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    auto taskMsg = newTaskMsgScan(27, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF1 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sF1);
    BOOST_CHECK(f.scanFast->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(40, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF2 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sF2);
    BOOST_CHECK(f.scanFast->getSize() == 2);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(34, lsst::qserv::proto::ScanInfo::Rating::SLOW, f.qIdInc++, 0);
    Task::Ptr sS1 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sS1);
    BOOST_CHECK(f.scanSlow->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(31, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0);
    Task::Ptr sM1 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sM1);
    BOOST_CHECK(f.scanMed->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    BOOST_CHECK(f.blend->getSize() == 5);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 5);

    // Start all the Tasks.
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-1 start all tasks");
    // Tasks should come out in order of scheduler priority.
    auto og1 = f.blend->getCmd(false);
    BOOST_CHECK(og1.get() == g1.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 4);
    auto osF1 = f.blend->getCmd(false);
    BOOST_CHECK(osF1.get() == sF1.get());  // sF1 has lower chunkId than sF2
    BOOST_CHECK(f.blend->calcAvailableTheads() == 3);
    auto osF2 = f.blend->getCmd(false);
    BOOST_CHECK(osF2.get() == sF2.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 2);
    auto osM1 = f.blend->getCmd(false);
    BOOST_CHECK(osM1.get() == sM1.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 1);
    auto osS1 = f.blend->getCmd(false);
    BOOST_CHECK(osS1.get() == sS1.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    BOOST_CHECK(f.blend->getSize() == 0);
    BOOST_CHECK(f.blend->ready() == false);

    // All threads should now be in use or reserved, should be able to start one
    // Task for each scheduler but second Task should remain on queue.
    Task::Ptr g2 = makeTask(newTaskMsgSimple(41, f.qIdInc++, 0), f.queries);
    f.blend->queCmd(g2);
    BOOST_CHECK(f.group->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    Task::Ptr g3 = makeTask(newTaskMsgSimple(12, f.qIdInc++, 0), f.queries);
    f.blend->queCmd(g3);
    BOOST_CHECK(f.group->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 2);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(70, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF3 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sF3);
    BOOST_CHECK(f.scanFast->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 3);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(72, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF4 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sF4);
    BOOST_CHECK(f.scanFast->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 4);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(13, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0);
    Task::Ptr sM2 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sM2);
    BOOST_CHECK(f.scanMed->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 5);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(15, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0);
    Task::Ptr sM3 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sM3);
    BOOST_CHECK(f.scanMed->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 6);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(5, lsst::qserv::proto::ScanInfo::Rating::SLOW, f.qIdInc++, 0);
    Task::Ptr sS2 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sS2);
    BOOST_CHECK(f.scanSlow->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 7);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(6, lsst::qserv::proto::ScanInfo::Rating::SLOW, f.qIdInc++, 0);
    Task::Ptr sS3 = makeTask(taskMsg, f.queries);
    f.blend->queCmd(sS3);
    BOOST_CHECK(f.scanSlow->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 8);
    BOOST_CHECK(f.blend->ready() == true);

    // Expect 1 group, 1 fast, 1 medium, and 1 slow in that order
    auto og2 = f.blend->getCmd(false);
    BOOST_CHECK(og2.get() == g2.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    auto osF3 = f.blend->getCmd(false);
    BOOST_CHECK(osF3.get() == sF3.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    BOOST_CHECK(f.blend->ready() == true);
    auto osM2 = f.blend->getCmd(false);
    BOOST_CHECK(osM2.get() == sM2.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    BOOST_CHECK(f.blend->ready() == true);
    auto osS2 = f.blend->getCmd(false);
    BOOST_CHECK(osS2.get() == sS2.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    BOOST_CHECK(f.blend->getSize() == 4);
    BOOST_CHECK(f.blend->ready() == false);  // all threads in use

    // Finishing a fast Task should allow the last fast Task to go.
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-1 call commandFinish");
    f.blend->commandFinish(osF3);
    auto osF4 = f.blend->getCmd(false);
    BOOST_CHECK(osF4.get() == sF4.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    BOOST_CHECK(f.blend->ready() == false);

    // Finishing 2 fast Tasks should allow a group Task to go.
    f.blend->commandFinish(osF1);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 0);
    f.blend->commandFinish(osF2);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 1);
    auto og3 = f.blend->getCmd(false);
    BOOST_CHECK(og3.get() == g3.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 1);
    BOOST_CHECK(f.blend->ready() == false);

    // Finishing the last fast Task should let a medium Task go.
    f.blend->commandFinish(osF4);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 2);
    auto osM3 = f.blend->getCmd(false);
    BOOST_CHECK(osM3.get() == sM3.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 2);
    BOOST_CHECK(f.blend->ready() == false);
    BOOST_CHECK(f.blend->getCmd(false) == nullptr);

    // Finishing a group Task should allow a slow Task to got (only remaining Task)
    BOOST_CHECK(f.blend->getSize() == 1);
    f.blend->commandFinish(og1);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 2);
    auto osS3 = f.blend->getCmd(false);
    BOOST_CHECK(osS3.get() == sS3.get());
    BOOST_CHECK(f.blend->calcAvailableTheads() == 2);
    BOOST_CHECK(f.blend->getSize() == 0);
    BOOST_CHECK(f.blend->ready() == false);

    // Close out all tasks and check counts.
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-1 close out all Tasks");
    f.blend->commandFinish(og2);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 2);
    BOOST_CHECK(f.blend->getInFlight() == 7);
    f.blend->commandFinish(og3);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 3);
    BOOST_CHECK(f.blend->getInFlight() == 6);
    f.blend->commandFinish(osM1);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 3);
    BOOST_CHECK(f.blend->getInFlight() == 5);
    f.blend->commandFinish(osM2);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 3);
    f.blend->commandFinish(osM3);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 4);
    f.blend->commandFinish(osS1);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 4);
    f.blend->commandFinish(osS2);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 4);
    f.blend->commandFinish(osS3);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 5);
    BOOST_CHECK(f.blend->getInFlight() == 0);
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-1 done");
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(BlendScheduleThreadLimitingTest) {
#if 0   // &&& fix and re-enable
    LOGS(_log, LOG_LVL_DEBUG, "Test_case BlendScheduleThreadLimitingTest");
    SchedFixture f(60.0, 1);  // Values to keep QueriesAndChunk from triggering.
    // Test that only 6 threads can be started on a single ScanScheduler
    // This leaves 3 threads available, 1 for each other scheduler.
    BOOST_CHECK(f.blend->ready() == false);
    std::vector<Task::Ptr> scanTasks;
    for (int j = 0; j < 7; ++j) {
        auto tsk = makeTask(newTaskMsgScan(j, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0),
                            f.queries);
        f.blend->queCmd(tsk);
        if (j < 6) {
            BOOST_CHECK(f.blend->ready() == true);
            auto cmd = f.blend->getCmd(false);
            BOOST_CHECK(cmd != nullptr);
            auto task = std::dynamic_pointer_cast<lsst::qserv::wbase::Task>(cmd);
            scanTasks.push_back(task);
        }
        if (j == 6) {
            BOOST_CHECK(f.blend->ready() == false);
            BOOST_CHECK(f.blend->getCmd(false) == nullptr);
        }
    }
    {
        // Finishing one task should allow the 7th one to run.
        f.blend->commandFinish(scanTasks[0]);
        BOOST_CHECK(f.blend->ready() == true);
        auto cmd = f.blend->getCmd(false);
        BOOST_CHECK(cmd != nullptr);
        auto task = std::dynamic_pointer_cast<lsst::qserv::wbase::Task>(cmd);
        scanTasks.push_back(task);
    }
    // Finish all the scanTasks, scanTasks[0] is already finished.
    for (int j = 1; j < 7; ++j) f.blend->commandFinish(scanTasks[j]);
    BOOST_CHECK(f.blend->getInFlight() == 0);
    BOOST_CHECK(f.blend->ready() == false);

    // Test that only 6 threads can be started on a single GroupScheduler
    // This leaves 3 threads available, 1 for each other scheduler.
    std::vector<Task::Ptr> groupTasks;
    for (int j = 0; j < 7; ++j) {
        f.blend->queCmd(makeTask(newTaskMsg(j, f.qIdInc++, 0), f.queries));
        if (j < 6) {
            BOOST_CHECK(f.blend->ready() == true);
            auto cmd = f.blend->getCmd(false);
            BOOST_CHECK(cmd != nullptr);
            auto task = std::dynamic_pointer_cast<lsst::qserv::wbase::Task>(cmd);
            groupTasks.push_back(task);
        }
        if (j == 6) {
            BOOST_CHECK(f.blend->ready() == false);
            BOOST_CHECK(f.blend->getCmd(false) == nullptr);
        }
    }
    {
        // Finishing one task should allow the 7th one to run.
        f.blend->commandFinish(groupTasks[0]);
        BOOST_CHECK(f.blend->ready() == true);
        auto cmd = f.blend->getCmd(false);
        BOOST_CHECK(cmd != nullptr);
        auto task = std::dynamic_pointer_cast<lsst::qserv::wbase::Task>(cmd);
        groupTasks.push_back(task);
    }
    // Finish all the groupTasks, groupTasks[0] is already finished.
    for (int j = 1; j < 7; ++j) f.blend->commandFinish(groupTasks[j]);
    BOOST_CHECK(f.blend->getInFlight() == 0);
    BOOST_CHECK(f.blend->ready() == false);
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-2 done");
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(BlendScheduleQueryRemovalTest) {
#if 0   // &&& fix and re-enable
    // Test that space is appropriately reserved for each scheduler as Tasks are started and finished.
    // In this case, memMan->lock(..) always returns true (really HandleType::ISEMPTY).
    // ChunkIds matter as they control the order Tasks come off individual schedulers.
    SchedFixture f(60.0, 1);  // Values to keep QueriesAndChunk from triggering.
    LOGS(_log, LOG_LVL_DEBUG, "Test_case BlendScheduleQueryRemovalTest");
    // Add two queries to scanFast scheduler and then move one query to scanSlow.
    int startChunk = 70;
    unsigned int jobs = 11;
    unsigned int jobsA = jobs;
    unsigned int jobsB = jobs;
    std::vector<Task::Ptr> queryATasks;
    std::vector<Task::Ptr> queryBTasks;
    lsst::qserv::QueryId qIdA = f.qIdInc++;
    lsst::qserv::QueryId qIdB = f.qIdInc++;
    {
        int jobId = 0;
        int chunkId = startChunk;
        for (unsigned int j = 0; j < jobs; ++j) {
            auto taskMsg = newTaskMsgScan(chunkId, lsst::qserv::proto::ScanInfo::Rating::FAST, qIdA, jobId);
            Task::Ptr mv = makeTask(taskMsg, f.queries);
            queryATasks.push_back(mv);
            f.queries->addTask(mv);
            f.blend->queCmd(mv);
            taskMsg = newTaskMsgScan(chunkId++, lsst::qserv::proto::ScanInfo::Rating::FAST, qIdB, jobId++);
            mv = makeTask(taskMsg, f.queries);
            queryBTasks.push_back(mv);
            f.queries->addTask(mv);
            f.blend->queCmd(mv);
        }
    }
    BOOST_CHECK(f.scanFast->getSize() == jobs * 2);
    BOOST_CHECK(f.scanSlow->getSize() == 0);

    // This should run 1 job from one of the queries, but there are no guarantees about which one.
    // This is to test that moveUserQuery behaves appropriately for running Tasks.
    auto poppedTask = f.blend->getCmd(false);
    bool poppedFromA = false;
    for (auto const& tk : queryATasks) {
        if (tk == poppedTask) {
            poppedFromA = true;
            break;
        }
    }
    if (poppedFromA)
        --jobsA;
    else
        --jobsB;

    f.blend->moveUserQuery(qIdA, f.scanFast, f.scanSlow);  // move query qIdA to scanSlow.
    LOGS(_log, LOG_LVL_DEBUG, "fastSize=" << f.scanFast->getSize() << " slowSize=" << f.scanSlow->getSize());
    BOOST_CHECK(f.scanFast->getSize() == jobsB);
    BOOST_CHECK(f.scanSlow->getSize() == jobsA);
    // Can't use queryATasks[0] for this as it was popped from the queue before the move.
    auto taskFromA = queryATasks[1];
    auto schedForA = std::dynamic_pointer_cast<wsched::ScanScheduler>(taskFromA->getTaskScheduler());
    LOGS(_log, LOG_LVL_DEBUG, "taskFromA=" << taskFromA->getIdStr() << " sched=" << schedForA->getName());
    BOOST_CHECK(schedForA == f.scanSlow);
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(BlendScheduleQueryBootTaskTest) {
#if 0   // &&& fix and re-enable
    // Test if a task is removed if it takes takes too long.
    // Give the user query 0.1 seconds to run and run it for a second, it should get removed.
    double tenthOfSecInMinutes = 1.0 / 600.0;  // task
    SchedFixture f(tenthOfSecInMinutes, 1);    // sleep 1 second then check if tasks took too long
    LOGS(_log, LOG_LVL_DEBUG, "Test_case BlendScheduleQueryBootTaskTest");

    // Create a thread pool to run task
    auto pool = lsst::qserv::util::ThreadPool::newThreadPool(20, 1000, f.blend);

    // Create fake data - one query to get a baseline time, another to take too long.
    // IMPORTANT: the "fast" taskl is needed to establish the baseline in QueriesAndChunks.
    // Otherwise the next task (the one which is going to be booted from its scheduler)
    // won't be booted.
    int qid = 5;
    auto taskMsg = newTaskMsgScan(27, lsst::qserv::proto::ScanInfo::Rating::FAST, qid++, 0);
    Task::Ptr task = makeTask(taskMsg, f.queries);
    std::atomic<bool> running{false};
    auto fastFunc = [&running, &task, queriesAndChunks = f.queries](lsst::qserv::util::CmdData*) {
        queriesAndChunks->startedTask(task);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        queriesAndChunks->finishedTask(task);
        running = true;
    };
    task->setUnitTest(fastFunc);
    f.queries->addTask(task);
    f.blend->queCmd(task);
    while (!running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    running = false;
    // f.queries should now have a baseline for chunk 27.
    LOGS(_log, LOG_LVL_DEBUG, "Chunks after fastFunc " << *f.queries);

    taskMsg = newTaskMsgScan(27, lsst::qserv::proto::ScanInfo::Rating::FAST, qid, 0);
    task = makeTask(taskMsg, f.queries);
    std::atomic<bool> slowSleepDone{false};
    auto slowFunc = [&running, &slowSleepDone, &task,
                     queriesAndChunks = f.queries](lsst::qserv::util::CmdData*) {
        queriesAndChunks->startedTask(task);
        running = true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        slowSleepDone = true;
        // Keep this thread running until told to stop.
        while (running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        queriesAndChunks->finishedTask(task);
        LOGS(_log, LOG_LVL_DEBUG, "slowFunc end");
    };
    task->setUnitTest(slowFunc);
    f.queries->addTask(task);
    auto queryStats = f.queries->getStats(qid);
    BOOST_CHECK(queryStats != nullptr);
    if (queryStats != nullptr) {
        BOOST_CHECK(queryStats->getTasksBooted() == 0);
    }
    f.blend->queCmd(task);
    // Wait for slowFunc to start running then wait for slowFunc to finish sleeping.
    while (!running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    while (!slowSleepDone) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // By now the slowFunc query has taken a second, far longer than the 0.1 seconds it was allowed.
    // examineAll() should boot the query.
    LOGS(_log, LOG_LVL_INFO, "Chunks after slowFunc " << *f.queries);
    f.queries->examineAll();
    running = false;  // allow slowFunc to exit its loop and finish.
    LOGS(_log, LOG_LVL_INFO, "Chunks after examineAll " << *f.queries);

    // Check if the tasks booted value for qid has gone up.
    queryStats = f.queries->getStats(qid);
    BOOST_CHECK(queryStats != nullptr);
    if (queryStats != nullptr) {
        LOGS(_log, LOG_LVL_INFO, "taskBooted=" << queryStats->getTasksBooted());
        BOOST_CHECK(queryStats->getTasksBooted() == 1);
    }

    LOGS(_log, LOG_LVL_INFO, "BlendScheduleQueryBootTaskTest waiting for pool to finish.");
    pool->shutdownPool();
    LOGS(_log, LOG_LVL_INFO, "BlendScheduleQueryBootTaskTest done");
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(SlowTableHeapTest) {
#if 0   // &&& fix and re-enable
    LOGS(_log, LOG_LVL_DEBUG, "Test_case SlowTableHeapTest start");
    auto queries = QueriesAndChunks::setupGlobal(chrono::seconds(1), chrono::seconds(300), maxBootedC,
                                                 maxDarkTasksC, resetForTestingC);
    wsched::ChunkTasks::SlowTableHeap heap{};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(heap.empty() == true);

    Task::Ptr a1 = makeTask(newTaskMsgScan(7, 3, qIdInc++, 0, "charlie"), queries);
    heap.push(a1);
    BOOST_CHECK(heap.top().get() == a1.get());
    BOOST_CHECK(heap.empty() == false);

    Task::Ptr a2 = makeTask(newTaskMsgScan(7, 3, qIdInc++, 0, "delta"), queries);
    heap.push(a2);
    BOOST_CHECK(heap.top().get() == a2.get());

    Task::Ptr a3 = makeTask(newTaskMsgScan(7, 4, qIdInc++, 0, "bravo"), queries);
    heap.push(a3);
    BOOST_CHECK(heap.top().get() == a3.get());

    Task::Ptr a4 = makeTask(newTaskMsgScan(7, 2, qIdInc++, 0, "alpha"), queries);
    heap.push(a4);
    BOOST_CHECK(heap.top().get() == a3.get());
    BOOST_CHECK(heap.size() == 4);

    BOOST_CHECK(heap.pop().get() == a3.get());
    BOOST_CHECK(heap.pop().get() == a2.get());
    BOOST_CHECK(heap.pop().get() == a1.get());
    BOOST_CHECK(heap.pop().get() == a4.get());
    BOOST_CHECK(heap.empty() == true);
    LOGS(_log, LOG_LVL_DEBUG, "SlowTableHeapTest done");
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(ChunkTasksTest) {
#if 0   // &&& fix and re-enable
    LOGS(_log, LOG_LVL_DEBUG, "Test_case ChunkTasksTest start");
    auto queries = QueriesAndChunks::setupGlobal(chrono::seconds(1), chrono::seconds(300), maxBootedC,
                                                 maxDarkTasksC, resetForTestingC);
    // MemManNone always returns that memory is available.
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, true);
    int chunkId = 7;
    wsched::ChunkTasks chunkTasks{chunkId, memMan};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(chunkTasks.empty() == true);
    BOOST_CHECK(chunkTasks.readyToAdvance() == true);

    Task::Ptr a1 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "charlie"), queries);
    chunkTasks.queTask(a1);
    BOOST_CHECK(chunkTasks.empty() == false);
    BOOST_CHECK(chunkTasks.readyToAdvance() == false);
    BOOST_CHECK(chunkTasks.size() == 1);

    Task::Ptr a2 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "delta"), queries);
    chunkTasks.queTask(a2);
    BOOST_CHECK(chunkTasks.size() == 2);

    Task::Ptr a3 = makeTask(newTaskMsgScan(chunkId, 4, qIdInc++, 0, "bravo"), queries);
    chunkTasks.queTask(a3);
    BOOST_CHECK(chunkTasks.size() == 3);

    Task::Ptr a4 = makeTask(newTaskMsgScan(chunkId, 2, qIdInc++, 0, "alpha"), queries);
    chunkTasks.queTask(a4);
    BOOST_CHECK(chunkTasks.size() == 4);

    BOOST_CHECK(chunkTasks.getTask(true).get() == a3.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a2.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a1.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a4.get());
    chunkTasks.taskComplete(a1);
    chunkTasks.taskComplete(a1);  // duplicate should not cause problems.
    chunkTasks.taskComplete(a2);
    chunkTasks.taskComplete(a4);
    BOOST_CHECK(chunkTasks.empty() == true);
    BOOST_CHECK(chunkTasks.readyToAdvance() == false);
    chunkTasks.taskComplete(a3);
    BOOST_CHECK(chunkTasks.readyToAdvance() == true);

    chunkTasks.setActive();
    chunkTasks.queTask(a3);
    chunkTasks.queTask(a4);
    chunkTasks.queTask(a2);
    chunkTasks.queTask(a1);
    BOOST_CHECK(chunkTasks.readyToAdvance() == true);
    BOOST_CHECK(chunkTasks.empty() == false);
    BOOST_CHECK(chunkTasks.size() == 4);

    // move tasks from pending to active
    chunkTasks.setActive(false);
    BOOST_CHECK(chunkTasks.readyToAdvance() == false);
    BOOST_CHECK(chunkTasks.empty() == false);
    BOOST_CHECK(chunkTasks.size() == 4);

    BOOST_CHECK(chunkTasks.getTask(true).get() == a3.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a2.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a1.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a4.get());
    BOOST_CHECK(chunkTasks.empty() == true);
    BOOST_CHECK(chunkTasks.readyToAdvance() == false);
    chunkTasks.taskComplete(a1);
    chunkTasks.taskComplete(a2);
    chunkTasks.taskComplete(a3);
    chunkTasks.taskComplete(a4);
    BOOST_CHECK(chunkTasks.readyToAdvance() == true);
    LOGS(_log, LOG_LVL_DEBUG, "ChunkTasksTest done");
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_CASE(ChunkTasksQueueTest) {
#if 0   // &&& fix and re-enable
    LOGS(_log, LOG_LVL_DEBUG, "Test_case ChunkTasksQueueTest start");
    auto queries = QueriesAndChunks::setupGlobal(chrono::seconds(1), chrono::seconds(300), maxBootedC,
                                                 maxDarkTasksC, resetForTestingC);
    // MemManNone always returns that memory is available.
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, true);
    int firstChunkId = 100;
    int secondChunkId = 150;
    int chunkId = firstChunkId;
    wsched::ChunkTasksQueue ctl{nullptr, memMan};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(ctl.empty() == true);
    BOOST_CHECK(ctl.ready(true) == false);

    Task::Ptr a1 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "charlie"), queries);
    ctl.queueTask(a1);
    BOOST_CHECK(ctl.empty() == false);

    Task::Ptr a2 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "delta"), queries);
    ctl.queueTask(a2);
    Task::Ptr a3 = makeTask(newTaskMsgScan(chunkId, 4, qIdInc++, 0, "bravo"), queries);
    ctl.queueTask(a3);
    Task::Ptr a4 = makeTask(newTaskMsgScan(chunkId, 2, qIdInc++, 0, "alpha"), queries);
    ctl.queueTask(a4);

    BOOST_CHECK(ctl.ready(true) == true);
    BOOST_CHECK(ctl.getTask(true).get() == a3.get());
    BOOST_CHECK(ctl.getTask(true).get() == a2.get());
    BOOST_CHECK(ctl.getTask(true).get() == a1.get());
    BOOST_CHECK(ctl.getTask(true).get() == a4.get());
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.empty() == false);
    ctl.taskComplete(a1);
    ctl.taskComplete(a2);
    ctl.taskComplete(a3);
    ctl.taskComplete(a4);
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.empty() == true);

    chunkId = secondChunkId;
    Task::Ptr b1 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "c"), queries);
    ctl.queueTask(b1);
    BOOST_CHECK(ctl.empty() == false);

    Task::Ptr b2 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "d"), queries);
    ctl.queueTask(b2);
    Task::Ptr b3 = makeTask(newTaskMsgScan(chunkId, 4, qIdInc++, 0, "b"), queries);
    ctl.queueTask(b3);
    Task::Ptr b4 = makeTask(newTaskMsgScan(chunkId, 2, qIdInc++, 0, "a"), queries);
    ctl.queueTask(b4);
    ctl.queueTask(a3);
    ctl.queueTask(a4);
    ctl.queueTask(a2);
    ctl.queueTask(a1);

    BOOST_CHECK(ctl.ready(true) == true);
    BOOST_CHECK(ctl.getTask(true).get() == a3.get());
    BOOST_CHECK(ctl.getTask(true).get() == a2.get());
    BOOST_CHECK(ctl.getTask(true).get() == a1.get());
    BOOST_CHECK(ctl.getTask(true).get() == a4.get());
    BOOST_CHECK(ctl.getTask(true).get() == b3.get());
    BOOST_CHECK(ctl.getTask(true).get() == b2.get());
    ctl.taskComplete(a1);
    ctl.taskComplete(a2);
    ctl.taskComplete(a3);
    ctl.taskComplete(a4);
    BOOST_CHECK(ctl.getTask(true).get() == b1.get());
    BOOST_CHECK(ctl.ready(true) == true);
    BOOST_CHECK(ctl.getTask(true).get() == b4.get());
    BOOST_CHECK(ctl.empty() == false);
    BOOST_CHECK(ctl.ready(true) == false);
    ctl.taskComplete(b1);
    ctl.taskComplete(b2);
    ctl.taskComplete(b3);
    ctl.taskComplete(b4);
    BOOST_CHECK(ctl.empty() == false);
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.empty() == true);

    // test wrap around and pending
    ctl.queueTask(b1);
    ctl.queueTask(b2);
    BOOST_CHECK(ctl.getActiveChunkId() == -1);
    BOOST_CHECK(ctl.getTask(true).get() == b2.get());
    BOOST_CHECK(ctl.getActiveChunkId() == secondChunkId);
    ctl.queueTask(a1);
    ctl.queueTask(a2);
    ctl.queueTask(a3);
    ctl.queueTask(b3);  // test pendingTasks
    ctl.queueTask(b4);
    ctl.queueTask(a4);
    BOOST_CHECK(ctl.getTask(true).get() == b1.get());
    BOOST_CHECK(ctl.getActiveChunkId() == secondChunkId);
    BOOST_CHECK(ctl.getTask(true).get() == a3.get());
    BOOST_CHECK(ctl.getTask(true).get() == a2.get());
    BOOST_CHECK(ctl.getTask(true).get() == a1.get());
    BOOST_CHECK(ctl.getTask(true).get() == a4.get());
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.getActiveChunkId() == secondChunkId);
    ctl.taskComplete(b1);
    ctl.taskComplete(b2);
    BOOST_CHECK(ctl.getActiveChunkId() == secondChunkId);
    BOOST_CHECK(ctl.getTask(true).get() == b3.get());
    BOOST_CHECK(ctl.getActiveChunkId() == firstChunkId);
    ctl.taskComplete(a1);
    ctl.taskComplete(a2);
    ctl.taskComplete(a3);
    ctl.taskComplete(a4);
    BOOST_CHECK(ctl.getTask(true).get() == b4.get());
    BOOST_CHECK(ctl.getActiveChunkId() == secondChunkId);
    ctl.taskComplete(b3);
    ctl.taskComplete(b4);
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.getActiveChunkId() == -1);
    LOGS(_log, LOG_LVL_DEBUG, "ChunkTasksQueueTest done");
#endif  // &&& fix and re-enable
}

BOOST_AUTO_TEST_SUITE_END()
