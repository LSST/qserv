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
#include "wsched/GroupScheduler.h"

// System headers
#include <iostream>
#include <mutex>
#include <sstream>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/Bug.h"
#include "global/LogContext.h"
#include "proto/worker.pb.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wsched.GroupScheduler");
}

namespace lsst {
namespace qserv {
namespace wsched {


GroupQueue::GroupQueue(int maxAccepted, wbase::Task::Ptr const& task) : _maxAccepted{maxAccepted} {
    assert(task != nullptr);
    _hasChunkId = task->msg.has_chunkid();
    if (_hasChunkId) {
        _chunkId = task->msg.chunkid();
    }
    assert(queTask(task));
}

/// Return true if this GroupQueue accepts this task.
/// The task is acceptable if has the same chunk id.
bool GroupQueue::queTask(wbase::Task::Ptr const& task) {
    /// Not having a chunk id is considered an id.
    auto hasChunkId = task->msg.has_chunkid();
    if (hasChunkId != _hasChunkId) {
        return false; // Reject as one has a chunk id and the other does not.
    }
    auto chunkId = task->msg.chunkid();
    if (hasChunkId && chunkId != _chunkId) {
        return false; // Reject since chunk ids don't match.
    }
    // Accept if not already full
    if (_accepted < _maxAccepted) {
        ++_accepted;
        _tasks.push_back(task);
        return true;
    }
    return false;
}

/// Get a command off the queue. If no message is available, wait until one is.
wbase::Task::Ptr GroupQueue::getTask() {
    auto task = _tasks.front();
    _tasks.pop_front();
    return task;
}

wbase::Task::Ptr GroupQueue::peekTask() {
    return _tasks.front();
}


/// Queue a Task in the GroupScheduler.
/// Tasks in the same chunk are grouped together.
void GroupScheduler::queCmd(util::Command::Ptr const& cmd) {
    wbase::Task::Ptr t = std::dynamic_pointer_cast<wbase::Task>(cmd);
    if (t == nullptr) {
        LOGS(_log, LOG_LVL_WARN, getName() << " queCmd could not be converted to Task or was nullptr");
        return;
    }
    QSERV_LOGCONTEXT_QUERY_JOB(t->getQueryId(), t->getJobId());
    std::lock_guard<std::mutex> lock(util::CommandQueue::_mx);
    // Start at the front of the queue looking for a group to accept the task.
    bool queued = false;
    for(auto iter = _queue.begin(), end = _queue.end(); iter != end && !queued; ++iter) {
        GroupQueue::Ptr group = *iter;
        if (group->queTask(t)) {
            queued = true;
        }
    }
    if (!queued) {
        // Wasn't inserted into an existing group, need to make a new group.
        auto group = std::make_shared<GroupQueue>(_maxGroupSize, t);
        _queue.push_back(group);
    }
    auto uqCount = _incrCountForUserQuery(t->getQueryId());
    LOGS(_log, LOG_LVL_DEBUG, getName() << " queCmd uqCount=" << uqCount
        << " rating=" << t->getScanInfo().scanRating << " interactive=" << t->getScanInteractive());
    util::CommandQueue::_cv.notify_one();
}


void GroupScheduler::queCmd(std::vector<util::Command::Ptr> const& cmds) {
    for (auto const& cmd:cmds) {
        queCmd(cmd);
    }
}

/// Return a Task from the front of the queue. If no message is available, wait until one is.
util::Command::Ptr GroupScheduler::getCmd(bool wait)  {
    std::unique_lock<std::mutex> lock(util::CommandQueue::_mx);
    if (wait) {
        util::CommandQueue::_cv.wait(lock, [this](){return _ready();});
    } else if (!_ready()) {
        return nullptr;
    }
    auto group = _queue.front();
    auto task = group->getTask();
    if (group->isEmpty()) {
        _queue.pop_front();
    }
    ++_inFlight; // Considered inFlight as soon as it's off the queue.
    _decrCountForUserQuery(task->getQueryId());
    _incrChunkTaskCount(task->getChunkId());
    LOGS(_log, LOG_LVL_DEBUG, "GroupSched tskStart task=" << task->getIdStr()
                           << " chunk=" << task->getChunkId());
    return task;
}


void GroupScheduler::commandFinish(util::Command::Ptr const& cmd) {
    --_inFlight;
    auto t = std::dynamic_pointer_cast<wbase::Task>(cmd);
    if (t != nullptr) _decrChunkTaskCount(t->getChunkId());
    LOGS(_log, LOG_LVL_DEBUG, "GroupSched tskEnd task=" << t->getIdStr() << " chunk=" << t->getChunkId());
}


/// MaxActiveChunks and resource limitations (aside from available threads) are ignored by the GroupScheduler.
GroupScheduler::GroupScheduler(std::string const& name, int maxThreads, int maxReserve, int maxGroupSize, int priority)
  : SchedulerBase{name, maxThreads, maxReserve, 0, priority}, _maxGroupSize{maxGroupSize} {
}

bool GroupScheduler::empty() {
    std::lock_guard<std::mutex> lock(util::CommandQueue::_mx);
    return _queue.empty();
}

/// Returns true when a Task is ready to run.
bool GroupScheduler::ready() {
    std::lock_guard<std::mutex> lock(util::CommandQueue::_mx);
    return _ready();
}

/// Precondition: _mx must be locked.
bool GroupScheduler::_ready() {
    // GroupScheduler is not limited by resource availability and ignores maxActiveChunks.
    return !_queue.empty() && _inFlight < maxInFlight();
}


/// Return the number of groups (not Tasks) in the queue.
std::size_t GroupScheduler::getSize() const {
    std::lock_guard<std::mutex> lock(util::CommandQueue::_mx);
    return _queue.size();
}

}}} // namespace lsst::qserv::wsched
