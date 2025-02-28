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
#ifndef LSST_QSERV_WSCHED_CHUNKTASKLIST_H
#define LSST_QSERV_WSCHED_CHUNKTASKLIST_H

// System headers
#include <algorithm>
#include <list>
#include <map>
#include <mutex>
#include <vector>

// Qserv headers
#include "memman/MemMan.h"
#include "wbase/Task.h"
#include "wsched/ChunkTaskCollection.h"
#include "wsched/SchedulerBase.h"

namespace lsst::qserv::wsched {

/// A class to store Tasks for a specific chunk.
/// Tasks are normally placed on _activeTasks, but will be added
/// to _pendingTasks when this is the active chunk.
/// The active chunk is the first chunk to be checked for tasks to run.
/// Placing tasks on the pending list prevents getting stuck on the
/// active chunk indefinitely.
class ChunkTasks {
public:
    using Ptr = std::shared_ptr<ChunkTasks>;
    enum class ReadyState { READY = 0, NOT_READY = 1, NO_RESOURCES = 2 };
    static std::string toStr(ReadyState);

    ChunkTasks(int chunkId, memman::MemMan::Ptr const& memMan) : _chunkId{chunkId}, _memMan{memMan} {}
    ChunkTasks() = delete;
    ChunkTasks(ChunkTasks const&) = delete;
    ChunkTasks& operator=(ChunkTasks const&) = delete;

    bool empty() const;
    void queTask(wbase::Task::Ptr const& task);
    wbase::Task::Ptr getTask(bool useFlexibleLock);
    ReadyState ready(bool useFlexibleLock);
    void taskComplete(wbase::Task::Ptr const& task);

    void movePendingToActive();             ///< Move all pending Tasks to _activeTasks.
    bool readyToAdvance();                  ///< @return true if active Tasks for this chunk are done.
    void setActive(bool active = true);     ///< Flag current requests so new requests will be pending.
    bool setResourceStarved(bool starved);  ///< hook for tracking starvation.
    std::size_t size() const { return _activeTasks.size() + _pendingTasks.size(); }
    int getChunkId() { return _chunkId; }

    // Remove 'task' from this instance.
    // @return a pointer to the removed Task.
    wbase::Task::Ptr removeTask(wbase::Task::Ptr const& task);

    /// @return a string describing this instance for the log file.
    std::string cInfo(bool listTasks = false) const;

    /// Class that keeps the slowest tables at the front of the heap.
    class SlowTableHeap {
    public:
        // Using a greater than comparison function results in a minimum value heap.
        static bool compareFunc(wbase::Task::Ptr const& x, wbase::Task::Ptr const& y) {
            if (!x || !y) {
                return false;
            }
            // compare scanInfo (slower scans first)
            int siComp = x->getScanInfo()->compareTables(*(y->getScanInfo()));
            return siComp < 0;
        };
        void push(wbase::Task::Ptr const& task);
        wbase::Task::Ptr pop();
        wbase::Task::Ptr top() {
            if (_tasks.empty()) return nullptr;
            return _tasks.front();
        }
        bool empty() const { return _tasks.empty(); }
        size_t size() const { return _tasks.size(); }
        void heapify() { std::make_heap(_tasks.begin(), _tasks.end(), compareFunc); }

        std::vector<wbase::Task::Ptr> _tasks;
    };

private:
    int _chunkId;                                 ///< Chunk Id for all Tasks in this instance.
    bool _active{false};                          ///< True when this is the active chunk.
    bool _resourceStarved{false};                 ///< True when advancement is prevented by lack of memory.
    wbase::Task::Ptr _readyTask{nullptr};         ///< Task that is ready to run with memory reserved.
    SlowTableHeap _activeTasks;                   ///< All Tasks must be put on this before they can run.
    std::vector<wbase::Task::Ptr> _pendingTasks;  ///< Task that should not be run until later.
    std::set<wbase::Task*> _inFlightTasks;        ///< Set of Tasks that this chunk has in flight.

    memman::MemMan::Ptr _memMan;
};

/// This class queues Tasks by their chunkId and tables rating and names.
/// New Tasks are queued with other Tasks with the same chunkId and then by shared
/// scan tables used.
/// - Tasks are provided starting with the _activeChunk, which remains the
///   _activeChunk until all of its Tasks are completed. At which time, the
///   _activeChunk advances to the chunk with the next higher chunkId. While
///   a chunk is the _activeChunk, all new Tasks for that chunk are put in
///   in a pending list so that the active chunk does not get stalled.
/// - While all the Tasks on the _active chunk have been started, but not completed,
///   Tasks can be taken from chunks after the _activeChunk as long as resources are
///   available.
/// Like the other schedulers, ready() is the core of this class as it determines
/// if a Task is ready to run and which Task will be provided by getTask().
class ChunkTasksQueue : public ChunkTaskCollection {
public:
    using Ptr = std::shared_ptr<ChunkTasksQueue>;

    /// This must be std::map to maintain valid iterators.
    /// Only erase() will invalidate and iterator with std::map.
    using ChunkMap = std::map<int, ChunkTasks::Ptr>;

    enum { READY, NOT_READY, NO_RESOURCES };

    ChunkTasksQueue(SchedulerBase* scheduler, memman::MemMan::Ptr const& memMan)
            : _memMan{memMan}, _scheduler{scheduler} {}
    ChunkTasksQueue(ChunkTasksQueue const&) = delete;
    ChunkTasksQueue& operator=(ChunkTasksQueue const&) = delete;

    void queueTask(wbase::Task::Ptr const& task) override;
    void queueTask(std::vector<wbase::Task::Ptr> const& tasks) override;
    wbase::Task::Ptr getTask(bool useFlexibleLock) override;
    bool empty() const override;
    std::size_t getSize() const override { return _taskCount; }
    bool ready(bool useFlexibleLock) override;
    void taskComplete(wbase::Task::Ptr const& task) override;

    bool setResourceStarved(bool starved) override;
    int getActiveChunkId();  ///< return the active chunk id, or -1 if there isn't one.

    wbase::Task::Ptr removeTask(wbase::Task::Ptr const& task) override;

    std::string queueInfo() const override {
        std::lock_guard<std::mutex> lck(_mapMx);
        return _queueInfo();
    }

private:
    bool _ready(bool useFlexibleLock);
    bool _empty() const { return _chunkMap.empty(); }
    std::string _queueInfo() const;  ///< _mapMx must be locked before calling

    mutable std::mutex _mapMx;                         ///< Protects _chunkMap, _activeChunk, and _readyChunk.
    ChunkMap _chunkMap;                                ///< map by chunk Id.
    ChunkMap::iterator _activeChunk{_chunkMap.end()};  ///< points at the active ChunkTasks in _chunkList
    ChunkTasks::Ptr _readyChunk{nullptr};              ///< Chunk with the task that's ready to run.

    memman::MemMan::Ptr _memMan;
    std::atomic<int> _taskCount{0};  ///< Count of all tasks currently in _chunkMap.
    bool _resourceStarved{false};
    SchedulerBase* _scheduler;  ///< Pointer to scheduler that owns this. This can be nullptr.
};

}  // namespace lsst::qserv::wsched

#endif  // LSST_QSERV_WSCHED_CHUNKTASKLIST_H
