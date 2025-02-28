// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2016 LSST Corporation.
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
#include "wsched/BlendScheduler.h"

// System headers
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <sstream>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/LogContext.h"
#include "proto/worker.pb.h"
#include "util/Bug.h"
#include "util/EventThread.h"
#include "util/Timer.h"
#include "wcontrol/Foreman.h"
#include "wsched/GroupScheduler.h"
#include "wsched/ScanScheduler.h"

using namespace std;

template <class Sched>
inline Sched* other(Sched* notThis, Sched* a, Sched* b) {
    return (notThis == a) ? b : a;
}

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wsched.BlendScheduler");
}

namespace lsst::qserv::wsched {

BlendScheduler* dbgBlendScheduler = nullptr;  ///< A symbol for gdb

////////////////////////////////////////////////////////////////////////
// class BlendScheduler
////////////////////////////////////////////////////////////////////////

BlendScheduler::BlendScheduler(string const& name, wpublish::QueriesAndChunks::Ptr const& queries,
                               int schedMaxThreads, shared_ptr<GroupScheduler> const& group,
                               shared_ptr<ScanScheduler> const& snailScheduler,
                               vector<shared_ptr<ScanScheduler>> const& scanSchedulers)
        : SchedulerBase{name, 0, 0, 0, 0},
          _schedMaxThreads{schedMaxThreads},
          _group{group},
          _scanSnail{snailScheduler},
          _queries{queries} {
    dbgBlendScheduler = this;
    // If these are not defined, there is no point in continuing.
    assert(_group);
    assert(_scanSnail);
    int position = 0;
    _schedulers.push_back(_group);  // _group scheduler must be first in the list.
    _group->setDefaultPosition(position++);
    for (auto const& sched : scanSchedulers) {
        _schedulers.push_back(sched);
        sched->setDefaultPosition(position++);
    }
    _schedulers.push_back(_scanSnail);
    _scanSnail->setDefaultPosition(position++);
    assert(_schedulers.size() >= 2);  // Must have at least _group and _scanSnail in the list.
    _sortScanSchedulers();
    LOGS(_log, LOG_LVL_INFO, "BlendScheduler _schedMaxThreads=" << _schedMaxThreads);
    for (auto const& sched : _schedulers) {
        LOGS(_log, LOG_LVL_INFO, "Scheduler " << _name << " found scheduler " << sched->getName());
    }
}

void BlendScheduler::_sortScanSchedulers() {
    auto lessThan = [this](SchedulerBase::Ptr const& a, SchedulerBase::Ptr const& b) -> bool {
        // group scheduler is always first
        if (a == _group) return true;
        if (b == _group) return false;

        // snail scheduler is always last
        if (a == _scanSnail) return false;
        if (b == _scanSnail) return true;

        if (_prioritizeByInFlight) {
            // More scans in flight means lower priority.
            auto aInFlight = a->getInFlight() - a->getPriority();
            auto bInFlight = b->getInFlight() - b->getPriority();
            if (aInFlight < bInFlight) return true;
            if (bInFlight < aInFlight) return false;
        }

        /// Order by original position in the list
        return (a->getDefaultPosition() < b->getDefaultPosition());
    };

    string str = "sort:";
    {
        lock_guard<mutex> lg(_schedMtx);
        sort(_schedulers.begin(), _schedulers.end(), lessThan);

        for (auto const& sched : _schedulers) {
            str += sched->getName() + ", ";
        }
    }
    LOGS(_log, LOG_LVL_TRACE, str);
}

void BlendScheduler::queTaskLoad(util::Command::Ptr const& cmd) {
    {
        lock_guard guardA(util::CommandQueue::_mx);
        _taskLoadQueue.push_back(cmd);
    }
    notify(false);
}

void BlendScheduler::queCmd(util::Command::Ptr const& cmd) {
    std::vector<util::Command::Ptr> vect;
    vect.push_back(cmd);
    queCmd(vect);
}

void BlendScheduler::queCmd(std::vector<util::Command::Ptr> const& cmds) {
    // Do the book keeping and determine which queue the tasks in 'cmds'
    // should be added to. All tasks in 'cmds' must belong to the same
    // user query and will go to the same scheduler.
    bool first = true;
    wpublish::QueryStatistics::Ptr queryStats;
    std::vector<util::Command::Ptr> taskCmds;
    SchedulerBase::Ptr targSched = nullptr;
    bool onInteractive = false;
    for (auto const& cmd : cmds) {
        wbase::Task::Ptr task = dynamic_pointer_cast<wbase::Task>(cmd);
        if (task == nullptr) {
            // These should be few and far between.
            LOGS(_log, LOG_LVL_INFO, "BlendScheduler::queCmd got control command");
            if (cmds.size() > 1) {
                throw util::Bug(ERR_LOC, "BlendScheduler::queCmd cmds.size() > 1 when no task was set.");
            }
            {
                lock_guard guardA(util::CommandQueue::_mx);
                _ctrlCmdQueue.queCmd(cmd);
            }

            notify(true);  // notify all=true
            continue;
        }

        if (first) {
            QSERV_LOGCONTEXT_QUERY_JOB(task->getQueryId(), task->getJobId());
        }

        lock_guard guardB(util::CommandQueue::_mx);
        // Check for scan tables. The information for all tasks should be the same
        // as they all belong to the same query, so only examine the first task.
        if (first) {
            first = false;

            auto const& scanTables = task->getScanInfo()->infoTables;
            bool interactive = task->getScanInteractive();
            if (scanTables.size() <= 0 || interactive) {
                // If there are no scan tables, no point in putting on a shared scan.
                LOGS(_log, LOG_LVL_TRACE,
                     "Blend chose group scanTables.size=" << scanTables.size()
                                                          << " interactive=" << interactive);
                onInteractive = true;
                targSched = _group;
            } else {
                onInteractive = false;
                int scanPriority = task->getScanInfo()->scanRating;
                if (LOG_CHECK_LVL(_log, LOG_LVL_TRACE)) {
                    ostringstream ss;
                    ss << "Blend chose scan for priority=" << scanPriority << " : ";
                    for (auto scanTbl : scanTables) {
                        ss << scanTbl.db + "." + scanTbl.table + " ";
                    }
                    LOGS(_log, LOG_LVL_TRACE, ss.str());
                }
                {  // Find the scheduler responsible for this 'scanPriority'.
                    lock_guard<mutex> lg(_schedMtx);
                    for (auto const& sched : _schedulers) {
                        ScanScheduler::Ptr scan = dynamic_pointer_cast<ScanScheduler>(sched);
                        if (scan != nullptr) {
                            if (scan->isRatingInRange(scanPriority)) {
                                targSched = scan;
                                break;
                            }
                        }
                    }
                }
                // If the user query for this task has been booted, put this task on the snail scheduler.
                queryStats = _queries->getStats(task->getQueryId());
                if (queryStats && queryStats->getQueryBooted()) {
                    targSched = _scanSnail;
                }
                if (targSched == nullptr) {
                    // Task wasn't assigned with a scheduler, assuming it is terribly slow.
                    // Assign it to the slowest scheduler so it does the least damage to other queries.
                    LOGS_WARN("Task had unexpected scanRating=" << scanPriority << " adding to scanSnail");
                    targSched = _scanSnail;
                }
            }
        }
        task->setOnInteractive(onInteractive);
        task->setTaskScheduler(targSched);
        _queries->queuedTask(task);
        taskCmds.push_back(task);
        LOGS(_log, LOG_LVL_TRACE,
             "BlendScheduler::queCmd<vect> added tid=" << task->getIdStr()
                                                       << " sched=" << targSched->getName());
    }

    _logSchedulers();

    if (!taskCmds.empty()) {
        LOGS(_log, LOG_LVL_TRACE, "Blend queCmd");
        targSched->queCmd(taskCmds);
        if (queryStats) {
            queryStats->tasksAddedToScheduler(targSched, taskCmds.size());
        }
        _infoChanged = true;
        notify(true);  // notify all=true
    }
}

void BlendScheduler::commandStart(util::Command::Ptr const& cmd) {
    auto t = dynamic_pointer_cast<wbase::Task>(cmd);
    if (t == nullptr) {
        // This happens with loader and control commands.
        LOGS(_log, LOG_LVL_TRACE, "BlendScheduler::commandStart cmd not a Task");
        return;
    }

    QSERV_LOGCONTEXT_QUERY_JOB(t->getQueryId(), t->getJobId());

    LOGS(_log, LOG_LVL_TRACE, "BlendScheduler::commandStart");
    wcontrol::Scheduler::Ptr s = dynamic_pointer_cast<wcontrol::Scheduler>(t->getTaskScheduler());
    if (s != nullptr) {
        s->commandStart(t);
    } else {
        LOGS(_log, LOG_LVL_ERROR, "BlendScheduler::commandStart scheduler not found");
    }
    _infoChanged = true;
}

void BlendScheduler::commandFinish(util::Command::Ptr const& cmd) {
    auto t = dynamic_pointer_cast<wbase::Task>(cmd);
    if (t == nullptr) {
        LOGS(_log, LOG_LVL_TRACE, "BlendScheduler::commandFinish cmd is not a Task");
        return;
    }

    QSERV_LOGCONTEXT_QUERY_JOB(t->getQueryId(), t->getJobId());

    wcontrol::Scheduler::Ptr s = dynamic_pointer_cast<wcontrol::Scheduler>(t->getTaskScheduler());
    LOGS(_log, LOG_LVL_TRACE, "BlendScheduler::commandFinish");
    if (s != nullptr) {
        s->commandFinish(t);
    } else {
        LOGS(_log, LOG_LVL_ERROR, "BlendScheduler::commandFinish scheduler not found");
    }
    _infoChanged = true;
    if (LOG_CHECK_LVL(_log, LOG_LVL_TRACE)) {
        _logChunkStatus();
    }
    notify(false);  // notify one
}

bool BlendScheduler::ready() {
    bool ready = false;
    {
        util::LockGuardTimed guard(util::CommandQueue::_mx, "BlendScheduler::ready");
        ready = _ready();
    }
    if (ready) {
        notify(false);  // notify all=false
    }
    return ready;
}

/// Returns true when any sub-scheduler has a command ready.
/// Precondition util::CommandQueue::_mx must be locked when this is called.
bool BlendScheduler::_ready() {
    ostringstream os;
    bool ready = false;

    if (_taskLoadQueue.size() > 0) {
        ready = true;
        return ready;
    }

    // _readSched points to the scheduler with a ready task until that
    // task has been retrieved by getCmd().
    if (_readySched != nullptr) {
        ready = true;
    }
    bool changed = _infoChanged.exchange(false);

    if (!ready) {
        lock_guard<mutex> lg(_schedMtx);

        // Get the total number of threads schedulers want reserved
        int availableThreads = _calcAvailableTheads();
        for (auto const& sched : _schedulers) {
            availableThreads = sched->applyAvailableThreads(availableThreads);
            ready = sched->ready();
            if (changed && LOG_CHECK_LVL(_log, LOG_LVL_TRACE)) {
                os << sched->getName() << "(r=" << ready << " sz=" << sched->getSize()
                   << " fl=" << sched->getInFlight() << " avail=" << availableThreads << ") ";
            }
            if (ready) {
                _readySched = sched;
                break;
            }
        }
    }

    // If nothing ready on the schedulers, check if the thread pool size should be changed.
    if (!ready) {
        ready = _ctrlCmdQueue.ready();
    }
    if (changed) {
        LOGS(_log, LOG_LVL_TRACE, getName() << "_ready() " << os.str());
    }
    return ready;
}

atomic<uint16_t> logChunkLimiter = 0;

util::Command::Ptr BlendScheduler::getCmd(bool wait) {
    util::Command::Ptr cmd;
    bool ready = false;
    {
        unique_lock<mutex> lock(util::CommandQueue::_mx);
        if (wait) {
            util::CommandQueue::_cv.wait(lock, [this]() { return _ready(); });
            ready = true;
        } else {
            ready = _ready();
        }

        if (ready && _taskLoadQueue.size() > 0) {
            cmd = _taskLoadQueue.front();
            _taskLoadQueue.pop_front();
            notify(false);
            return cmd;
        }

        _logSchedulers();

        // Try to get a command from the schedulers
        if (ready && (_readySched != nullptr)) {
            cmd = _readySched->getCmd(false);
            if (cmd != nullptr) {
                _sortScanSchedulers();
                if (LOG_CHECK_LVL(_log, LOG_LVL_TRACE)) {
                    wbase::Task::Ptr task = dynamic_pointer_cast<wbase::Task>(cmd);
                    LOGS(_log, LOG_LVL_TRACE,
                         "Blend getCmd() using cmd from " << _readySched->getName()
                                                          << " chunkId=" << task->getChunkId()
                                                          << " QID=" << task->getIdStr());
                }
            }
            _readySched.reset();
        }
    }

    if (cmd == nullptr) {
        // The scheduler didn't have anything, see if there's anything on the control queue,
        // which could change the size of the pool.
        cmd = _ctrlCmdQueue.getCmd();
    }

    if (cmd != nullptr) {
        _infoChanged = true;
        if (LOG_CHECK_LVL(_log, LOG_LVL_TRACE) || (logChunkLimiter++ % 100 == 0)) {
            _logChunkStatus();
        }
        notify(false);  // notify all=false
    }
    // returning nullptr is acceptable.
    return cmd;
}

/// Method A - maybe use with MemManReal
int BlendScheduler::_getAdjustedMaxThreads(int oldAdjMax, int inFlight) {
    int newAdjMax = oldAdjMax - max(inFlight - 1, 0);
    if (newAdjMax < 1) {
        LOGS(_log, LOG_LVL_ERROR, "_getAdjustedMaxThreadsgetCmd() too low newAdjMax=" << newAdjMax);
        newAdjMax = 1;
    }
    return newAdjMax;
}

/// @return the number of threads that are not reserved by any sub-scheduler.
int BlendScheduler::_calcAvailableTheads() {
    int reserve = 0;
    {
        for (auto const& sched : _schedulers) {
            reserve += sched->desiredThreadReserve();
        }
    }
    int available = _schedMaxThreads - reserve;
    if (available < 0) {
        LOGS(_log, LOG_LVL_TRACE, "calcAvailableTheads negative available=" << available);
    }
    return available;
}

/// Returns the number of Tasks queued in all sub-schedulers.
size_t BlendScheduler::getSize() const {
    size_t sz = 0;
    lock_guard<mutex> lg(_schedMtx);
    for (auto const& sched : _schedulers) {
        sz += sched->getSize();
    }
    return sz;
}

/// Returns the number of Tasks inFlight.
int BlendScheduler::getInFlight() const {
    int inFlight = 0;
    lock_guard<mutex> lg(_schedMtx);
    for (auto const& sched : _schedulers) {
        inFlight += sched->getInFlight();
    }
    return inFlight;
}

void BlendScheduler::_logChunkStatus() {
    if (LOG_CHECK_LVL(_log, LOG_LVL_INFO)) {
        string str;
        {
            lock_guard<mutex> lg(_schedMtx);
            for (auto const& sched : _schedulers) {
                if (sched != nullptr) str += sched->chunkStatusStr() + " ";
            }
        }
        LOGS(_log, LOG_LVL_INFO, str);
    }
}

nlohmann::json BlendScheduler::statusToJsonBlend() {
    nlohmann::json status;
    status["name"] = getName();
    status["priority"] = getPriority();
    status["num_tasks_in_queue"] = getSize();
    status["num_tasks_in_flight"] = getInFlight();
    nlohmann::json schedulers = nlohmann::json::array();
    {
        lock_guard<mutex> lg(_schedMtx);
        for (auto&& sched : _schedulers) {
            schedulers.push_back(sched->statusToJsonBase());
        }
    }
    status["schedulers"] = schedulers;
    return status;
}

bool BlendScheduler::isScanSnail(SchedulerBase::Ptr const& scan) { return scan == _scanSnail; }

int BlendScheduler::moveUserQueryToSnail(QueryId qId, SchedulerBase::Ptr const& source) {
    if (source == _scanSnail) {
        LOGS(_log, LOG_LVL_INFO,
             QueryIdHelper::makeIdStr(qId) << " moveUserQueryToSnail can't move, query is already on snail.");
        // TODO: send a message back to czar asking to cancel query
        return 0;
    }
    return moveUserQuery(qId, source, _scanSnail);
}

int BlendScheduler::moveUserQuery(QueryId qId, SchedulerBase::Ptr const& source,
                                  SchedulerBase::Ptr const& destination) {
    LOGS(_log, LOG_LVL_DEBUG,
         __func__ << " " << QueryIdHelper::makeIdStr(qId)
                  << " source=" << ((source == nullptr) ? "NULL" : source->getName())
                  << " dest=" << ((destination == nullptr) ? "NULL" : destination->getName()));
    int count = 0;  // Number of Tasks that were moved.
    if (destination == nullptr) {
        LOGS(_log, LOG_LVL_WARN,
             QueryIdHelper::makeIdStr(qId) << " moveUserQuery destination can not be nullptr");
        return count;
    }
    // Go through the Tasks in the query and remove any that are not already on the 'destination'.
    auto taskList = _queries->removeQueryFrom(qId, source);
    // Add the tasks in taskList to 'destination'. taskList only contains tasks that were on the queue,
    // not tasks that were running.
    for (auto const& task : taskList) {
        // Change the scheduler to the new scheduler as normally this is done in BlendScheduler::queCmd
        LOGS(_log, LOG_LVL_INFO,
             __func__ << " task=" << task->getIdStr() << " moving from "
                      << task->getTaskScheduler()->getName() << " to " << destination->getName());
        task->setTaskScheduler(destination);
        destination->queCmd(task);
        ++count;
    }
    return count;
}

void BlendScheduler::_logSchedulers() {
    chrono::system_clock::time_point now = chrono::system_clock::now();
    if (now > _nextRecordPerformanceTime) {
        _nextRecordPerformanceTime = now + _intervalRecordPerformanceTime;
        recordPerformanceData();
    }
}

void ControlCommandQueue::queCmd(util::Command::Ptr const& cmd) {
    lock_guard<mutex> lock{_mx};
    _qu.push_back(cmd);
}

util::Command::Ptr ControlCommandQueue::getCmd() {
    lock_guard<mutex> lock{_mx};
    if (_qu.empty()) {
        return nullptr;
    }
    auto cmd = _qu.front();
    _qu.pop_front();
    return cmd;
}

bool ControlCommandQueue::ready() {
    lock_guard<mutex> lock{_mx};
    return !_qu.empty();
}

}  // namespace lsst::qserv::wsched
