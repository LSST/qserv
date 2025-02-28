// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2018 LSST Corporation.
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
 *  @author: John Gates,
 */

#ifndef LSST_QSERV_UTIL_THREADPOOL_H_
#define LSST_QSERV_UTIL_THREADPOOL_H_

// System headers
#include <atomic>
#include <cassert>
#include <deque>
#include <queue>
#include <thread>
#include <vector>

// Qserv headers
#include "util/EventThread.h"

namespace lsst::qserv::util {

class ThreadPool;

/// An EventThread to be used by the ThreadPool class.
/// finishup() is used to tell the ThreadPool that this thread is finished.
class PoolEventThread : public EventThread, public std::enable_shared_from_this<PoolEventThread> {
public:
    using Ptr = std::shared_ptr<PoolEventThread>;

    static PoolEventThread::Ptr newPoolEventThread(std::shared_ptr<ThreadPool> const& threadPool,
                                                   CommandQueue::Ptr const& q);
    virtual ~PoolEventThread();

    /// Cause this thread to leave the thread pool. This MUST only called from within
    /// the thread that will be removed (most likely from within a CommandThreadPool action).
    void leavePool();

    /// This can be called from outside the thread that will be removed.
    /// - If within the thread that will leave the pool, leavePool()
    ///   should be called.
    /// - atMaxThreadCount() should be called first to avoid seriously
    ///   breaking the limit of threads.
    /// - This version of leavePool never waits as that could cause a deadlock.
    bool leavePool(Command::Ptr const& cmd);

    /// Return true if at or above the maximum number of threads that can
    /// exist concurrently.
    bool atMaxThreadCount();

protected:
    void specialActions(Command::Ptr const& cmd) override;
    void finishup() override;
    std::shared_ptr<ThreadPool> _threadPool;
    std::atomic<bool> _finishupOnce{false};  //< Ensure finishup() only called once.

private:
    PoolEventThread(std::shared_ptr<ThreadPool> const& threadPool, CommandQueue::Ptr const& q);
};

/// A Command that is aware that it is running as part of a PoolEventThread,
// which allows it to tell the event thread and pool to take special actions.
class CommandForThreadPool : public CommandTracked {
public:
    using Ptr = std::shared_ptr<CommandForThreadPool>;

    CommandForThreadPool() = default;
    explicit CommandForThreadPool(std::function<void(CmdData*)> func) : CommandTracked{func} {}
    virtual ~CommandForThreadPool();

    /// Return true if the number of threads created and still existing is
    /// greater than the max.
    bool atMaxThreadCount();
    PoolEventThread::Ptr getAndNullPoolEventThread();

    friend class PoolEventThread;

private:
    void _setPoolEventThread(PoolEventThread::Ptr const& poolEventThread);

    std::weak_ptr<PoolEventThread> _poolEventThread;
    std::mutex _poolMtx;
};

/// ThreadPool is a variable size pool of threads all fed by the same CommandQueue.
/// Growing the pool is simple, shrinking the pool is complex. Both operations should
/// have no effect on running commands or commands on the queue.
/// NOTE: shutdownPool() MUST be called before a ThreadPool is destroyed or there will
///  likely be a segmentation fault. Every command sent to the pool before shutdown is
///  called should complete. Once shutdown has been called, the size of the pool
///  cannot be increased (target size permanently set to 0).
/// Note: It is possible for threads to leave the pool and be replaced using leavePool()
///  This is usually done when a thread no longer requires significant CPU but has
///  to wait for something to happen, like transferring data.
///  _poolThreadCount is a total of all threads that are still running both in the pool
///  and those that have left the pool but have not finished.
///  _poolThreadCount total should not exceed _maxThreadCount.
///  _maxThreadCount can be significantly larger than the _targetThrdCount.
///
class ThreadPool : public std::enable_shared_from_this<ThreadPool> {
public:
    using Ptr = std::shared_ptr<ThreadPool>;

    /// Used to create thread pool where there is not expected to be be a need
    /// to remove time consuming, low CPU usage threads from the pool.
    static ThreadPool::Ptr newThreadPool(unsigned int thrdCount, CommandQueue::Ptr const& q,
                                         EventThreadJoiner::Ptr const& joiner = nullptr);

    /// Used to create a pool where threads are expected to be removed for processing
    /// and replaced by calling PoolEventThread::leavePool().
    /// Useful for scheduling queries against mysql. The queries use significant CPU until
    /// mysql finishes, and then the results sit around (using little CPU) until the czar
    /// collects them.
    static ThreadPool::Ptr newThreadPool(unsigned int thrdCount, unsigned int maxThreadCount,
                                         CommandQueue::Ptr const& q,
                                         EventThreadJoiner::Ptr const& joiner = nullptr);

    virtual ~ThreadPool();

    void shutdownPool();
    CommandQueue::Ptr getQueue() { return _q; }
    unsigned int getTargetThrdCount() {
        std::lock_guard<std::mutex> lock(_countMutex);
        return _targetThrdCount;
    }
    unsigned int size() {
        std::lock_guard<std::mutex> lock(_poolMutex);
        return _pool.size();
    }

    void waitForResize(int millisecs);
    void endAll() { resize(0); }
    void resize(unsigned int targetThrdCount);
    bool release(PoolEventThread* thread);

    /// Return true if existing threads are at or above _maxThreadCount.
    bool atMaxThreadPoolCount();

    friend PoolEventThread;

private:
    ThreadPool(unsigned int thrdCount, unsigned int maxPoolThreads, CommandQueue::Ptr const& q,
               EventThreadJoiner::Ptr const& joiner);
    void _resize();

    std::mutex _poolMutex;                                ///< Protects _pool
    std::vector<std::shared_ptr<PoolEventThread>> _pool;  ///< All the threads in our pool.

    std::mutex _countMutex;            ///< protects _targetThrdCount
    unsigned int _targetThrdCount{0};  ///< How many threads wanted in the pool.
    std::condition_variable _countCV;  ///< Notifies about changes to _pool size, uses _countMutex.
    CommandQueue::Ptr _q;              ///< The queue used by all threads in the _pool.

    EventThreadJoiner::Ptr _joinerThread;  ///< Tracks and joins threads removed from the pool.
    std::atomic<bool> _shutdown{false};    ///< True after shutdownPool has been called.

    // Functions to track the number of threads created by the pool.
    /// Wait until the number of existing threads is <= max existing threads.
    /// The pool kicks out threads that require significant time to process and
    /// replaces them on a regular basis, see PoolEventThread::leavePool().
    /// Under some circumstances, the number of threads that have been kicked out but
    /// have not finished can become extremely large (tens of thousands). There
    /// isn't much choice but to wait for some to finish before kicking more out.
    void _waitIfAtMaxThreadPoolCount();

    /// Increase the count of existing threads.
    void _incrPoolThreadCount();

    /// Decrease the count of existing threads.
    void _decrPoolThreadCount();

    unsigned int _poolThreadCount = 0;    ///< Number of threads that exist.
    unsigned int _maxThreadCount = 5000;  ///< Max number of thread allowed, set from config.
    std::condition_variable _cvPool{};    ///< Signal when threads deleted.
    mutable std::mutex _mxPool{};         ///< Protects _poolThreadCount, _cvPool, _mxPool
};

}  // namespace lsst::qserv::util
#endif  // LSST_QSERV_UTIL_THREADPOOL_H_
