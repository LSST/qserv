// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2011-2016 LSST Corporation.
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
/// Task.h
/// @author Daniel L. Wang (danielw)
#ifndef LSST_QSERV_WBASE_TASK_H
#define LSST_QSERV_WBASE_TASK_H

// System headers
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

// Qserv headers
#include "global/intTypes.h"
#include "memman/MemMan.h"
#include "proto/ScanTableInfo.h"
#include "util/ThreadPool.h"
#include "util/threadSafe.h"

// Forward declarations
namespace google {
namespace protobuf {
    class Arena;
}}

namespace lsst {
namespace qserv {
namespace wbase {
    struct ScriptMeta;
    class SendChannelShared;
}
namespace proto {
    class TaskMsg;
    class TaskMsg_Fragment;
}
namespace wpublish {
    class ResourceMonitorLock;
}}} // End of forward declarations

namespace lsst {
namespace qserv {
namespace wbase {

/// Base class for tracking a database query for a worker Task.
class TaskQueryRunner {
public:
    using Ptr = std::shared_ptr<TaskQueryRunner>;
    virtual ~TaskQueryRunner() {};
    virtual bool runQuery()=0;
    virtual void cancel()=0; ///< Repeated calls to cancel() must be harmless.
};

class Task;

/// Base class for scheduling Tasks.
/// Allows the scheduler to take appropriate action when a task is cancelled.
class TaskScheduler {
public:
    using Ptr = std::shared_ptr<TaskScheduler>;
    virtual ~TaskScheduler() {}
    virtual void taskCancelled(Task*)=0;///< Repeated calls must be harmless.
    virtual bool removeTask(std::shared_ptr<Task> const& task, bool removeRunning)=0;
};

/// Used to find tasks that are in process for debugging with Task::_idStr.
/// This is largely meant to track down incomplete tasks in a possible intermittent
/// failure and should probably be removed when it is no longer needed.
/// It depends on code in BlendScheduler to work. If the decision is made to keep it
/// forever, dependency on BlendScheduler needs to be re-worked.
struct IdSet {
    void add(std::string const& id) {
        std::lock_guard<std::mutex> lock(mx);
        _ids.insert(id);
    }
    void remove(std::string const& id) {
        std::lock_guard<std::mutex> lock(mx);
        _ids.erase(id);
    }
    std::atomic<int> maxDisp{5}; //< maximum number of entries to show with operator<<
    friend std::ostream& operator<<(std::ostream& os, IdSet const& idSet);
private:
    std::set<std::string> _ids;
    mutable std::mutex mx;
};

/// class Task defines a query task to be done, containing a TaskMsg
/// (over-the-wire) additional concrete info related to physical
/// execution conditions.
/// Task is non-copyable
class Task : public util::CommandForThreadPool {
public:
    static std::string const defaultUser;
    using Ptr =  std::shared_ptr<Task>;

    enum class State {CREATED, QUEUED, RUNNING, FINISHED};

    struct ChunkEqual {
        bool operator()(Task::Ptr const& x, Task::Ptr const& y);
    };
    struct ChunkIdGreater {
        bool operator()(Ptr const& x, Ptr const& y);
    };

    explicit Task(proto::TaskMsg const& t, std::string const& query, int fragmentNumber,
                  std::shared_ptr<SendChannelShared> const& sc,
                  std::shared_ptr<google::protobuf::Arena> const& gArena,
                  std::shared_ptr<wpublish::ResourceMonitorLock> const& rmLock);
    Task& operator=(const Task&) = delete;
    Task(const Task&) = delete;
    virtual ~Task();

    /// Read 'taskMsg' to generate a vector of one or more task objects all using the same 'sendChannel'
    static std::vector<Ptr> createTasks(proto::TaskMsg const& taskMsg,
                                        std::shared_ptr<SendChannelShared> const& sendChannel,
                                        std::shared_ptr<google::protobuf::Arena> const& gArena,
                                        std::shared_ptr<wpublish::ResourceMonitorLock> const& rmLock);

    proto::TaskMsg const& msg; ///< Protobufs Task spec, in memory held by _gArena
    std::shared_ptr<SendChannelShared> sendChannel; ///< For result reporting
    std::string hash; ///< hash of TaskMsg
    std::string user; ///< Incoming username
    time_t entryTime {0}; ///< Timestamp for task admission
    char timestr[100]; ///< ::ctime_r(&t.entryTime, timestr)
    // Note that manpage spec of "26 bytes"  is insufficient

    /// Cancel the query in progress and set _cancelled.
    void cancel();

    /// Check if this task should be cancelled and call cancel() as needed.
    /// @return true if this task was or needed to be cancelled.
    bool checkCancelled();

    std::string getQueryString() { return _queryString; }
    int getQueryFragmentNum() { return _queryFragmentNum; }
    bool setTaskQueryRunner(TaskQueryRunner::Ptr const& taskQueryRunner); ///< return true if already cancelled.
    void freeTaskQueryRunner(TaskQueryRunner *tqr);
    void setTaskScheduler(TaskScheduler::Ptr const& scheduler) { _taskScheduler = scheduler; }
    TaskScheduler::Ptr getTaskScheduler() { return _taskScheduler.lock(); }
    friend std::ostream& operator<<(std::ostream& os, Task const& t);

    // Shared scan information
    int getChunkId();
    QueryId getQueryId() const { return _qId; }
    int getJobId() const { return _jId; }
    int getAttemptCount() const { return _attemptCount; }
    bool getScanInteractive() {return _scanInteractive; }
    proto::ScanInfo& getScanInfo() { return _scanInfo; }
    void setOnInteractive(bool val) { _onInteractive = val; }
    bool getOnInteractive() { return _onInteractive; }
    bool hasMemHandle() const { return _memHandle != memman::MemMan::HandleType::INVALID; }
    memman::MemMan::Handle getMemHandle() { return _memHandle; }
    memman::MemMan::Status getMemHandleStatus();
    void setMemHandle(memman::MemMan::Handle handle) { _memHandle = handle; }
    void setMemMan(memman::MemMan::Ptr const& memMan) { _memMan = memMan; }
    void waitForMemMan();
    bool getSafeToMoveRunning() { return _safeToMoveRunning; }
    void setSafeToMoveRunning(bool val) { _safeToMoveRunning = val; } ///< For testing only.

    static IdSet allIds; // set of all task jobId numbers that are not complete.
    std::string getIdStr() const {return _idStr;}

    /// @return true if qId and jId match this task's query and job ids.
    bool idsMatch(QueryId qId, int jId) { return (_qId == qId && _jId == jId); }

    // Functions for tracking task state and statistics.
    State getState() const;
    std::chrono::milliseconds getRunTime() const;
    void queued(std::chrono::system_clock::time_point const& now);
    void started(std::chrono::system_clock::time_point const& now);
    std::chrono::milliseconds finished(std::chrono::system_clock::time_point const& now);

    void freeResourceMonitorLock() { _resourceMonitorLock.reset(); }

private:
    QueryId  const    _qId = 0; ///< queryId from czar
    int      const    _jId = 0; ///< jobId from czar
    int      const    _attemptCount = 0; // attemptCount from czar
    std::string const _idStr = QueryIdHelper::makeIdStr(0, 0, true); // < for logging only
    std::string _queryString; ///< The query this task will run.
    int _queryFragmentNum = 0; ///< The fragment number of the query in the task message.
    std::shared_ptr<google::protobuf::Arena> _gArena; ///< holds memory for msg.

    /// Shared pointer to the ResourceMonitorLock, this should be reset when
    /// this Task is done with the chunk.
    std::shared_ptr<wpublish::ResourceMonitorLock> _resourceMonitorLock;

    std::atomic<bool> _cancelled{false};
    std::atomic<bool> _safeToMoveRunning{false}; ///< false until done with waitForMemMan().
    TaskQueryRunner::Ptr _taskQueryRunner;
    std::weak_ptr<TaskScheduler> _taskScheduler;
    proto::ScanInfo _scanInfo;
    bool _scanInteractive; ///< True if the czar thinks this query should be interactive.
    bool _onInteractive{false}; ///< True if the scheduler put this task on the interactive (group) scheduler.
    std::atomic<memman::MemMan::Handle> _memHandle{memman::MemMan::HandleType::INVALID};
    memman::MemMan::Ptr _memMan;

    mutable std::mutex _stateMtx; ///< Mutex to protect state related members _state, _???Time.
    State _state{State::CREATED};
    std::chrono::system_clock::time_point _queueTime;
    std::chrono::system_clock::time_point _startTime;
    std::chrono::system_clock::time_point _finishTime;
    size_t _totalSize = 0; ///< Total size of the result so far.

};

}}} // namespace lsst::qserv::wbase

#endif // LSST_QSERV_WBASE_TASK_H
