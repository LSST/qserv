// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2016 LSST Corporation.
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

// Class header
#include "ChunkTasksQueue.h"

#include "util/Bug.h"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wsched.ChunkTasksQueue");
}

namespace lsst::qserv::wsched {

/// Queue tasks from an uberjob.
void ChunkTasksQueue::queueTask(std::vector<wbase::Task::Ptr> const& tasks) {
    std::lock_guard<std::mutex> lg(_mapMx);
    auto iter = _chunkMap.end();
    int prevChunkId = -1;  // invalid chunkId number
    for (auto const& task : tasks) {
        int chunkId = task->getChunkId();
        // If it's the first time through, or the chunkId is different than the previous one, then
        // find the correct ChunkTask. UberJobs are constructed in a way that makes it likely
        // that subchunks for the same chunk will be grouped together in `tasks`.
        if (iter == _chunkMap.end() || prevChunkId != chunkId) {
            prevChunkId = chunkId;
            iter = _chunkMap.find(chunkId);
            if (iter == _chunkMap.end()) {
                // Correct ChunkTask wasn't found, make a new one.
                std::pair<int, ChunkTasks::Ptr> ele(chunkId, std::make_shared<ChunkTasks>(chunkId, _memMan));
                auto res = _chunkMap.insert(ele);  // insert should fail if the key already exists.
                LOGS(_log, LOG_LVL_TRACE, " queueTask chunk=" << chunkId << " created=" << res.second);
                iter = res.first;
            }
        }
        ++_taskCount;
        iter->second->queTask(task);
    }
}

/// Queue a Task with other tasks on the same chunk.
void ChunkTasksQueue::queueTask(wbase::Task::Ptr const& task) {
    std::vector<wbase::Task::Ptr> vect;
    vect.push_back(task);
    queueTask(vect);
}

/// @return true if this object is ready to provide a Task from its queue.
bool ChunkTasksQueue::ready(bool useFlexibleLock) {
    std::lock_guard<std::mutex> lock(_mapMx);
    return _ready(useFlexibleLock);
}

/// Precondition: _queueMutex must be locked
/// @return true if this object is ready to provide a Task from its queue with _readyChunk pointing
///         to a chunk with a Task that is ready to run.
///         When returning false, _readyChunk will be nullptr.
/// This function starts checking at the _activeChunk and only looks to the next chunk
/// if there are no tasks to run on the current chunk. It continues through the list until
/// all chunks have been checked, a ready task is found, or there are not enough resources to
/// run the next Task on the current chunk.
/// The _activeChunk advances when all of its Tasks have completed.
bool ChunkTasksQueue::_ready(bool useFlexibleLock) {
    if (_readyChunk != nullptr) {
        return true;
    }
    if (_empty()) {
        return false;
    }

    // If the _activeChunk is invalid, start at the beginning.
    if (_activeChunk == _chunkMap.end()) {
        LOGS(_log, LOG_LVL_DEBUG, "ChunkTasksQueue::_ready _activeChunk invalid, reset");
        _activeChunk = _chunkMap.begin();
        _activeChunk->second->setActive();  // Flag tasks on active so new Tasks added wont be run.
    }

    // Check the active chunk for valid Tasks
    if (_activeChunk->second->ready(useFlexibleLock) == ChunkTasks::ReadyState::READY) {
        _readyChunk = _activeChunk->second;
        return true;
    }

    // Should the active chunk be advanced?
    if (_activeChunk->second->readyToAdvance()) {
        LOGS(_log, LOG_LVL_TRACE, "ChunkTasksQueue::_ready advancing chunk");
        auto newActive = _activeChunk;
        ++newActive;
        if (newActive == _chunkMap.end()) {
            newActive = _chunkMap.begin();
        }

        // Clean up the old _active chunk before moving on.
        _activeChunk->second->setActive(false);  // This should move pending Tasks to _activeTasks
        // _inFlightTasks must be empty as readyToAdvance was true.
        if (_activeChunk->second->empty()) {
            if (newActive == _activeChunk) {
                newActive = _chunkMap.end();
            }
            _chunkMap.erase(_activeChunk);
        }

        _activeChunk = newActive;
        if (newActive == _chunkMap.end()) {
            // _chunkMap is empty.
            return false;
        }
        newActive->second->movePendingToActive();
        newActive->second->setActive();
    }

    // Advance through chunks until READY or NO_RESOURCES found, or until entire list scanned.
    auto iter = _activeChunk;
    ChunkTasks::ReadyState chunkState = iter->second->ready(useFlexibleLock);
    LOGS(_log, LOG_LVL_TRACE,
         "_ready loopA state=" << ChunkTasks::toStr(chunkState) << " iter=" << iter->first << " "
                               << iter->second->cInfo());
    while (chunkState != ChunkTasks::ReadyState::READY &&
           chunkState != ChunkTasks::ReadyState::NO_RESOURCES) {
        ++iter;
        if (iter == _chunkMap.end()) {
            iter = _chunkMap.begin();
        }
        if (iter == _activeChunk) {
            // All chunks have been checked, give up.
            return false;
        }
        // Limit the number of chunks being queried on this scheduler.
        if (_scheduler != nullptr && _scheduler->getActiveChunkCount() >= _scheduler->getMaxActiveChunks()) {
            int newChunkId = iter->second->getChunkId();
            if (!_scheduler->chunkAlreadyActive(newChunkId)) {
                return false;
            }
        }
        chunkState = iter->second->ready(useFlexibleLock);
    }
    LOGS(_log, LOG_LVL_TRACE,
         "_ready loopB state=" << ChunkTasks::toStr(chunkState) << " iter=" << iter->first << " "
                               << iter->second->cInfo());
    if (chunkState == ChunkTasks::ReadyState::NO_RESOURCES) {
        // Advancing past a chunk where there aren't enough resources could cause many
        // scheduling issues.
        return false;
    }
    _readyChunk = iter->second;
    return true;
}

wbase::Task::Ptr ChunkTasksQueue::getTask(bool useFlexibleLock) {
    std::lock_guard<std::mutex> lock(_mapMx);
    // Attempt to set _readyChunk.
    _ready(useFlexibleLock);
    // If a Task was ready, _readyChunk will not be nullptr.
    if (_readyChunk != nullptr) {
        wbase::Task::Ptr task = _readyChunk->getTask(useFlexibleLock);
        _readyChunk = nullptr;
        --_taskCount;
        return task;
    }
    return nullptr;
}

/// This is called when a Task finishes.
void ChunkTasksQueue::taskComplete(wbase::Task::Ptr const& task) {
    std::lock_guard<std::mutex> lock(_mapMx);
    auto iter = _chunkMap.find(task->getChunkId());
    if (iter != _chunkMap.end()) {
        iter->second->taskComplete(task);
    }
}

bool ChunkTasksQueue::setResourceStarved(bool starved) {
    bool ret = _resourceStarved;
    _resourceStarved = starved;
    return ret;
}

int ChunkTasksQueue::getActiveChunkId() {
    std::lock_guard<std::mutex> lock(_mapMx);
    if (_activeChunk == _chunkMap.end()) {
        return -1;
    }
    return _activeChunk->second->getChunkId();
}

wbase::Task::Ptr ChunkTasksQueue::removeTask(wbase::Task::Ptr const& task) {
    LOGS(_log, LOG_LVL_DEBUG, __func__ << task->getIdStr());
    // Find the correct chunk
    auto chunkId = task->getChunkId();
    std::lock_guard<std::mutex> lock(_mapMx);
    auto iter = _chunkMap.find(chunkId);
    if (iter == _chunkMap.end()) return nullptr;

    // Erase the task if it is in the chunk
    ChunkTasks::Ptr ct = iter->second;
    auto ret = ct->removeTask(task);
    if (ret != nullptr) {
        --_taskCount;  // Need to do this as getTask() wont be called for task.
    }
    return ret;
}

bool ChunkTasksQueue::empty() const {
    std::lock_guard<std::mutex> lock(_mapMx);
    return _empty();
}

std::string ChunkTasksQueue::_queueInfo() const {
    std::stringstream os;
    os << "(qi _activeChunk=";
    if (_activeChunk == _chunkMap.end()) {
        os << "NULL";
    } else {
        ChunkTasks::Ptr const& cts = _activeChunk->second;
        os << _activeChunk->first << " " << cts->cInfo();
    }
    os << ")";
    return os.str();
}

std::string ChunkTasks::toStr(ReadyState state) {
    switch (state) {
        case ReadyState::READY:
            return "READY";
        case ReadyState::NOT_READY:
            return "NOT_READY";
        case ReadyState::NO_RESOURCES:
            return "NO_RESOURCES";
        default:
            return "UNKNOWN_ERR";
    }
}

/// Remove task from ChunkTasks.
/// This depends on owner for thread safety.
/// @return a pointer to the removed task or
wbase::Task::Ptr ChunkTasks::removeTask(wbase::Task::Ptr const& task) {
    // This function isn't called often, so logging changes can be useful.
    LOGS(_log, LOG_LVL_DEBUG, "removeTask " << task->getIdStr() << " " << cInfo());

    auto eraseFunc = [this, &task](std::vector<wbase::Task::Ptr>& vect) -> wbase::Task::Ptr {
        auto queryId = task->getQueryId();
        auto jobId = task->getJobId();
        auto tseq = task->getTSeq();
        for (auto iter = vect.begin(); iter != vect.end(); ++iter) {
            if ((*iter)->idsMatch(queryId, jobId, tseq)) {
                auto ret = *iter;
                vect.erase(iter);
                return ret;
            }
        }
        return nullptr;
    };

    wbase::Task::Ptr result = nullptr;
    // Is it in _activeTasks?
    result = eraseFunc(_activeTasks._tasks);
    if (result != nullptr) {
        _activeTasks.heapify();
        LOGS(_log, LOG_LVL_DEBUG, "removeTask act " << cInfo());
        return result;
    }

    // Is it in _pendingTasks?
    result = eraseFunc(_pendingTasks);
    LOGS(_log, LOG_LVL_DEBUG, "removeTask pend " << cInfo());
    return result;
}

void ChunkTasks::SlowTableHeap::push(wbase::Task::Ptr const& task) {
    _tasks.push_back(task);
    std::push_heap(_tasks.begin(), _tasks.end(), compareFunc);
}

wbase::Task::Ptr ChunkTasks::SlowTableHeap::pop() {
    if (_tasks.empty()) {
        return nullptr;
    }
    auto task = _tasks.front();
    std::pop_heap(_tasks.begin(), _tasks.end(), compareFunc);
    _tasks.pop_back();
    return task;
}

/// Queue new Tasks to be run, ordered with the slowest tables first.
/// This relies on ChunkTasks owner for thread safety.
void ChunkTasks::queTask(wbase::Task::Ptr const& a) {
    const char* state = "";
    // If this is the active chunk, put new Tasks on the pending list, as
    // we could easily get stuck on this chunk as new Tasks come in.
    if (_active) {
        _pendingTasks.push_back(a);
        state = "PENDING";
    } else {
        _activeTasks.push(a);
        state = "ACTIVE";
    }
    LOGS(_log, LOG_LVL_TRACE,
         "ChunkTasks::queTask tid=" << a->getIdStr() << " chunkId=" << _chunkId << " state=" << state
                                    << " active.sz=" << _activeTasks._tasks.size()
                                    << " pend.sz=" << _pendingTasks.size() << cInfo());
    if (_activeTasks.empty()) {
        LOGS(_log, LOG_LVL_TRACE, "Top of ACTIVE is now: (empty)");
    } else {
        LOGS(_log, LOG_LVL_TRACE, "Top of ACTIVE is now: " << _activeTasks.top()->getIdStr());
    }
}

/// Set this chunk as the active chunk and move pending jobs to active if needed.
void ChunkTasks::setActive(bool active) {
    if (_active != active) {
        LOGS(_log, LOG_LVL_TRACE, "ChunkTasks " << _chunkId << " active changed to " << active);
        if (_active && !active) {
            movePendingToActive();
        }
    }
    _active = active;
}

/// Move all pending Tasks to the active heap.
void ChunkTasks::movePendingToActive() {
    for (auto const& t : _pendingTasks) {
        LOGS(_log, LOG_LVL_TRACE, "ChunkTasks " << _chunkId << " pending->active " << t->getIdStr());
        _activeTasks.push(t);
    }
    _pendingTasks.clear();
}

/// @return true if active AND pending are empty.
bool ChunkTasks::empty() const { return _activeTasks.empty() && _pendingTasks.empty(); }

/// This is ready to advance when _activeTasks is empty and no Tasks are in flight.
bool ChunkTasks::readyToAdvance() {
    // There is a rare case where _activeTasks and _inFlightTasks are empty but _readyTask in not null.
    bool advance = _activeTasks.empty() && _inFlightTasks.empty() && _readyTask == nullptr;
    LOGS(_log, LOG_LVL_TRACE,
         "ChunkTasks::readyToAdvance chunkId=" << _chunkId << " _activeTasks.sz=" << _activeTasks.size()
                                               << " _inFlightTasks.sz=" << _inFlightTasks.size()
                                               << " _readyTask==null=" << (_readyTask == nullptr)
                                               << " advance=" << advance << cInfo());
    return advance;
}

/// @Return true if a Task is ready to be run.
// ChunkTasks does not have its own mutex and depends on its owner for thread safety.
// If a Task is ready to be run, _readyTask will not be nullptr.
ChunkTasks::ReadyState ChunkTasks::ready(bool useFlexibleLock) {
    LOGS(_log, LOG_LVL_TRACE, "ready start " << cInfo());
    auto logMemManRes = [this, useFlexibleLock](bool starved, std::string const& msg, int handle, int chunkId,
                                                std::vector<memman::TableInfo> const& tblVect) {
        setResourceStarved(starved);
        if (!starved) {
            std::string str;
            for (auto const& tblInfo : tblVect) {
                str += tblInfo.tableName + " ";
            }
            LOGS(_log, LOG_LVL_TRACE,
                 "ready memMan flex=" << useFlexibleLock << " handle=" << handle << " " << msg
                                      << "chunk=" << chunkId << " - " << str);
        }
    };

    if (_readyTask != nullptr) {
        return ChunkTasks::ReadyState::READY;
    }
    if (_activeTasks.empty()) {
        return ChunkTasks::ReadyState::NOT_READY;
    }

    // Calling this function doesn't get expensive until it gets here. Luckily,
    // after this point it will return READY or NO_RESOURCES, and ChunkTasksQueue::_ready
    // will not examine any further chunks upon seeing those results.
    auto task = _activeTasks.top();
    LOGS(_log, LOG_LVL_TRACE, "ready checking task=" << task->getIdStr() << " " << cInfo());
    int chunkId = -1;
    if (!task->hasMemHandle()) {
        memman::TableInfo::LockType lckOptTbl = memman::TableInfo::LockType::REQUIRED;
        if (useFlexibleLock) lckOptTbl = memman::TableInfo::LockType::FLEXIBLE;
        memman::TableInfo::LockType lckOptIdx = memman::TableInfo::LockType::NOLOCK;
        auto scanInfo = task->getScanInfo();
        chunkId = task->getChunkId();
        if (chunkId != _chunkId) {
            // This would slow things down badly, but the system would survive.
            LOGS(_log, LOG_LVL_ERROR,
                 "ChunkTasks " << _chunkId << " got task for chunk " << chunkId << " " << task->getIdStr());
        }
        std::vector<memman::TableInfo> tblVect;
        for (auto const& tbl : scanInfo->infoTables) {
            memman::TableInfo ti(tbl.db + "/" + tbl.table, lckOptTbl, lckOptIdx);
            tblVect.push_back(ti);
        }
        // If tblVect is empty, we should get the empty handle
        memman::MemMan::Handle handle = _memMan->prepare(tblVect, chunkId);
        LOGS(_log, LOG_LVL_TRACE,
             "memPrep " << _memMan->getStatistics().logString() << " "
                        << _memMan->getStatus(handle).logString());
        if (handle == 0) {
            switch (errno) {
                case ENOMEM:
                    logMemManRes(true, "ENOMEM", handle, chunkId, tblVect);
                    return ChunkTasks::ReadyState::NO_RESOURCES;
                case ENOENT:
                    LOGS(_log, LOG_LVL_ERROR,
                         "_memMgr->lock errno=ENOENT chunk not found " << task->getIdStr());
                    // Not sure if this is the best course of action, but it should just need one
                    // logic path. The query should fail from the missing tables
                    // and the czar must be able to handle that with appropriate retries.
                    handle = memman::MemMan::HandleType::ISEMPTY;
                    break;
                default:
                    LOGS(_log, LOG_LVL_ERROR, "_memMgr->lock file system error " << task->getIdStr());
                    // Any error reading the file system is probably fatal for the worker.
                    throw std::bad_exception();
                    return ChunkTasks::ReadyState::NO_RESOURCES;
            }
        }
        task->setMemHandle(handle);
        logMemManRes(false, task->getIdStr() + " got handle", handle, chunkId, tblVect);
    }

    // There is a Task to run at this point, pull it off the heap to avoid confusion.
    _activeTasks.pop();
    _readyTask = task;
    LOGS(_log, LOG_LVL_TRACE,
         "ready pop t=" << task->getIdStr() << " top="
                        << ((_activeTasks.top() == nullptr) ? "NULL" : _activeTasks.top()->getIdStr()) << " "
                        << cInfo());
    return ChunkTasks::ReadyState::READY;
}

/// @return old value of _resourceStarved.
bool ChunkTasks::setResourceStarved(bool starved) {
    auto val = _resourceStarved;
    _resourceStarved = starved;
    return val;
}

/// @return a Task that is ready to run, if available. Otherwise return nullptr.
/// ChunkTasks relies on its owner for thread safety.
wbase::Task::Ptr ChunkTasks::getTask(bool useFlexibleLock) {
    if (ready(useFlexibleLock) != ReadyState::READY) {
        LOGS(_log, LOG_LVL_DEBUG, "ChunkTasks " << _chunkId << " denying task");
        return nullptr;
    }
    // Return and clear _readyTask so it isn't called more than once.
    auto task = _readyTask;
    _readyTask = nullptr;
    if (task->getChunkId() == _chunkId) {
        _inFlightTasks.insert(task.get());
    } else {
        LOGS(_log, LOG_LVL_ERROR,
             "ChunkTasks::getTask chunkId mismatch task=" << task->getIdStr() << "(" << task->getChunkId()
                                                          << ")"
                                                          << " chunkid=" << _chunkId);
    }
    return task;
}

void ChunkTasks::taskComplete(wbase::Task::Ptr const& task) { _inFlightTasks.erase(task.get()); }

std::string ChunkTasks::cInfo() const {
    std::stringstream os;
    os << " cInfo(chkId=" << _chunkId << " act=" << _active << " starv=" << _resourceStarved
       << " readyTask=" << _readyTask << " inF=" << _inFlightTasks.size() << " (act=" << _activeTasks.size()
       << " ";
    for (auto const& tsk : _activeTasks._tasks) {
        os << tsk->getIdStr() << ", ";
    }
    os << ") (pend.sz=" << _pendingTasks.size() << " ";
    for (auto const& tsk : _pendingTasks) {
        os << tsk->getIdStr() << ", ";
    }
    os << "))";

    return os.str();
}

}  // namespace lsst::qserv::wsched
