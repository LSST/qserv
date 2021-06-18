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
#include "proto/ScanTableInfo.h"
#include "proto/worker.pb.h"
#include "util/Command.h"
#include "util/EventThread.h"
#include "wbase/SendChannelShared.h"
#include "wbase/Task.h"
#include "wcontrol/TransmitMgr.h"
#include "wpublish/QueriesAndChunks.h"
#include "wpublish/ResourceMonitor.h"
#include "wsched/ChunkDisk.h"
#include "wsched/ChunkTasksQueue.h"
#include "wsched/BlendScheduler.h"
#include "wsched/FifoScheduler.h"
#include "wsched/GroupScheduler.h"
#include "wsched/ScanScheduler.h"

// Boost unit test header
#define BOOST_TEST_MODULE WorkerScheduler
#include "boost/test/included/unit_test.hpp"

namespace test = boost::test_tools;
namespace wsched = lsst::qserv::wsched;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wsched.testSchedulers");
}

using lsst::qserv::proto::TaskMsg;
using lsst::qserv::wbase::Task;
using lsst::qserv::wbase::SendChannel;
using lsst::qserv::wbase::SendChannelShared;
using lsst::qserv::wpublish::ResourceMonitor;
using lsst::qserv::wpublish::ResourceMonitorLock;

double const oneHr = 60.0;

lsst::qserv::wcontrol::TransmitMgr::Ptr locTransmitMgr =
        std::make_shared<lsst::qserv::wcontrol::TransmitMgr>(50);

std::vector<SendChannelShared::Ptr> locSendSharedPtrs;

auto gArena = std::make_shared<google::protobuf::Arena>();

std::shared_ptr<ResourceMonitor> resourceMonitor(new ResourceMonitor());

/// Just need a dummy ResourceMonitorLock as it is irrelevant to what is being tested.
std::shared_ptr<ResourceMonitorLock> getRmLock() {
    auto resourceLock = std::make_shared<ResourceMonitorLock>(*(resourceMonitor.get()), "/dummyRes/1234");
    return resourceLock;
}


Task::Ptr makeTask(TaskMsg* tm) {
    auto sendC = std::make_shared<SendChannel>();
    auto sc = SendChannelShared::create(sendC, locTransmitMgr);
    locSendSharedPtrs.push_back(sc);
    Task::Ptr task(new Task(*tm, "", 0, sc, gArena, getRmLock()));
    task->setSafeToMoveRunning(true); // Can't wait for MemMan in unit tests.
    return task;
}


struct SchedulerFixture {

    SchedulerFixture(void) {
        counter = 20;
    }
    ~SchedulerFixture(void) { }

    TaskMsg* newTaskMsg(int seq, lsst::qserv::QueryId qId, int jobId) {
        TaskMsg* t = google::protobuf::Arena::CreateMessage<TaskMsg>(gArena.get());
        t->set_session(123456);
        t->set_queryid(qId);
        t->set_jobid(jobId);
        t->set_chunkid(seq);
        t->set_db("elephant");
        for(int i=0; i < 3; ++i) {
            TaskMsg::Fragment* f = t->add_fragment();
            f->add_query("Hello, this is a query.");
            f->mutable_subchunks()->add_id(100+i);
            f->set_resulttable("r_341");
        }
        ++counter;
        return t;
    }

    TaskMsg* newTaskMsgSimple(int seq, lsst::qserv::QueryId qId, int jobId) {
        TaskMsg* t = google::protobuf::Arena::CreateMessage<TaskMsg>(gArena.get());
        t->set_session(123456);
        t->set_queryid(qId);
        t->set_jobid(jobId);
        t->set_chunkid(seq);
        t->set_db("moose");
        ++counter;
        return t;
    }


    TaskMsg* newTaskMsgScan(int seq, int priority, lsst::qserv::QueryId qId, int jobId,
                              std::string const& tableName="whatever") {
        TaskMsg* taskMsg = newTaskMsg(seq, qId, jobId);
        taskMsg->set_scanpriority(priority);
        auto sTbl = taskMsg->add_scantable();
        sTbl->set_db("elephant");
        sTbl->set_table(tableName);
        sTbl->set_scanrating(priority);
        sTbl->set_lockinmemory(true);
        return taskMsg;
    }


    Task::Ptr queMsgWithChunkId(wsched::GroupScheduler &gs, int chunkId,
                                lsst::qserv::QueryId qId, int jobId) {
        Task::Ptr t = makeTask(newTaskMsg(chunkId, qId, jobId));
        gs.queCmd(t);
        return t;
    }

    int counter;
};


BOOST_FIXTURE_TEST_SUITE(SchedulerSuite, SchedulerFixture)

BOOST_AUTO_TEST_CASE(Grouping) {
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
    Task::Ptr a1 = queMsgWithChunkId(gs, a, qIdInc++, 0);
    BOOST_CHECK(gs.empty() == false);
    BOOST_CHECK(gs.ready() == true);


    Task::Ptr b1 = queMsgWithChunkId(gs, b, qIdInc++, 0);
    Task::Ptr c1 = queMsgWithChunkId(gs, c, qIdInc++, 0);
    Task::Ptr b2 = queMsgWithChunkId(gs, b, qIdInc++, 0);
    Task::Ptr b3 = queMsgWithChunkId(gs, b, qIdInc++, 0);
    Task::Ptr b4 = queMsgWithChunkId(gs, b, qIdInc++, 0);
    Task::Ptr a2 = queMsgWithChunkId(gs, a, qIdInc++, 0);
    Task::Ptr a3 = queMsgWithChunkId(gs, a, qIdInc++, 0);
    Task::Ptr b5 = queMsgWithChunkId(gs, b, qIdInc++, 0);
    Task::Ptr d1 = queMsgWithChunkId(gs, d, qIdInc++, 0);
    BOOST_CHECK(gs.getSize() == 5);
    BOOST_CHECK(gs.ready() == true);

    // Should get all the first 3 'a' commands in order
    auto aa1 = gs.getCmd(false);
    auto aa2 = gs.getCmd(false);
    Task::Ptr a4 = queMsgWithChunkId(gs, a, qIdInc++, 0); // this should get its own group
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
}


BOOST_AUTO_TEST_CASE(GroupMaxThread) {
    // Test that maxThreads is meaningful.
    wsched::GroupScheduler gs{"GroupSchedB", 3, 0, 100, 0};
    lsst::qserv::QueryId qIdInc = 1;
    int a = 42;
    Task::Ptr a1 = queMsgWithChunkId(gs, a, qIdInc++, 0);
    Task::Ptr a2 = queMsgWithChunkId(gs, a, qIdInc++, 0);
    Task::Ptr a3 = queMsgWithChunkId(gs, a, qIdInc++, 0);
    Task::Ptr a4 = queMsgWithChunkId(gs, a, qIdInc++, 0);
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
}

BOOST_AUTO_TEST_CASE(DiskMinHeap) {
    wsched::ChunkDisk::MinHeap minHeap{};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(minHeap.empty() == true);

    Task::Ptr a47 = makeTask(newTaskMsg(47, qIdInc++, 0));
    minHeap.push(a47);
    BOOST_CHECK(minHeap.top().get() == a47.get());
    BOOST_CHECK(minHeap.empty() == false);

    Task::Ptr a42 = makeTask(newTaskMsg(42, qIdInc++, 0));
    minHeap.push(a42);
    BOOST_CHECK(minHeap.top().get() == a42.get());

    Task::Ptr a60 = makeTask(newTaskMsg(60, qIdInc++, 0));
    minHeap.push(a60);
    BOOST_CHECK(minHeap.top().get() == a42.get());

    Task::Ptr a18 = makeTask(newTaskMsg(18, qIdInc++, 0));
    minHeap.push(a18);
    BOOST_CHECK(minHeap.top().get() == a18.get());

    BOOST_CHECK(minHeap.pop().get() == a18.get());
    BOOST_CHECK(minHeap.pop().get() == a42.get());
    BOOST_CHECK(minHeap.pop().get() == a47.get());
    BOOST_CHECK(minHeap.pop().get() == a60.get());
    BOOST_CHECK(minHeap.empty() == true);
}

BOOST_AUTO_TEST_CASE(ChunkDiskMemManNoneTest) {
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, false);
    wsched::ChunkDisk cDisk(memMan);
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(cDisk.empty() == true);
    BOOST_CHECK(cDisk.getSize() == 0);
    BOOST_CHECK(cDisk.ready(true) == false);

    Task::Ptr a47 = makeTask(newTaskMsgScan(47,0, qIdInc++, 0));
    cDisk.queueTask(a47); // should go on pending
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 1);
    // call to ready swaps active and passive.
    BOOST_CHECK(cDisk.ready(false) == false);
    // This call to read will cause a47 to be flagged as having resources to run.
    BOOST_CHECK(cDisk.ready(true) == true);


    Task::Ptr a42 = makeTask(newTaskMsgScan(42,0, qIdInc++, 0));
    cDisk.queueTask(a42); // should go on pending
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 2);
    // a47 at top of active has been flagged as ok to run.
    BOOST_CHECK(cDisk.ready(false) == true);

    Task::Ptr b42 = makeTask(newTaskMsgScan(42, 0, qIdInc++, 0));
    cDisk.queueTask(b42); // should go on pending
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 3);
    BOOST_CHECK(cDisk.ready(false) == true);

    // Get the first task
    auto aa47 = cDisk.getTask(false);
    BOOST_CHECK(aa47.get() == a47.get());
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 2);
    // useFlexibleLock should fail since MemManNone always denies LOCK requests for scans.
    BOOST_CHECK(cDisk.ready(false) == false);
    // MemManNone always grants FLEXIBLELOCK requests
    BOOST_CHECK(cDisk.ready(true) == true);
    // Since MemManNone already ok'ed the task last time ready was called, ready should be true.
    BOOST_CHECK(cDisk.ready(false) == true);

    // After calling ready, a42 is at top
    Task::Ptr a18 = makeTask(newTaskMsgScan(18, 0, qIdInc++, 0));
    cDisk.queueTask(a18); // should go on pending
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 3);
    BOOST_CHECK(cDisk.ready(false) == true);

    // The last task should still be flagged as being ok'ed by MemManNone
    auto aa42 = cDisk.getTask(false);
    BOOST_CHECK(aa42.get() == a42.get());
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 2);
    BOOST_CHECK(cDisk.ready(false) == false);

    auto bb42 = cDisk.getTask(false);
    BOOST_CHECK(bb42.get() == nullptr);
    bb42 = cDisk.getTask(true);
    BOOST_CHECK(bb42.get() == b42.get());
    BOOST_CHECK(cDisk.empty() == false);
    BOOST_CHECK(cDisk.getSize() == 1);
    BOOST_CHECK(cDisk.ready(false) == false);

    auto aa18 = cDisk.getTask(false);
    BOOST_CHECK(aa18.get() == nullptr);
    aa18 = cDisk.getTask(true);
    BOOST_CHECK(aa18.get() == a18.get());
    BOOST_CHECK(cDisk.empty() == true);
    BOOST_CHECK(cDisk.getSize() == 0);
    BOOST_CHECK(cDisk.ready(true) == false);

}


BOOST_AUTO_TEST_CASE(ScanScheduleTest) {
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, false);
    wsched::ScanScheduler sched{"ScanSchedA", 2, 1, 0, 20, memMan, 0, 100, oneHr};

    lsst::qserv::QueryId qIdInc = 1;

    // Test ready state as Tasks added and removed.
    BOOST_CHECK(sched.ready() == false);

    Task::Ptr a38 = makeTask(newTaskMsgScan(38, 0, qIdInc++, 0));
    sched.queCmd(a38);
    // Calling read swaps active and pending heaps, putting a38 at the top of the active.
    BOOST_CHECK(sched.ready() == true);

    Task::Ptr a40 = makeTask(newTaskMsgScan(40, 0, qIdInc++, 0)); // goes on active
    sched.queCmd(a40);

    // Making a non-scan message so MemManNone will grant it an empty Handle
    Task::Ptr b41 = makeTask(newTaskMsg(41, qIdInc++, 0)); // goes on active
    sched.queCmd(b41);

    // Making a non-scan message so MemManNone will grant it an empty Handle
    Task::Ptr a33 = makeTask(newTaskMsg(33, qIdInc++, 0)); // goes on pending.
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
}


struct SchedFixture {
    SchedFixture() {
        setupQueriesBlend();
    }
    SchedFixture(double maxScanTimeFast, bool examinAllSleep)
          : _maxScanTimeFast{maxScanTimeFast}, _examineAllSleep{examinAllSleep} {
        setupQueriesBlend();
    }
    ~SchedFixture() {}

    void setupQueriesBlend() {
        queries = std::make_shared<lsst::qserv::wpublish::QueriesAndChunks>(
                std::chrono::seconds(1),
                std::chrono::seconds(_examineAllSleep), 5);
        blend = std::make_shared<wsched::BlendScheduler>("blendSched", queries, maxThreads,
                group, scanSlow, scanSchedulers);
        group->setDefaultPosition(0);
        scanFast->setDefaultPosition(1);
        scanMed->setDefaultPosition(2);
        scanSlow->setDefaultPosition(3);
        queries->setBlendScheduler(blend);
        queries->setRequiredTasksCompleted(1); // Make it easy to set a baseline.
    }

    int const fastest = lsst::qserv::proto::ScanInfo::Rating::FASTEST;
    int const fast    = lsst::qserv::proto::ScanInfo::Rating::FAST;
    int const medium  = lsst::qserv::proto::ScanInfo::Rating::MEDIUM;
    int const slow    = lsst::qserv::proto::ScanInfo::Rating::SLOW;

    lsst::qserv::QueryId qIdInc{1};

    int maxThreads{9};
    int maxActiveChunks{20};
    int priority{2};

private:
    double _maxScanTimeFast{oneHr}; ///< Don't hit time limit in tests.
    int _examineAllSleep{0}; ///< Don't run _examineThread when 0

public:
    lsst::qserv::memman::MemManNone::Ptr memMan{
        std::make_shared<lsst::qserv::memman::MemManNone>(1, true)};
    wsched::GroupScheduler::Ptr group{std::make_shared<wsched::GroupScheduler>(
                "GroupSched", maxThreads, 2, 3, priority++)};
    wsched::ScanScheduler::Ptr scanSlow{std::make_shared<wsched::ScanScheduler>(
                "ScanSlow", maxThreads, 2, priority++, maxActiveChunks, memMan, medium+1, slow, oneHr)};
    wsched::ScanScheduler::Ptr scanMed{std::make_shared<wsched::ScanScheduler>(
                "ScanMed",  maxThreads, 2, priority++, maxActiveChunks, memMan, fast+1, medium, oneHr)};
    wsched::ScanScheduler::Ptr scanFast{std::make_shared<wsched::ScanScheduler>(
                "ScanFast", maxThreads, 3, priority++, maxActiveChunks, memMan, fastest, fast,
                _maxScanTimeFast)};
    std::vector<wsched::ScanScheduler::Ptr> scanSchedulers{scanFast, scanMed};

    lsst::qserv::wpublish::QueriesAndChunks::Ptr queries;
    wsched::BlendScheduler::Ptr blend;
};

BOOST_AUTO_TEST_CASE(BlendScheduleTest) {
    // Test that space is appropriately reserved for each scheduler as Tasks are started and finished.
    // In this case, memMan->lock(..) always returns true (really HandleType::ISEMPTY).
    // ChunkIds matter as they control the order Tasks come off individual schedulers.
    SchedFixture f;

    BOOST_CHECK(f.blend->ready() == false);
    BOOST_CHECK(f.blend->calcAvailableTheads() == 5);

    // Put one message on each scheduler except ScanFast, which gets 2.
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-1 add Tasks");
    Task::Ptr g1 = makeTask(newTaskMsgSimple(40, f.qIdInc++, 0));
    f.blend->queCmd(g1);
    BOOST_CHECK(f.group->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    auto taskMsg = newTaskMsgScan(27, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF1 = makeTask(taskMsg);
    f.blend->queCmd(sF1);
    BOOST_CHECK(f.scanFast->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(40, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF2 = makeTask(taskMsg);
    f.blend->queCmd(sF2);
    BOOST_CHECK(f.scanFast->getSize() == 2);
    BOOST_CHECK(f.blend->ready() == true);


    taskMsg = newTaskMsgScan(34, lsst::qserv::proto::ScanInfo::Rating::SLOW, f.qIdInc++, 0);
    Task::Ptr sS1 = makeTask(taskMsg);
    f.blend->queCmd(sS1);
    BOOST_CHECK(f.scanSlow->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(31, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0);
    Task::Ptr sM1 = makeTask(taskMsg);
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
    BOOST_CHECK(osF1.get() == sF1.get()); // sF1 has lower chunkId than sF2
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
    Task::Ptr g2 = makeTask(newTaskMsgSimple(41, f.qIdInc++, 0));
    f.blend->queCmd(g2);
    BOOST_CHECK(f.group->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 1);
    BOOST_CHECK(f.blend->ready() == true);

    Task::Ptr g3 = makeTask(newTaskMsgSimple(12, f.qIdInc++, 0));
    f.blend->queCmd(g3);
    BOOST_CHECK(f.group->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 2);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(70, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF3 = makeTask(taskMsg);
    f.blend->queCmd(sF3);
    BOOST_CHECK(f.scanFast->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 3);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(72, lsst::qserv::proto::ScanInfo::Rating::FAST, f.qIdInc++, 0);
    Task::Ptr sF4 = makeTask(taskMsg);
    f.blend->queCmd(sF4);
    BOOST_CHECK(f.scanFast->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 4);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(13, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0);
    Task::Ptr sM2 = makeTask(taskMsg);
    f.blend->queCmd(sM2);
    BOOST_CHECK(f.scanMed->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 5);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(15, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0);
    Task::Ptr sM3 = makeTask(taskMsg);
    f.blend->queCmd(sM3);
    BOOST_CHECK(f.scanMed->getSize() == 2);
    BOOST_CHECK(f.blend->getSize() == 6);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(5, lsst::qserv::proto::ScanInfo::Rating::SLOW, f.qIdInc++, 0);
    Task::Ptr sS2 = makeTask(taskMsg);
    f.blend->queCmd(sS2);
    BOOST_CHECK(f.scanSlow->getSize() == 1);
    BOOST_CHECK(f.blend->getSize() == 7);
    BOOST_CHECK(f.blend->ready() == true);

    taskMsg = newTaskMsgScan(6, lsst::qserv::proto::ScanInfo::Rating::SLOW, f.qIdInc++, 0);
    Task::Ptr sS3 = makeTask(taskMsg);
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
    BOOST_CHECK(f.blend->ready() == false); // all threads in use

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
}


BOOST_AUTO_TEST_CASE(BlendScheduleThreadLimitingTest) {
    SchedFixture f;
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-2 check thread limiting");
    // Test that only 6 threads can be started on a single ScanScheduler
    // This leaves 3 threads available, 1 for each other scheduler.
    BOOST_CHECK(f.blend->ready() == false);
    std::vector<Task::Ptr> scanTasks;
    for (int j=0; j<7; ++j) {
        auto tsk = makeTask(newTaskMsgScan(j, lsst::qserv::proto::ScanInfo::Rating::MEDIUM, f.qIdInc++, 0));
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
    for (int j=1; j<7; ++j) f.blend->commandFinish(scanTasks[j]);
    BOOST_CHECK(f.blend->getInFlight() == 0);
    BOOST_CHECK(f.blend->ready() == false);

    // Test that only 6 threads can be started on a single GroupScheduler
    // This leaves 3 threads available, 1 for each other scheduler.
    std::vector<Task::Ptr> groupTasks;
    for (int j=0; j<7; ++j) {
        f.blend->queCmd(makeTask(newTaskMsg(j, f.qIdInc++, 0)));
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
    for (int j=1; j<7; ++j) f.blend->commandFinish(groupTasks[j]);
    BOOST_CHECK(f.blend->getInFlight() == 0);
    BOOST_CHECK(f.blend->ready() == false);
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleTest-2 done");
}


BOOST_AUTO_TEST_CASE(BlendScheduleQueryRemovalTest) {
    // Test that space is appropriately reserved for each scheduler as Tasks are started and finished.
    // In this case, memMan->lock(..) always returns true (really HandleType::ISEMPTY).
    // ChunkIds matter as they control the order Tasks come off individual schedulers.
    SchedFixture f;
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleQueryRemovalTest");
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
        for (unsigned int j=0; j<jobs; ++j) {
            auto taskMsg = newTaskMsgScan(chunkId, lsst::qserv::proto::ScanInfo::Rating::FAST, qIdA, jobId);
            Task::Ptr mv = makeTask(taskMsg);
            queryATasks.push_back(mv);
            f.queries->addTask(mv);
            f.blend->queCmd(mv);
            taskMsg = newTaskMsgScan(chunkId++, lsst::qserv::proto::ScanInfo::Rating::FAST, qIdB, jobId++);
            mv = makeTask(taskMsg);
            queryBTasks.push_back(mv);
            f.queries->addTask(mv);
            f.blend->queCmd(mv);
        }
    }
    BOOST_CHECK(f.scanFast->getSize() == jobs*2);
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
    if (poppedFromA) --jobsA;
    else --jobsB;

    f.blend->moveUserQuery(qIdA, f.scanFast, f.scanSlow); // move query qIdA to scanSlow.
    LOGS(_log, LOG_LVL_DEBUG, "fastSize=" << f.scanFast->getSize() << " slowSize=" << f.scanSlow->getSize());
    BOOST_CHECK(f.scanFast->getSize() == jobsB);
    BOOST_CHECK(f.scanSlow->getSize() == jobsA);
    // Can't use queryATasks[0] for this as it was popped from the queue before the move.
    auto taskFromA = queryATasks[1];
    auto schedForA = std::dynamic_pointer_cast<wsched::ScanScheduler>(taskFromA->getTaskScheduler());
    LOGS(_log, LOG_LVL_DEBUG, "taskFromA=" << taskFromA->getIdStr() << " sched=" << schedForA->getName());
    BOOST_CHECK(schedForA == f.scanSlow);
}


BOOST_AUTO_TEST_CASE(BlendScheduleQueryBootTaskTest) {
    // Test if a task is removed if it takes takes too long.
    // Give the user query 0.1 seconds to run and run it for a second, it should get removed.
    double tenthOfSecInMinutes = 1.0/600.0; // task
    SchedFixture f(tenthOfSecInMinutes, 1); // sleep 1 second then check if tasks took too long
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleQueryBootTaskTest");

    // Create a thread pool to run task
    auto pool = lsst::qserv::util::ThreadPool::newThreadPool(20, 1000, f.blend);

    // Create fake data - one query to get a baseline time, another to take too long.
    int qid=5;
    auto taskMsg = newTaskMsgScan(27, lsst::qserv::proto::ScanInfo::Rating::FAST, qid++, 0);
    Task::Ptr task = makeTask(taskMsg);
    std::atomic<bool> running{false};
    auto fastFunc = [&running](lsst::qserv::util::CmdData*){
        running = true;
    };
    task->setFunc(fastFunc);
    f.queries->addTask(task);
    f.blend->queCmd(task);
    while (!running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    running = false;
    // f.queries should now have a baseline for chunk 27.
    LOGS(_log, LOG_LVL_DEBUG, "Chunks after fastFunc " << *f.queries);

    taskMsg = newTaskMsgScan(27, lsst::qserv::proto::ScanInfo::Rating::FAST, qid, 0);
    task = makeTask(taskMsg);
    std::atomic<bool> slowSleepDone{false};
    auto slowFunc = [&running, &slowSleepDone](lsst::qserv::util::CmdData*){
        running = true;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        slowSleepDone = true;
        // Keep this thread running until told to stop.
        while (running) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LOGS(_log, LOG_LVL_DEBUG, "slowFunc end");
    };
    task->setFunc(slowFunc);
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
    LOGS(_log, LOG_LVL_DEBUG, "Chunks after slowFunc " << *f.queries);
    f.queries->examineAll();
    running = false; // allow slowFunc to exit its loop and finish.
    LOGS(_log, LOG_LVL_DEBUG, "Chunks after examineAll " << *f.queries);

    // Check if the tasks booted value for qid has gone up.
    queryStats = f.queries->getStats(qid);
    BOOST_CHECK(queryStats != nullptr);
    if (queryStats != nullptr) {
        LOGS(_log, LOG_LVL_DEBUG, "taskBooted=" << queryStats->getTasksBooted());
        BOOST_CHECK(queryStats->getTasksBooted() == 1);
    }

    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleQueryBootTaskTest waiting for pool to finish.");
    pool->shutdownPool();
    LOGS(_log, LOG_LVL_DEBUG, "BlendScheduleQueryBootTaskTest done");
}



BOOST_AUTO_TEST_CASE(SlowTableHeapTest) {
    LOGS(_log, LOG_LVL_DEBUG, "SlowTableHeapTest start");
    wsched::ChunkTasks::SlowTableHeap heap{};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(heap.empty() == true);

    Task::Ptr a1 = makeTask(newTaskMsgScan(7, 3, qIdInc++, 0, "charlie"));
    heap.push(a1);
    BOOST_CHECK(heap.top().get() == a1.get());
    BOOST_CHECK(heap.empty() == false);

    Task::Ptr a2 = makeTask(newTaskMsgScan(7, 3, qIdInc++, 0, "delta"));
    heap.push(a2);
    BOOST_CHECK(heap.top().get() == a2.get());

    Task::Ptr a3 = makeTask(newTaskMsgScan(7, 4, qIdInc++, 0, "bravo"));
    heap.push(a3);
    BOOST_CHECK(heap.top().get() == a3.get());

    Task::Ptr a4 = makeTask(newTaskMsgScan(7, 2, qIdInc++, 0, "alpha"));
    heap.push(a4);
    BOOST_CHECK(heap.top().get() == a3.get());
    BOOST_CHECK(heap.size() == 4);


    BOOST_CHECK(heap.pop().get() == a3.get());
    BOOST_CHECK(heap.pop().get() == a2.get());
    BOOST_CHECK(heap.pop().get() == a1.get());
    BOOST_CHECK(heap.pop().get() == a4.get());
    BOOST_CHECK(heap.empty() == true);
    LOGS(_log, LOG_LVL_DEBUG, "SlowTableHeapTest done");
}


BOOST_AUTO_TEST_CASE(ChunkTasksTest) {
    LOGS(_log, LOG_LVL_DEBUG, "ChunkTasksTest start");
    // MemManNone always returns that memory is available.
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, true);
    int chunkId = 7;
    wsched::ChunkTasks chunkTasks{chunkId, memMan};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(chunkTasks.empty() == true);
    BOOST_CHECK(chunkTasks.readyToAdvance() == true);

    Task::Ptr a1 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "charlie"));
    chunkTasks.queTask(a1);
    BOOST_CHECK(chunkTasks.empty() == false);
    BOOST_CHECK(chunkTasks.readyToAdvance() == false);
    BOOST_CHECK(chunkTasks.size() == 1);

    Task::Ptr a2 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "delta"));
    chunkTasks.queTask(a2);
    BOOST_CHECK(chunkTasks.size() == 2);

    Task::Ptr a3 = makeTask(newTaskMsgScan(chunkId, 4, qIdInc++, 0, "bravo"));
    chunkTasks.queTask(a3);
    BOOST_CHECK(chunkTasks.size() == 3);

    Task::Ptr a4 = makeTask(newTaskMsgScan(chunkId, 2, qIdInc++, 0, "alpha"));
    chunkTasks.queTask(a4);
    BOOST_CHECK(chunkTasks.size() == 4);

    BOOST_CHECK(chunkTasks.getTask(true).get() == a3.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a2.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a1.get());
    BOOST_CHECK(chunkTasks.getTask(true).get() == a4.get());
    chunkTasks.taskComplete(a1);
    chunkTasks.taskComplete(a1); // duplicate should not cause problems.
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
}


BOOST_AUTO_TEST_CASE(ChunkTasksQueueTest) {
    LOGS(_log, LOG_LVL_DEBUG, "ChunkTasksQueueTest start");
    // MemManNone always returns that memory is available.
    auto memMan = std::make_shared<lsst::qserv::memman::MemManNone>(1, true);
    int firstChunkId = 100;
    int secondChunkId = 150;
    int chunkId = firstChunkId;
    wsched::ChunkTasksQueue ctl{nullptr, memMan};
    lsst::qserv::QueryId qIdInc = 1;

    BOOST_CHECK(ctl.empty() == true);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);
    BOOST_CHECK(ctl.ready(true) == false);

    Task::Ptr a1 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "charlie"));
    ctl.queueTask(a1);
    BOOST_CHECK(ctl.empty() == false);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);

    Task::Ptr a2 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "delta"));
    ctl.queueTask(a2);
    Task::Ptr a3 = makeTask(newTaskMsgScan(chunkId, 4, qIdInc++, 0, "bravo"));
    ctl.queueTask(a3);
    Task::Ptr a4 = makeTask(newTaskMsgScan(chunkId, 2, qIdInc++, 0, "alpha"));
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
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);

    chunkId = secondChunkId;
    Task::Ptr b1 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "c"));
    ctl.queueTask(b1);
    BOOST_CHECK(ctl.empty() == false);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);

    Task::Ptr b2 = makeTask(newTaskMsgScan(chunkId, 3, qIdInc++, 0, "d"));
    ctl.queueTask(b2);
    Task::Ptr b3 = makeTask(newTaskMsgScan(chunkId, 4, qIdInc++, 0, "b"));
    ctl.queueTask(b3);
    Task::Ptr b4 = makeTask(newTaskMsgScan(chunkId, 2, qIdInc++, 0, "a"));
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
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == false);
    BOOST_CHECK(ctl.getTask(true).get() == b3.get());
    BOOST_CHECK(ctl.getTask(true).get() == b2.get());
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == false);
    ctl.taskComplete(a1);
    ctl.taskComplete(a2);
    ctl.taskComplete(a3);
    ctl.taskComplete(a4);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);
    BOOST_CHECK(ctl.getTask(true).get() == b1.get());
    BOOST_CHECK(ctl.ready(true) == true);
    BOOST_CHECK(ctl.getTask(true).get() == b4.get());
    BOOST_CHECK(ctl.empty() == false);
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == false);
    ctl.taskComplete(b1);
    ctl.taskComplete(b2);
    ctl.taskComplete(b3);
    ctl.taskComplete(b4);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);
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
    ctl.queueTask(b3); // test pendingTasks
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
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);
    BOOST_CHECK(ctl.getTask(true).get() == b3.get());
    BOOST_CHECK(ctl.getActiveChunkId() == firstChunkId);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == false);
    ctl.taskComplete(a1);
    ctl.taskComplete(a2);
    ctl.taskComplete(a3);
    ctl.taskComplete(a4);
    BOOST_CHECK(ctl.nextTaskDifferentChunkId() == true);
    BOOST_CHECK(ctl.getTask(true).get() == b4.get());
    BOOST_CHECK(ctl.getActiveChunkId() == secondChunkId);
    ctl.taskComplete(b3);
    ctl.taskComplete(b4);
    BOOST_CHECK(ctl.ready(true) == false);
    BOOST_CHECK(ctl.getActiveChunkId() == -1);
    LOGS(_log, LOG_LVL_DEBUG, "ChunkTasksQueueTest done");
}


BOOST_AUTO_TEST_SUITE_END()
