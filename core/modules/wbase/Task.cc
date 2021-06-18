// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2016 AURA/LSST.
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
  * @file
  *
  * @brief Task is a bundle of query task fields
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "wbase/Task.h"

// Third-party headers
#include "boost/regex.hpp"
#include <boost/algorithm/string/replace.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/Bug.h"
#include "global/constants.h"
#include "global/LogContext.h"
#include "proto/TaskMsgDigest.h"
#include "proto/worker.pb.h"
#include "wbase/Base.h"
#include "wbase/SendChannelShared.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.wbase.Task");

std::ostream&
dump(std::ostream& os,
    lsst::qserv::proto::TaskMsg_Fragment const& f) {
    os << "frag: " << "q=";
    for(int i=0; i < f.query_size(); ++i) {
        os << f.query(i) << ",";
    }
    if (f.has_subchunks()) {
        os << " sc=";
        for(int i=0; i < f.subchunks().id_size(); ++i) {
            os << f.subchunks().id(i) << ",";
        }
    }
    os << " rt=" << f.resulttable();
    return os;
}

} // annonymous namespace

namespace lsst {
namespace qserv {
namespace wbase {

// Task::ChunkEqual functor
bool Task::ChunkEqual::operator()(Task::Ptr const& x, Task::Ptr const& y) {
    if (!x || !y) { return false; }
    //&&& if ((!x->msg) || (!y->msg)) { return false; }
    return x->msg.has_chunkid() && y->msg.has_chunkid()
        && x->msg.chunkid()  == y->msg.chunkid();
}

// Task::PtrChunkIdGreater functor
bool Task::ChunkIdGreater::operator()(Task::Ptr const& x, Task::Ptr const& y) {
    if (!x || !y) { return false; }
    //&&&if ((!x->msg) || (!y->msg)) { return false; }
    return x->msg.chunkid()  > y->msg.chunkid();
}


std::string const Task::defaultUser = "qsmaster";
IdSet Task::allIds{};



/// When the constructor is called, there is not enough information
/// available to define the action to take when this task is run, so
/// Command::setFunc() is used set the action later. This is why
/// the util::CommandThreadPool is not called here.
Task::Task(proto::TaskMsg const& t, std::string const& query, int fragmentNumber,
           std::shared_ptr<SendChannelShared> const& sc,
           std::shared_ptr<google::protobuf::Arena> const& gArena,
           std::shared_ptr<wpublish::ResourceMonitorLock> const& rmLock)
    : msg(t), sendChannel(sc),
      _qId(t.queryid()), _jId(t.jobid()), _attemptCount(t.attemptcount()),
      _idStr(QueryIdHelper::makeIdStr(_qId, _jId)),
      _queryString(query),
      _queryFragmentNum(fragmentNumber),
      _gArena(gArena),
      _resourceMonitorLock(rmLock) {


    hash = hashTaskMsg(t);

    if (t.has_user()) {
        user = t.user();
    } else {
        user = defaultUser;
    }
    timestr[0] = '\0';

    allIds.add(std::to_string(_qId) + "_" + std::to_string(_jId));
    LOGS(_log, LOG_LVL_DEBUG, "Task(...) " << "this=" << this << " : " << allIds);

    // Determine which major tables this task will use.
    int const size = msg.scantable_size();
    for(int j=0; j < size; ++j) {
        _scanInfo.infoTables.push_back(proto::ScanTableInfo(msg.scantable(j)));
    }
    _scanInfo.scanRating = msg.scanpriority();
    _scanInfo.sortTablesSlowestFirst();
    _scanInteractive = msg.scaninteractive();
}

Task::~Task() {
    allIds.remove(std::to_string(_qId) + "_" + std::to_string(_jId));
    LOGS(_log, LOG_LVL_TRACE, "~Task() : " << allIds);
}


std::vector<Task::Ptr> Task::createTasks(proto::TaskMsg const& taskMsg,
                                         std::shared_ptr<wbase::SendChannelShared> const& sendChannel,
                                         std::shared_ptr<google::protobuf::Arena> const& gArena,
                                         std::shared_ptr<wpublish::ResourceMonitorLock> const& rmLock) {
    QSERV_LOGCONTEXT_QUERY_JOB(taskMsg.queryid(), taskMsg.jobid());
    std::vector<Task::Ptr> vect;

    /// Make one task for each fragment.
    int fragmentCount = taskMsg.fragment_size();
    if (fragmentCount < 1) {
        throw Bug("Task::createTasks No fragments to execute in TaskMsg");
    }
    for (int fragNum=0; fragNum<fragmentCount; ++fragNum) {
        proto::TaskMsg_Fragment const& fragment = taskMsg.fragment(fragNum);
        for (const std::string queryStr: fragment.query()) {
            // fragment.has_subchunks() == true and fragment.subchunks().id().empty() == false
            // is apparently valid and must go to the else clause.
            // TODO: Look into the creation of fragment on the czar as this is not intuitive.
            if (fragment.has_subchunks() && not fragment.subchunks().id().empty()) {
                for (auto subchunkId : fragment.subchunks().id()) {
                    std::string qs(queryStr);
                    boost::algorithm::replace_all(qs, SUBCHUNK_TAG, std::to_string(subchunkId));
                    auto task = std::make_shared<wbase::Task>(taskMsg, qs, fragNum, sendChannel, gArena, rmLock);
                    vect.push_back(task);
                }
            } else {
                //LOGS(_log, LOG_LVL_INFO, "&&& Task::createTasks queryStr=" << queryStr);
                auto task = std::make_shared<wbase::Task>(taskMsg, queryStr, fragNum, sendChannel, gArena, rmLock);
                //TODO: Maybe? Is it better to move fragment info from
                //      ChunkResource getResourceFragment(int i) to here???
                //      It looks like Task should contain a ChunkResource::Info object
                //      which could help clean up ChunkResource and related classes.
                vect.push_back(task);
            }

        }
    }
    LOGS(_log, LOG_LVL_INFO, "&&& Task::createTasks vect.size=" << vect.size());
    //&&&sendChannel->setTaskCount(vect.size());
    sendChannel->incrTaskCountBy(vect.size());
    return vect;
}


/// @return the chunkId for this task. If the task has no chunkId, return -1.
int Task::getChunkId() {
    if (msg.has_chunkid()) {
        return msg.chunkid();
    }
    return -1;
}


/// Flag the Task as cancelled, try to stop the SQL query, and try to remove it from the schedule.
void Task::cancel() {
    if (_cancelled.exchange(true)) {
        // Was already cancelled.
        return;
    }
    auto qr = _taskQueryRunner; // Need a copy in case _taskQueryRunner is reset.
    if (qr != nullptr) {
        qr->cancel();
    }

    // At this point, this code doesn't do anything. It may be
    // useful to remove this task from the scheduler, but it
    // seems doubtful that that would improve performance.
    auto sched = _taskScheduler.lock();
    if (sched != nullptr) {
        sched->taskCancelled(this);
    }
}

bool Task::checkCancelled() {
    // A czar doesn't directly tell the worker the query is dead.
    // A czar has XrdSsi kill the SsiRequest, which kills the
    // sendChannel used by this task. sendChannel can be killed
    // in other ways, however, without the sendChannel, this task
    // has no way to return anything to the originating czar and
    // may as well give up now.
    if (sendChannel == nullptr || sendChannel->isDead()) {
        // The sendChannel is dead, probably squashed by the czar.
        cancel();
    }
    return _cancelled;
}


/// @return true if task has already been cancelled.
bool Task::setTaskQueryRunner(TaskQueryRunner::Ptr const& taskQueryRunner) {
    _taskQueryRunner = taskQueryRunner;
    return checkCancelled();
}

void Task::freeTaskQueryRunner(TaskQueryRunner *tqr){
    if (_taskQueryRunner.get() == tqr) {
        _taskQueryRunner.reset();
    } else {
        LOGS(_log, LOG_LVL_WARN, "Task::freeTaskQueryRunner pointer didn't match!");
    }
}


/// Set values associated with the Task being put on the queue.
void Task::queued(std::chrono::system_clock::time_point const& now) {
    std::lock_guard<std::mutex> guard(_stateMtx);
    _state = State::QUEUED;
    _queueTime = now;
}


Task::State Task::getState() const {
    std::lock_guard<std::mutex> lock(_stateMtx);
    return _state;
}


/// Set values associated with the Task being started.
void Task::started(std::chrono::system_clock::time_point const& now) {
    std::lock_guard<std::mutex> guard(_stateMtx);
    _state = State::RUNNING;
    _startTime = now;
}


/// Set values associated with the Task being finished.
/// @return milliseconds to complete the Task, system clock time.
std::chrono::milliseconds Task::finished(std::chrono::system_clock::time_point const& now) {

    /// Freeing the ResourceMonitorLock should have already happened, but doing so
    /// again is harmless and it may have been missed if there was an exception.
    freeResourceMonitorLock();

    std::chrono::milliseconds duration;
    {
        std::lock_guard<std::mutex> guard(_stateMtx);
        _finishTime = now;
        _state = State::FINISHED;
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(_finishTime - _startTime);
    }
    // Ensure that the duration is greater than 0.
    if (duration.count() < 1) {
        duration = std::chrono::milliseconds{1};
    }
    LOGS(_log, LOG_LVL_DEBUG, "processing millisecs=" << duration.count());
    return duration;
}


/// @return the amount of time spent so far on the task in milliseconds.
std::chrono::milliseconds Task::getRunTime() const {
    std::chrono::milliseconds duration{0};
    {
        std::lock_guard<std::mutex> guard(_stateMtx);
        if (_state == State::FINISHED) {
            duration = std::chrono::duration_cast<std::chrono::milliseconds>(_finishTime - _startTime);
        } else if (_state == State::RUNNING) {
            auto now = std::chrono::system_clock::now();
            duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - _startTime);
        }
    }
    return duration;
}


/// Wait for MemMan to finish reserving resources. The mlock call can take several seconds
/// and only one mlock call can be running at a time. Further, queries finish slightly faster
/// if they are mlock'ed in the same order they were scheduled, hence the ulockEvents
/// EventThread and CommandMlock class.
void Task::waitForMemMan() {
    if (_memMan != nullptr) {
        if (_memMan->lock(_memHandle, true)) {
            int errorCode = (errno == EAGAIN ? ENOMEM : errno);
            LOGS(_log, LOG_LVL_WARN, "mlock err=" << errorCode <<
                    " " <<_memMan->getStatistics().logString() <<
                    " " << _memMan->getStatus(_memHandle).logString());
        }
        LOGS(_log, LOG_LVL_DEBUG, "waitForMemMan " <<_memMan->getStatistics().logString() <<
                                  " " << _memMan->getStatus(_memHandle).logString());
    }
    _safeToMoveRunning = true;
}


memman::MemMan::Status Task::getMemHandleStatus() {
    if (_memMan == nullptr || !hasMemHandle()) {
        return memman::MemMan::Status();
    }
    return _memMan->getStatus(_memHandle);
}


std::ostream& operator<<(std::ostream& os, Task const& t) {
    proto::TaskMsg const& m = t.msg;
    os << "Task: "
       << "msg: " << t._idStr
       << " session=" << m.session()
       << " chunk=" << m.chunkid()
       << " db=" << m.db()
       << " entry time=" << t.timestr
       << " ";
    for(int i=0; i < m.fragment_size(); ++i) {
        dump(os, m.fragment(i));
        os << " ";
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, IdSet const& idSet) {
    // Limiting output as number of entries can be very large.
    int maxDisp = idSet.maxDisp; // only affects the amount of data printed.
    std::lock_guard<std::mutex> lock(idSet.mx);
    os << "showing " << maxDisp << " of count=" << idSet._ids.size() << " ";
    bool first = true;
    int i = 0;
    for(auto id: idSet._ids) {
        if (!first) {
            os << ", ";
        } else {
            first = false;
        }
        os << id;
        if (++i >= maxDisp) break;
    }
    return os;
}

}}} // namespace lsst::qserv::wbase
