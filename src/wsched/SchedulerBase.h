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
 *
 */

#ifndef LSST_QSERV_WSCHED_SCHEDULERBASE_H
#define LSST_QSERV_WSCHED_SCHEDULERBASE_H

// System headers
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "wcontrol/Foreman.h"

namespace lsst::qserv::util {
class Histogram;
}

namespace lsst::qserv::wsched {

class BlendScheduler;

class SchedulerBase : public wcontrol::Scheduler {
public:
    using Ptr = std::shared_ptr<SchedulerBase>;

    static int getMaxPriority() { return 1000000000; }

    SchedulerBase(std::string const& name, int maxThreads, int maxReserve, int maxActiveChunks, int priority);
    virtual ~SchedulerBase() {}
    SchedulerBase(SchedulerBase const&) = delete;
    SchedulerBase& operator=(SchedulerBase const&) = delete;

    std::string getName() const override { return _name; }

    /// @return the number of tasks in flight.
    virtual int getInFlight() const { return _inFlight; }
    virtual std::size_t getSize() const = 0;  ///< @return the number of tasks in the queue (not in flight).
    virtual bool ready() = 0;                 ///< @return true if the scheduler is ready to provide a Task.
    int getUserQueriesInQ();                  ///< @return number of UserQueries in the queue.
    int getActiveChunkCount();                ///< @return number of chunks being queried.
    int getMaxActiveChunks() const { return _maxActiveChunks; }
    void setMaxActiveChunks(int maxActive);
    bool chunkAlreadyActive(int chunkId);  ///< Return true if chunkId currently has queries being run on it.

    /// Methods for altering priority.
    // Hooks for changing this schedulers priority/reserved threads.
    int getPriority() { return _priority; }
    void setPriority(int priority);  ///< Priority to use starting next chunk
    void setPriorityDefault();       ///< Return to default priority next chunk
    int getMaxReserve() { return _maxReserve; }
    virtual void setMaxReserve(int maxReserve) { _maxReserve = maxReserve; }
    void restoreMaxReserve() { setMaxReserve(_maxReserveDefault); }

    /// Use the number of available threads to determine how many threads this
    /// scheduler can use (_maxThreadAdj).
    /// @return (availableThreads - (The number of threads beyond our reserve that we are using.))
    virtual int applyAvailableThreads(int availableThreads) {
        _maxThreadsAdj = availableThreads + desiredThreadReserve();
        int remainingThreads = availableThreads - std::max(0, _inFlight - _maxReserve);
        return remainingThreads;
    }

    /// @return the number of threads this scheduler would like to have reserved for it.
    /// The idea being that if this scheduler has a _maxReserve of 2 and zero Tasks
    /// inFlight, it wants 1 thread reserved so it can start work right away.
    /// If it has 1 or 2 Tasks running, it asks for 2 threads to be reserved so the queries
    /// do not get interrupted, or in the case of 1 Task, a second Task can be started right away.
    /// If 3 or more Tasks are running it still asks for 2 to be reserved.
    virtual int desiredThreadReserve() { return std::min(_inFlight + 1, _maxReserve); }

    /// Return maximum number of Tasks this scheduler can have inFlight.
    virtual int maxInFlight() { return std::min(_maxThreads, _maxThreadsAdj); }

    /// Record performance data for the scheduler.
    virtual void recordPerformanceData();

    std::string chunkStatusStr();  //< @return a string

    /// @return a JSON representation of the object's status for the monitoring
    nlohmann::json statusToJsonBase();

    int getTotalTaskCount() { return _totalTaskCount; }

    /// Remove task from this scheduler.
    /// @return - If task was still in the queue, return true.
    /// Most schedulers do not support this operation. Currently only supports
    /// moving from/to ScanSchedulers.
    bool removeTask(wbase::Task::Ptr const& task, bool removeRunning) override { return false; }

    void setDefaultPosition(int val) { _defaultPosition = val; }
    int getDefaultPosition() const { return _defaultPosition; }

    /// Class used to track number of Tasks that are transmitting.
    class TransmittingTracker {
    public:
        using TPtr = std::shared_ptr<TransmittingTracker>;
        static TPtr create(SchedulerBase& sched) { return TPtr(new TransmittingTracker(sched)); }
        TransmittingTracker() = delete;
        ~TransmittingTracker() { _sched._transmitCount -= 1; }

    private:
        TransmittingTracker(SchedulerBase& sched) : _sched(sched) { _sched._transmitCount += 1; }
        SchedulerBase& _sched;
    };

protected:
    /// Increment the _userQueryCounts entry for queryId, creating it if needed.
    /// Precondition util::CommandQueue::_mx must be locked.
    /// @return the new count for queryId.
    int _incrCountForUserQuery(QueryId queryId, int sz);

    /// Decrement the _userQueryCounts entry for queryId. The entry is deleted if the new value <= 0.
    /// @return the new count for queryId.
    int _decrCountForUserQuery(QueryId queryId);

    void _incrChunkTaskCount(int chunkId);  //< Increase the count of Tasks working on this chunk.
    void _decrChunkTaskCount(int chunkId);  //< Decrease the count of Tasks working on this chunk.

    std::string const _name{};  //< Name of this scheduler.
    int _maxReserve = 1;        //< Number of threads this scheduler would like to have reserved for its use.
    int _maxReserveDefault = 1;
    int _maxThreads = 1;     //< Maximum number of threads for this scheduler to have inFlight.
    int _maxThreadsAdj = 1;  //< Maximum number of threads to have inFlight adjusted for available pool.

    int _priority;  ///< Current priority, higher value - higher priority
    int _priorityDefault;

    std::atomic<int> _inFlight{0};           ///< Number of Tasks running.
    std::atomic<int> _recentlyCompleted{0};  ///< Number of completed tasks, reset at intervals.
    std::atomic<int> _transmitCount{0};      ///< Number of tasks transmitting.

    std::shared_ptr<util::HistogramRolling> _histQueuedTasks;   ///< Histogram to track tasks on the queue
    std::shared_ptr<util::HistogramRolling> _histRunningTasks;  ///< Histogram to track running tasks
    std::shared_ptr<util::HistogramRolling>
            _histTransmittingTasks;  ///< Histogram to track transmitting tasks
    std::shared_ptr<util::HistogramRolling>
            _histRecentlyCompletedTasks;  ///< Histogram to track how many tasks were completed

private:
    /// The true purpose of _userQuerycount is to track how many different UserQuery's are on the queue.
    /// Number of Tasks for each UserQuery in the queue.
    std::map<QueryId, int> _userQueryCounts;

    std::atomic<int> _totalTaskCount{0};

    std::map<int, int> _chunkTasks;  ///< Number of tasks in each chunk actively being queried.
    std::mutex _countsMutex;         ///< Protects _userQueryCounts and _chunkTasks.
    // TODO: Decide to keep or remove _maxActiveChunks and related code. This depends primarily
    //       on 'everything' scheduler limits/needs.
    int _maxActiveChunks;      ///< Limit the number of chunks this scheduler can work on at one time.
    int _defaultPosition{10};  ///< Position of this scheduler in the list of schedulers.
};

}  // namespace lsst::qserv::wsched

#endif /* LSST_QSERV_WSCHED_SCHEDULERBASE_H_ */
