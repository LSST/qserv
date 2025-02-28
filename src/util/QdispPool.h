// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
#ifndef LSST_QSERV_UTIL_QDISPPOOL_H
#define LSST_QSERV_UTIL_QDISPPOOL_H

// System headers
#include <map>
#include <vector>

// Third-party headers
#include <nlohmann/json.hpp>

// Qserv headers
#include "util/ThreadPool.h"

namespace lsst::qserv::util {

class PriorityQueue;

class PriorityCommand : public util::CommandTracked {
public:
    using Ptr = std::shared_ptr<PriorityCommand>;
    PriorityCommand() = default;
    explicit PriorityCommand(std::function<void(util::CmdData*)> func) : CommandTracked(func) {}
    ~PriorityCommand() override = default;
    friend PriorityQueue;

private:
    int _priority{0};  ///< Need to know what queue this was placed on.
    /// Priority commands can only be queued once, or PriorityQueue acounting
    /// can be contaminated: this flag causes But to be thrown if queued twice.
    std::atomic<bool> _queued{false};
};

/// FIFO priority queue. Elements with the same priority are handled in
/// a FIFO manner. Lower integer values are higher priority.
/// Higher priority queues get asked first when a thread becomes available
/// but the system reserves room so that each priority has at least
/// a minimum number of threads running.
class PriorityQueue : public util::CommandQueue {
public:
    using Ptr = std::shared_ptr<PriorityQueue>;

    /// A queue for handling all messages of a given priority.
    class PriQ : public util::CommandQueue {
    public:
        using Ptr = std::shared_ptr<PriQ>;

        /// A snapshot status of the queue for logging or monitoring purposes.
        struct Stats {
            Stats(int priority_, size_t size_, int running_)
                    : priority(priority_), size(size_), running(running_) {}
            int priority;
            size_t size;
            int running;
        };

        explicit PriQ(int priority, int minRunning, int maxRunning)
                : _priority(priority), _minRunning(minRunning), _maxRunning(maxRunning) {}
        ~PriQ() override = default;
        int getPriority() const { return _priority; }
        int getMinRunning() const { return _minRunning; }
        int getMaxRunning() const { return _maxRunning; }

        Stats stats() const { return Stats(_priority, const_cast<PriQ*>(this)->size(), running); }

        std::atomic<int> running{0};  ///< number of jobs of this priority currently running.
    private:
        int const _priority;    ///< priority value of this queue
        int const _minRunning;  ///< minimum number of threads (unless nothing on this queue to run)
        int const _maxRunning;  ///< maximum number of threads for this PriQ to use.
    };

    PriorityQueue() = delete;
    PriorityQueue(PriorityQueue const&) = delete;
    PriorityQueue& operator=(PriorityQueue const&) = delete;

    PriorityQueue(int defaultPriority, int minRunning, int maxRunning) : _defaultPriority(defaultPriority) {
        _queues[_defaultPriority] = std::make_shared<PriQ>(_defaultPriority, minRunning, maxRunning);
    }

    ///< @Return true if the queue could be added.
    bool addPriQueue(int priority, int minRunning, int spareThreads);

    /// The pool needs to be able to place commands in this queue for shutdown.
    void queCmd(util::Command::Ptr const& cmd) override;

    void queCmd(PriorityCommand::Ptr const& cmd, int priority);

    util::Command::Ptr getCmd(bool wait = true) override;
    void prepareShutdown();

    void commandStart(util::Command::Ptr const& cmd) override;
    void commandFinish(util::Command::Ptr const& cmd) override;

    /// @return a snapshot of statistics for all queues (one element per queue)
    std::vector<PriQ::Stats> stats() const;

    /// @return a json object with queue information.
    nlohmann::json getJson() const;

private:
    /// @note a lock on _mtx must be held before calling the method
    /// @return a snapshot of statistics for all queues (one element per queue)
    std::vector<PriQ::Stats> _stats() const;

    /// @note a lock on _mtx must be held before calling the method
    /// @return the stringified representation of the statistics for all queues
    std::string _statsStr() const;

    void _incrDecrRunningCount(util::Command::Ptr const& cmd, int incrDecr);

    mutable std::mutex _mtx;
    std::condition_variable _cv;
    bool _shuttingDown{false};
    bool _changed{false};

    std::map<int, PriQ::Ptr> _queues;
    int _defaultPriority{1};
};

/// This class is used to provide a pool of threads for handling out going
/// and incoming messages from xrootd as well as a system for prioritizing
/// the messages.
/// This has not worked entirely as intended. Reducing the number of threads
/// had negative impacts on xrootd, but other changes have been made such that
/// reducing the size of the thread pools can be tried again.
/// What it does do is prioritize outgoing messages (typically jobs going to
/// workers), allow interactive queries to be handled quickly, even under
/// substantial loads, and it gives a good idea of how busy the czar really
/// is. Large numbers of queued items in any of the scan queries, or large
/// results would be good indicators to avoid giving a particular czar more
/// user queries.
///
class QdispPool {
public:
    typedef std::shared_ptr<QdispPool> Ptr;

    /// Default priority, the lowest possible priority.
    static int defaultPriority() { return 100; }
    /// This should be more than enough.
    static int maxPoolSize() { return 20000; }

    /// poolSize - total number of threads in the pool
    /// largestPriority - highest priority is 0, lowest possible priority is
    ///            100 and is reserved for default priority. largestPriority=4 would
    ///            result in PriorityQueues's being created for
    ///            priorities 0, 1, 2, 3, 4, and 100. Priority 100 is
    ///            meant for changing aspects of the pool and shutdown.
    /// runSizes - Each entry represents the maximum number of concurrent running
    ///            commands for a priority given by the position in the array.
    ///            If a position is undefined, the default value is 1.
    ///             ex. 5, 10, 10, 3, 3 would apply to the priorities above as
    ///              priority 0 can have up to 5 concurrent running commands
    ///              priorities 1 and 2 can have up to 10
    ///              priorities 3 and 4 can have up to 3
    /// minRunningSizes - Each entry represents the minimum number of threads
    ///             to be running (defaults to 0). Non-zero values can keep
    ///             lower priorities from being completely starved and/or
    ///             reduce deadlocks from high priorities depending on lower
    ///             priorities.
    QdispPool(int poolSize, int largestPriority, std::vector<int> const& maxRunSizes,
              std::vector<int> const& minRunningSizes);
    QdispPool() = delete;
    explicit QdispPool(bool unitTest);
    QdispPool(QdispPool const&) = delete;
    QdispPool& operator=(QdispPool const&) = delete;

    /// Lower priority numbers are higher priority.
    /// Invalid priorities get the lowest priority (high priority number).
    void queCmd(PriorityCommand::Ptr const& cmd, int priority) { _prQueue->queCmd(cmd, priority); }

    /// Commands on queue's with priority lower than default may not be run.
    void shutdownPool() {
        _prQueue->prepareShutdown();
        _pool->shutdownPool();
    }

    /// @return a json object with queue information.
    nlohmann::json getJson() const { return _prQueue->getJson(); }

private:
    PriorityQueue::Ptr _prQueue;
    util::ThreadPool::Ptr _pool;
};

}  // namespace lsst::qserv::util

#endif /* LSST_QSERV_UTIL_QDISPPOOL_H_ */
