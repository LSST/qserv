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

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "global/DbTable.h"
#include "global/intTypes.h"
#include "memman/MemMan.h"
#include "protojson/ScanTableInfo.h"
#include "wbase/TaskState.h"
#include "util/Histogram.h"
#include "util/ThreadPool.h"

// Forward declarations
namespace lsst::qserv::mysql {
class MySqlConfig;
}

namespace lsst::qserv::protojson {
class UberJobMsg;
}

namespace lsst::qserv::wbase {
class FileChannelShared;
}
namespace lsst::qserv::wcontrol {
class SqlConnMgr;
}
namespace lsst::qserv::wdb {
class ChunkResourceMgr;
}
namespace lsst::qserv::wpublish {
class QueriesAndChunks;
class QueryStatistics;
}  // namespace lsst::qserv::wpublish

namespace lsst::qserv::wbase {

class UberJobData;
class UserQueryInfo;

class TaskException : public util::Issue {
public:
    explicit TaskException(util::Issue::Context const& ctx, std::string const& msg) : util::Issue(ctx, msg) {}
};

/// Base class for tracking a database query for a worker Task.
class TaskQueryRunner {
public:
    using Ptr = std::shared_ptr<TaskQueryRunner>;
    virtual ~TaskQueryRunner() {};
    virtual bool runQuery() = 0;
    virtual void cancel() = 0;  ///< Repeated calls to cancel() must be harmless.
};

/// Class for storing database + table name.
class TaskDbTbl {
public:
    TaskDbTbl() = delete;
    TaskDbTbl(std::string const& db_, std::string const& tbl_) : db(db_), tbl(tbl_) {}
    std::string const db;
    std::string const tbl;
};

class Task;

/// Base class for scheduling Tasks.
/// Allows the scheduler to take appropriate action when a task is cancelled.
class TaskScheduler {
public:
    using Ptr = std::shared_ptr<TaskScheduler>;
    TaskScheduler();
    virtual ~TaskScheduler() {}
    virtual std::string getName() const = 0;  //< @return the name of the scheduler.
    virtual void taskCancelled(Task*) = 0;    ///< Repeated calls must be harmless.
    virtual bool removeTask(std::shared_ptr<Task> const& task, bool removeRunning) = 0;

    util::HistogramRolling::Ptr histTimeOfRunningTasks;       ///< Store information about running tasks
    util::HistogramRolling::Ptr histTimeOfTransmittingTasks;  ///< Store information about transmitting tasks.
};

/// class Task defines a query task to be done, containing a TaskMsg
/// (over-the-wire) additional concrete info related to physical
/// execution conditions.
/// Task is non-copyable
class Task : public util::CommandForThreadPool {
public:
    static std::string const defaultUser;
    using Ptr = std::shared_ptr<Task>;

    /// Class to store constant sets and vectors.
    class DbTblsAndSubchunks {
    public:
        DbTblsAndSubchunks() = delete;
        DbTblsAndSubchunks(DbTblsAndSubchunks const&) = delete;
        DbTblsAndSubchunks& operator=(DbTblsAndSubchunks const&) = delete;

        DbTblsAndSubchunks(DbTableSet const& dbTbls_, IntVector const& subchunksVect_)
                : dbTbls(dbTbls_), subchunksVect(subchunksVect_) {}
        ~DbTblsAndSubchunks() = default;

        /// Set of tables used by ChunkResourceRequest possible. Set in constructor and should never change.
        const DbTableSet dbTbls;

        /// Vector of subchunkIds. Set in constructor and should never change.
        const IntVector subchunksVect;
    };

    struct ChunkEqual {
        bool operator()(Ptr const& x, Ptr const& y);
    };
    struct ChunkIdGreater {
        bool operator()(Ptr const& x, Ptr const& y);
    };

    std::string cName(const char* func) const { return std::string("Task::") + func + " " + _idStr; }

    // TODO:UJ too many parameters.
    //  - fragmentNumber seems pointless
    //  - hasSubchunks seems redundant.
    //  Hopefully, many are the same for all tasks and can be moved to ujData and userQueryInfo.
    //  Candidates: maxTableSizeMb, FileChannelShared, resultsHttpPort.
    //  Unfortunately, this will be much easier if it is done after xrootd method is removed.
    Task(std::shared_ptr<UberJobData> const& ujData, int jobId, int attemptCount, int chunkId,
         int fragmentNumber, size_t templateId, bool hasSubchunks, int subchunkId, std::string const& db,
         std::vector<TaskDbTbl> const& fragSubTables, std::vector<int> const& fragSubchunkIds,
         std::shared_ptr<FileChannelShared> const& sc,
         std::shared_ptr<wpublish::QueryStatistics> const& queryStats_);

    Task& operator=(const Task&) = delete;
    Task(const Task&) = delete;
    virtual ~Task();

    /// Create the Tasks needed to run an UberJob on this worker.
    static std::vector<Ptr> createTasksFromUberJobMsg(
            std::shared_ptr<protojson::UberJobMsg> const& uberJobMsg,
            std::shared_ptr<UberJobData> const& ujData,
            std::shared_ptr<wbase::FileChannelShared> const& sendChannel,
            std::shared_ptr<wdb::ChunkResourceMgr> const& chunkResourceMgr,
            mysql::MySqlConfig const& mySqlConfig, std::shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr,
            std::shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks);

    /// Create Tasks needed to run unit tests.
    static std::vector<Ptr> createTasksForUnitTest(
            std::shared_ptr<UberJobData> const& ujData, nlohmann::json const& jsJobs,
            std::shared_ptr<wbase::FileChannelShared> const& sendChannel, int maxTableSizeMb,
            std::shared_ptr<wdb::ChunkResourceMgr> const& chunkResourceMgr);

    std::shared_ptr<FileChannelShared> getSendChannel() const { return _sendChannel; }
    void resetSendChannel() { _sendChannel.reset(); }  ///< reset the shared pointer for FileChannelShared
    std::string user;                                  ///< Incoming username
    // Note that manpage spec of "26 bytes"  is insufficient

    /// This is the function the scheduler will run, overriden from the util::Command class.
    /// This will check if it has already been called and then call QueryRunner::runQuery().
    /// @param data - is ignored by this class.
    void action(util::CmdData* data) override;

    /// Cancel the query in progress and set _cancelled.
    /// Query cancellation on the worker is fairly complicated.
    /// This may come from:
    /// - czar - user query was cancelled, an error, or limit reached.
    /// This function may also be called by `Task::checkCancelled()` - `_sendChannel`
    /// has been killed, usually a result of failed czar communication.
    /// If a `QueryRunner` object for this task exists, it must
    /// be cancelled to free up threads and other resources.
    /// Otherwise `_cancelled` is set so that an attempt
    /// to run this `Task` will result in a rapid exit.
    /// This functional also attempts to inform the scheduler for this
    /// `Task` that is has been cancelled. The scheduler currently does
    /// nothing in this case.
    void cancel();

    /// Check if this task should be cancelled and call cancel() as needed.
    /// @return true if this task was or needed to be cancelled.
    bool checkCancelled();

    TaskState state() const { return _state; }
    std::string getQueryString() const;
    bool setTaskQueryRunner(
            TaskQueryRunner::Ptr const& taskQueryRunner);  ///< return true if already cancelled.
    void freeTaskQueryRunner(TaskQueryRunner* tqr);
    void setTaskScheduler(TaskScheduler::Ptr const& scheduler) { _taskScheduler = scheduler; }
    TaskScheduler::Ptr getTaskScheduler() const { return _taskScheduler.lock(); }
    friend std::ostream& operator<<(std::ostream& os, Task const& t);

    // Shared scan information
    bool getHasChunkId() const { return _hasChunkId; }

    /// @return the chunkId for this task. If the task has no chunkId, return -1.
    int getChunkId() const { return _chunkId; }

    QueryId getQueryId() const { return _qId; }
    size_t getTemplateId() const { return _templateId; }
    int getJobId() const { return _jId; }
    int getAttemptCount() const { return _attemptCount; }
    bool getScanInteractive() const;
    int64_t getMaxTableSize() const;

    protojson::ScanInfo::Ptr getScanInfo() const;
    void setOnInteractive(bool val) { _onInteractive = val; }
    bool getOnInteractive() { return _onInteractive; }
    bool hasMemHandle() const { return _memHandle != memman::MemMan::HandleType::INVALID; }
    memman::MemMan::Handle getMemHandle() { return _memHandle; }
    memman::MemMan::Status getMemHandleStatus();
    void setMemHandle(memman::MemMan::Handle handle) { _memHandle = handle; }
    void setMemMan(memman::MemMan::Ptr const& memMan) { _memMan = memMan; }
    void waitForMemMan();
    bool getSafeToMoveRunning() { return _safeToMoveRunning; }
    void setSafeToMoveRunning(bool val) { _safeToMoveRunning = val; }  ///< For testing only.

    /// @return true if qId and jId match this task's query and job ids.
    bool idsMatch(QueryId qId, int jId, uint64_t tseq) const {
        return (_qId == qId && _jId == jId && tseq == _tSeq);
    }

    // Functions for tracking task state and statistics.

    /// @return 'true' if the task is still running (waiting before MySQL will finish executing
    ///  queries or sending a result set to Czar)
    bool isRunning() const;

    /// @return the amount of time spent so far on the task in milliseconds.
    std::chrono::milliseconds getRunTime() const;

    void queued(std::chrono::system_clock::time_point const& now);
    void started(std::chrono::system_clock::time_point const& now);

    /// The actual execution of the corresponding MySQL query (or queries) started.
    void queryExecutionStarted();

    /// MySQL finished executing queries.
    void queried();

    std::chrono::milliseconds finished(std::chrono::system_clock::time_point const& now);

    uint64_t getTSeq() const { return _tSeq; }

    /// The returned string is only useful for logging purposes.
    std::string getIdStr(bool invalid = false) const {
        return QueryIdHelper::makeIdStr(_qId, _jId, invalid) + std::to_string(_tSeq) + ":";
    }

    std::shared_ptr<wpublish::QueryStatistics> getQueryStats() const;

    /// Return a json object describing sdome details of this task.
    nlohmann::json getJson() const;

    std::string getDb() const { return _db; }
    int getCzarId() const { return _czarId; }
    bool getFragmentHasSubchunks() const { return _fragmentHasSubchunks; }
    int getSubchunkId() const { return _subchunkId; }

    /// Returns a reference to dbTbls.
    const DbTableSet& getDbTbls() const { return _dbTblsAndSubchunks->dbTbls; }

    /// Return a reference to the list of subchunk ids.
    const IntVector& getSubchunksVect() const { return _dbTblsAndSubchunks->subchunksVect; }

    /// Return an identifier of the corresponding MySQL query (if any was set).
    unsigned long getMySqlThreadId() const { return _mysqlThreadId.load(); }

    /// Set MySQL thread associated with a MySQL connection open before executing
    /// task's queries. The identifier is sampled by the worker tasks monitoring
    /// system in order to see what MySQL queries are being executed by tasks.
    void setMySqlThreadId(unsigned long id) { _mysqlThreadId.store(id); }

    /// Return true if this task was already booted.
    bool setBooted();

    /// Return true if the task was booted.
    bool isBooted() const { return _booted; }

    /// Only to be used in unit tests, use this to set a lambda function
    /// to use in a unit test.
    void setUnitTest(std::function<void(util::CmdData*)> func) {
        _unitTest = true;
        setFunc(func);
    }

    std::shared_ptr<UberJobData> getUberJobData() const { return _ujData; }

    /// Returns the LIMIT of rows for the query enforceable at the worker, where values <= 0 indicate
    /// that there is no limit to the number of rows sent back by the worker.
    /// @see UberJobData::getRowLimit()
    int getRowLimit() { return _rowLimit; }

private:
    std::shared_ptr<FileChannelShared> _sendChannel;  ///< Send channel.

    uint64_t const _tSeq = 0;          ///< identifier for the specific task
    QueryId const _qId = 0;            ///< queryId from czar
    size_t const _templateId;          ///< Id number of the template in _userQueryInfo.
    bool const _hasChunkId;            ///< True if there was a chunkId in the czar message.
    int const _chunkId;                ///< chunkId from czar
    int const _subchunkId;             ///< subchunkId from czar
    int const _jId = 0;                ///< jobId from czar
    int const _attemptCount = 0;       ///< attemptCount from czar
    int const _queryFragmentNum;       ///< The fragment number of the query in the task message.
    bool const _fragmentHasSubchunks;  ///< True if the fragment in this query has subchunks.
    std::string _db;                   ///< Task database
    int const _czarId;                 ///< czar Id from the task message.

    /// Set of tables and vector of subchunk ids used by ChunkResourceRequest. Do not change/reset.
    std::unique_ptr<DbTblsAndSubchunks> _dbTblsAndSubchunks;

    std::atomic<bool> _queryStarted{false};  ///< Set to true when the query is about to be run.
    std::atomic<bool> _cancelled{false};
    std::atomic<bool> _safeToMoveRunning{false};  ///< false until done with waitForMemMan().
    TaskQueryRunner::Ptr _taskQueryRunner;
    std::weak_ptr<TaskScheduler> _taskScheduler;
    bool _onInteractive{
            false};  ///< True if the scheduler put this task on the interactive (group) scheduler.

    /// Stores information on the query's resource usage.
    std::weak_ptr<wpublish::QueryStatistics> const _queryStats;

    std::atomic<memman::MemMan::Handle> _memHandle{memman::MemMan::HandleType::INVALID};
    memman::MemMan::Ptr _memMan;

    mutable std::mutex _stateMtx;  ///< Mutex to protect state related members _state, _???Time.
    std::atomic<TaskState> _state{TaskState::CREATED};
    std::chrono::system_clock::time_point _createTime =
            std::chrono::system_clock::now();              ///< task was created
    std::chrono::system_clock::time_point _queueTime;      ///< task was queued
    std::chrono::system_clock::time_point _startTime;      ///< task processing started
    std::chrono::system_clock::time_point _queryExecTime;  ///< query execution at MySQL started
    std::chrono::system_clock::time_point _queryTime;      ///< MySQL finished executing queries
    std::chrono::system_clock::time_point _finishTime;     ///< data transmission to Czar fiished
    size_t _totalSize = 0;                                 ///< Total size of the result so far.

    std::atomic<unsigned long> _mysqlThreadId{0};  ///< 0 if not connected to MySQL

    std::atomic<bool> _booted{false};  ///< Set to true if this task takes too long and is booted.

    /// Time stamp for when `_booted` is set to true, otherwise meaningless.
    TIMEPOINT _bootedTime;

    /// When > 0, indicates maximum number of rows needed for a result.
    int const _rowLimit;

    std::shared_ptr<UberJobData> _ujData;
    std::string const _idStr;

    bool _unitTest = false;  ///<
};

}  // namespace lsst::qserv::wbase

#endif  // LSST_QSERV_WBASE_TASK_H
