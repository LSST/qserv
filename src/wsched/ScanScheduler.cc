// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2019 LSST.
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
 * @brief A scheduler implementation that limits disk scans to one at
 * a time, but allows multiple queries to share I/O.
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header

#include "wsched/ScanScheduler.h"

// System headers
#include <cstddef>
#include <iostream>
#include <mutex>
#include <sstream>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/LogContext.h"
#include "util/Bug.h"
#include "util/Timer.h"
#include "wcontrol/Foreman.h"
#include "wsched/BlendScheduler.h"
#include "wsched/ChunkTasksQueue.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wsched.ScanScheduler");
}

using namespace std;

namespace lsst::qserv::wsched {

ScanScheduler::ScanScheduler(string const& name, int maxThreads, int maxReserve, int priority,
                             int maxActiveChunks, memman::MemMan::Ptr const& memMan, int minRating,
                             int maxRating, double maxTimeMinutes)
        : SchedulerBase{name, maxThreads, maxReserve, maxActiveChunks, priority},
          _memMan{memMan},
          _minRating{minRating},
          _maxRating{maxRating},
          _maxTimeMinutes{maxTimeMinutes} {
    _taskQueue = make_shared<ChunkTasksQueue>(this, _memMan);
    assert(_minRating <= _maxRating);
}

void ScanScheduler::commandStart(util::Command::Ptr const& cmd) {
    _infoChanged = true;
    auto logLvl = LOG_LVL_TRACE;
    if (LOG_CHECK_LVL(_log, logLvl)) {
        wbase::Task::Ptr task = dynamic_pointer_cast<wbase::Task>(cmd);
        if (task == nullptr) {
            LOGS(_log, LOG_LVL_WARN, "ScanScheduler::commandStart cmd failed conversion " << getName());
            return;
        }
        QSERV_LOGCONTEXT_QUERY_JOB(task->getQueryId(), task->getJobId());
        LOGS(_log, logLvl, "commandStart " << getName() << " task=" << task->getIdStr());
    }
    // task was registered Inflight when getCmd() was called.
}

void ScanScheduler::commandFinish(util::Command::Ptr const& cmd) {
    wbase::Task::Ptr task = dynamic_pointer_cast<wbase::Task>(cmd);
    _infoChanged = true;
    if (task == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "ScanScheduler::commandFinish cmd failed conversion " << getName());
        return;
    }

    QSERV_LOGCONTEXT_QUERY_JOB(task->getQueryId(), task->getJobId());
    LOGS(_log, LOG_LVL_TRACE, __func__ << " " << getName() << " task=" << task->getIdStr());

    _taskQueue->taskComplete(task);  // does not need _mx protection.
    {
        lock_guard<mutex> guard(util::CommandQueue::_mx);
        --_inFlight;
        ++_recentlyCompleted;
        LOGS(_log, LOG_LVL_TRACE,
             "commandFinish " << getName() << " inFlight=" << _inFlight << " " << task->getIdStr());

        // If there's an old _memManHandleToUnlock, it needs to be unlocked before a new value is assigned.
        if (_memManHandleToUnlock != memman::MemMan::HandleType::INVALID) {
            LOGS(_log, LOG_LVL_TRACE,
                 "ScanScheduler::commandFinish unlocking handle=" << _memManHandleToUnlock);
            _memMan->unlock(_memManHandleToUnlock);
            _memManHandleToUnlock = memman::MemMan::HandleType::INVALID;
        }

        // Wait to unlock the tables until after the next call to _ready or commandFinish.
        // This is done in case only one thread is running on this scheduler as
        // we don't want to release the tables in case the next Task wants some of them.
        if (!_taskQueue->empty()) {
            _memManHandleToUnlock = task->getMemHandle();
            LOGS(_log, LOG_LVL_TRACE, "setting handleToUnlock handle=" << _memManHandleToUnlock);
        } else {
            LOGS(_log, LOG_LVL_TRACE,
                 "ScanScheduler::commandFinish unlocking handle=" << task->getMemHandle());
            _memMan->unlock(task->getMemHandle());  // Nothing on the queue, no reason to wait.
        }

        _decrChunkTaskCount(task->getChunkId());
    }
    LOGS(_log, LOG_LVL_TRACE, "tskEnd chunk=" << task->getChunkId() << " " << task->getIdStr());
    // Whenever a Task finishes, sleeping threads need to check if resources
    // are available to run new Tasks.
    _cv.notify_one();
}

/// Returns true if there is a Task ready to go and we aren't up against any limits.
bool ScanScheduler::ready() {
    lock_guard<mutex> lock(util::CommandQueue::_mx);
    return _ready();
}

/// Precondition: _mx is locked
/// Returns true if there is a Task ready to go and we aren't up against any limits.
bool ScanScheduler::_ready() {
    bool logStuff = false;
    if (_infoChanged) {
        _infoChanged = false;
        logStuff = true;
        LOGS(_log, LOG_LVL_TRACE,
             getName() << " ScanScheduler::_ready "
                       << " inFlight=" << _inFlight << " maxThreads=" << _maxThreads
                       << " adj=" << _maxThreadsAdj << " activeChunks=" << getActiveChunkCount()
                       << _taskQueue->queueInfo());
    }
    if (_inFlight >= maxInFlight()) {
        if (logStuff) {
            LOGS(_log, LOG_LVL_TRACE, getName() << " ScanScheduler::_ready too many in flight " << _inFlight);
        }
        return false;
    }

    bool useFlexibleLock = (_inFlight < 1);
    /// Once _taskQueue->ready() has a task ready, it stays on that task until it is used by getTask().
    auto rdy = _taskQueue->ready(useFlexibleLock);  // Only returns true if MemMan grants resources.
    bool logMemStats = false;
    // If ready failed, holding on to this is unlikely to help, otherwise the new Task now has its own handle
    // which and will keep needed files in memory.
    if (_memManHandleToUnlock != memman::MemMan::HandleType::INVALID) {
        LOGS(_log, LOG_LVL_TRACE,
             "ScanScheduler::_ready unlocking handle="
                     << _memManHandleToUnlock << " "
                     << _memMan->getStatus(_memManHandleToUnlock).logString());
        _memMan->unlock(_memManHandleToUnlock);
        _memManHandleToUnlock = memman::MemMan::HandleType::INVALID;
        logMemStats = true;
        if (!rdy) {
            // Try again now that memory is freed
            rdy = _taskQueue->ready(useFlexibleLock);  // Only returns true if MemMan grants resources.
        }
    }
    if (rdy || logMemStats) {
        logMemManStats();
    }
    return rdy;
}

size_t ScanScheduler::getSize() const {
    lock_guard<mutex> lock(util::CommandQueue::_mx);
    return _taskQueue->getSize();
}

util::Command::Ptr ScanScheduler::getCmd(bool wait) {
    unique_lock<mutex> lock(util::CommandQueue::_mx);
    LOGS(_log, LOG_LVL_TRACE, "start getCmd " << getName() << " " << _taskQueue->queueInfo());
    if (wait) {
        util::CommandQueue::_cv.wait(lock, [this]() { return _ready(); });
    } else if (!_ready()) {
        return nullptr;
    }
    bool useFlexibleLock = (_inFlight < 1);
    auto task = _taskQueue->getTask(useFlexibleLock);
    if (task != nullptr) {
        ++_inFlight;  // in flight as soon as it is off the queue.
        QSERV_LOGCONTEXT_QUERY_JOB(task->getQueryId(), task->getJobId());
        LOGS(_log, LOG_LVL_TRACE,
             "getCmd " << getName() << " tskStart chunk=" << task->getChunkId() << " tid=" << task->getIdStr()
                       << " inflight=" << _inFlight << _taskQueue->queueInfo());
        _infoChanged = true;
        _decrCountForUserQuery(task->getQueryId());
        _incrChunkTaskCount(task->getChunkId());
        // Since a command was retrieved, there's a possibility another is ready.
        notify(false);  // notify all=false
    }
    return task;
}

void ScanScheduler::queCmd(util::Command::Ptr const& cmd) {
    vector<util::Command::Ptr> vect;
    vect.push_back(cmd);
    queCmd(vect);
}

void ScanScheduler::queCmd(vector<util::Command::Ptr> const& cmds) {
    LOGS(_log, LOG_LVL_TRACE, "ScanScheduler::queCmd cmds.sz=" << cmds.size());
    std::vector<wbase::Task::Ptr> tasks;
    bool first = true;
    QueryId qid;
    int jid = 0;
    // Convert to a vector of tasks
    for (auto const& cmd : cmds) {
        wbase::Task::Ptr tsk = dynamic_pointer_cast<wbase::Task>(cmd);
        if (tsk == nullptr) {
            throw util::Bug(ERR_LOC, getName() + " queCmd could not be converted to Task or was nullptr");
        }
        if (first) {
            first = false;
            qid = tsk->getQueryId();
            jid = tsk->getJobId();
            QSERV_LOGCONTEXT_QUERY_JOB(qid, jid);
        } else {
            if (qid != tsk->getQueryId()) {
                string eMsg("Mismatch multiple query/job ids in single queCmd ");
                eMsg += " expected QID=" + to_string(qid) + " got=" + to_string(tsk->getQueryId());
                eMsg += " expected JID=" + to_string(qid) + " got=" + to_string(tsk->getJobId());
                LOGS(_log, LOG_LVL_ERROR, eMsg);
                // This could cause difficult to detect problems later on.
                throw util::Bug(ERR_LOC, eMsg);
                return;
            }
        }
        tsk->setMemMan(_memMan);
        tasks.push_back(tsk);
        LOGS(_log, LOG_LVL_TRACE, getName() << " queCmd " << tsk->getIdStr());
    }
    // Queue the tasks
    {
        lock_guard<mutex> lock(util::CommandQueue::_mx);
        auto uqCount = _incrCountForUserQuery(qid, tasks.size());
        LOGS(_log, LOG_LVL_TRACE, getName() << " queCmd " << " uqCount=" << uqCount);
        _taskQueue->queueTask(tasks);
        _infoChanged = true;
    }

    if (cmds.size() > 1) {
        util::CommandQueue::_cv.notify_all();
    } else {
        util::CommandQueue::_cv.notify_one();
    }
}

/// @returns - true if a task was removed from the queue. If the task was running
///            or not found, false is returned. A return value of true indicates
///            that the task still needs to be started.
/// If the task is running: the task continues to run, but the scheduler is told
/// it is finished (this allows the scheduler to move on), and its thread is removed
/// from the thread pool (the thread pool creates a new thread to replace it).
bool ScanScheduler::removeTask(wbase::Task::Ptr const& task, bool removeRunning) {
    QSERV_LOGCONTEXT_QUERY_JOB(task->getQueryId(), task->getJobId());

    LOGS(_log, LOG_LVL_INFO, __func__ << " " << getName());
    // Check if task is in the queue.
    // _taskQueue has its own mutex to protect this.
    auto rmTask = _taskQueue->removeTask(task);
    bool inQueue = rmTask != nullptr;
    LOGS(_log, LOG_LVL_DEBUG, "removeTask inQueue=" << inQueue);
    if (inQueue) {
        LOGS(_log, LOG_LVL_INFO, "removeTask moving task on queue");
        _decrCountForUserQuery(task->getQueryId());
        return true;
    }

    LOGS(_log, LOG_LVL_DEBUG, "removeTask not in queue");
    // Wasn't in the queue, could be in flight.
    if (!removeRunning) {
        LOGS(_log, LOG_LVL_DEBUG, "removeTask not removing running tasks");
        return false;
    }
    // Removing the task before we're done with MemMan could cause undefined behavior.
    if (!task->getSafeToMoveRunning()) {
        LOGS(_log, LOG_LVL_WARN, "removeTask couldn't move as still waiting on MemMan");
        return false;
    }

    /// Don't remove the task if there are already too many threads in existence.
    if (task->atMaxThreadCount()) {
        LOGS(_log, LOG_LVL_WARN, "removeTask couldn't move as too many threads existing");
        return false;
    }

    /// The task can only leave the pool if it has been started, poolThread should be set as
    /// it is safe to move the running task according to the test above.
    auto poolThread = task->getAndNullPoolEventThread();
    if (poolThread != nullptr) {
        LOGS(_log, LOG_LVL_INFO, "removeTask moving running task");
        return poolThread->leavePool(task);
    } else {
        LOGS(_log, LOG_LVL_DEBUG,
             "removeTask PoolEventThread was null, "
             "presumably already moved for large result.");
    }
    return false;
}

void ScanScheduler::logMemManStats() {
    LOGS(_log, LOG_LVL_TRACE, "Scan " << _memMan->getStatistics().logString());
}

}  // namespace lsst::qserv::wsched
