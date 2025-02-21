// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2017 LSST Corporation.
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
// class Executive is in charge of "executing" user query fragments on
// a qserv cluster.

#ifndef LSST_QSERV_QDISP_EXECUTIVE_H
#define LSST_QSERV_QDISP_EXECUTIVE_H

// System headers
#include <atomic>
#include <mutex>
#include <sstream>
#include <vector>

// Third-party headers
#include "boost/asio.hpp"

// Qserv headers
#include "global/intTypes.h"
#include "global/ResourceUnit.h"
#include "global/stringTypes.h"
#include "protojson/ScanTableInfo.h"
#include "qdisp/JobDescription.h"
#include "qdisp/ResponseHandler.h"
#include "qdisp/UberJob.h"
#include "qmeta/JobStatus.h"
#include "util/EventThread.h"
#include "util/InstanceCount.h"
#include "util/MultiError.h"
#include "util/threadSafe.h"
#include "util/ThreadPool.h"

namespace lsst::qserv {

namespace ccontrol {
class UserQuerySelect;
}

namespace qmeta {
class MessageStore;
class QStatus;
}  // namespace qmeta

namespace qproc {
class QuerySession;
}

namespace qdisp {
class JobQuery;
class UberJob;
}  // namespace qdisp

namespace rproc {
class InfileMerger;
}

namespace util {
class AsyncTimer;
class PriorityCommand;
class QdispPool;
}  // namespace util

namespace qdisp {

/// class Executive manages the execution of jobs for a UserQuery.
class Executive : public std::enable_shared_from_this<Executive> {
public:
    typedef std::shared_ptr<Executive> Ptr;
    typedef std::unordered_map<int, std::shared_ptr<JobQuery>> JobMap;
    typedef int ChunkIdType;
    typedef std::map<ChunkIdType, std::shared_ptr<JobQuery>> ChunkIdJobMapType;

    /// Construct an Executive.
    /// If c->serviceUrl == ExecutiveConfig::getMockStr(), then use XrdSsiServiceMock
    /// instead of a real XrdSsiService
    static Executive::Ptr create(int secsBetweenUpdates, std::shared_ptr<qmeta::MessageStore> const& ms,
                                 std::shared_ptr<util::QdispPool> const& qdispPool,
                                 std::shared_ptr<qmeta::QStatus> const& qMeta,
                                 std::shared_ptr<qproc::QuerySession> const& querySession,
                                 boost::asio::io_service& asioIoService);

    virtual ~Executive();

    std::string cName(const char* funcName = "") {
        return std::string("Executive::") + funcName + " " + getIdStr();
    }

    /// Set the UserQuerySelect object for this query so this Executive can ask it to make new
    /// UberJobs in the future, if needed.
    void setUserQuerySelect(std::shared_ptr<ccontrol::UserQuerySelect> const& uqs) { _userQuerySelect = uqs; }

    /// Return a map that only contains Jobs not assigned to an UberJob.
    ChunkIdJobMapType unassignedChunksInQuery();

    /// Find the UberJob with `ujId`.
    std::shared_ptr<UberJob> findUberJob(UberJobId ujId);

    /// Add an item with a reference number
    std::shared_ptr<JobQuery> add(JobDescription::Ptr const& s);

    /// Add the UberJob `uj` to the list and queue it to be sent to a worker.
    void addAndQueueUberJob(std::shared_ptr<UberJob> const& uj);

    /// Queue `cmd`, using the QDispPool, so it can be used to collect the result file.
    void queueFileCollect(std::shared_ptr<util::PriorityCommand> const& cmd);

    /// Waits for all jobs on _jobStartCmdList to start. This should not be called
    /// before ALL jobs have been added to the pool.
    void waitForAllJobsToStart();

    /// Block until execution is completed
    /// @return true if execution was successful
    bool join();

    /// Notify the executive that an item has completed
    void markCompleted(JobId refNum, bool success);

    /// Squash all the jobs.
    void squash(std::string const& note);

    bool getEmpty() { return _empty; }

    /// These values cannot be set until information has been collected from
    /// QMeta, which isn't called until some basic checks on the user query
    /// have passed.
    void setQueryId(QueryId id);

    QueryId getId() const { return _id; }
    std::string const& getIdStr() const { return _idStr; }

    void setScanInteractive(bool interactive) { _scanInteractive = interactive; }
    bool getScanInteractive() const { return _scanInteractive; }

    /// @return number of jobs in flight.
    int getNumInflight() const;

    /// @return a description of the current execution progress.
    std::string getProgressDesc() const;

    /// @return true if cancelled
    bool getCancelled() { return _cancelled; }

    std::shared_ptr<util::QdispPool> getQdispPool() { return _qdispPool; }

    /// Add 'rowCount' to the total number of rows in the result table.
    void addResultRows(int64_t rowCount);

    int64_t getTotalResultRows() const { return _totalResultRows; }

    /// Check if conditions are met for the query to be complete with the
    /// rows already read in.
    void checkLimitRowComplete();

    /// Returns the maximum number of rows the worker needs for the LIMIT clause, or
    ///   a value <= 0 there's no limit that can be applied at the worker.
    int getUjRowLimit() const;

    /// @return _rowLimitComplete, which can only be meaningful if the
    ///         user query has not been cancelled.
    bool isRowLimitComplete() { return _rowLimitComplete && !_cancelled; }

    /// @return the value of _dataIgnoredCount
    int incrDataIgnoredCount() { return ++_dataIgnoredCount; }

    /// Store job status and execution errors for the proxy and qservMeta QMessages.
    /// @see python module lsst.qserv.czar.proxy.unlock()
    void updateProxyMessages();

    /// Call UserQuerySelect::buildAndSendUberJobs make new UberJobs for
    /// unassigned jobs.
    virtual void assignJobsToUberJobs();

    int getTotalJobs() { return _totalJobs; }

    /// Add an error code and message that may be displayed to the user.
    void addMultiError(int errorCode, std::string const& errorMsg, int errState);

    std::string dumpUberJobCounts() const;

    // The below value should probably be based on the user query, with longer sleeps for slower queries.
    int getAttemptSleepSeconds() const { return 15; }  // As above or until added to config file.
    int getMaxAttempts() const { return 5; }           // TODO:UJ Should be set by config

    /// Calling this indicates all Jobs for this user query have been created.
    void setAllJobsCreated() { _allJobsCreated = true; }

    /// Returns true if all jobs have been created.
    bool isAllJobsCreated() { return _allJobsCreated; }

    /// Send a message to all workers to cancel this query.
    /// @param deleteResults - If true, delete all result files for this query on the workers.
    void sendWorkersEndMsg(bool deleteResults);

    /// Complete UberJobs have their results on the czar, the
    /// incomplete UberJobs need to be stopped and possibly reassigned.
    void killIncompleteUberJobsOnWorker(std::string const& workerId);

    // Try to remove this and put in constructor
    void setScanInfo(protojson::ScanInfo::Ptr const& scanInfo) { _scanInfo = scanInfo; }

    /// Return a pointer to _scanInfo.
    protojson::ScanInfo::Ptr getScanInfo() { return _scanInfo; }

    /// Add fileSize to `_totalResultFileSize` and check if it exceeds limits.
    /// If it is too large, check the value against existing UberJob result
    /// sizes as `_totalResultFileSize` may include failed UberJobs.
    /// If the sum of all UberJob result files size is too large,
    /// cancel this user query.
    void checkResultFileSize(uint64_t fileSize = 0);

protected:
    Executive(int secondsBetweenUpdates, std::shared_ptr<qmeta::MessageStore> const& ms,
              std::shared_ptr<util::QdispPool> const& sharedResources,
              std::shared_ptr<qmeta::QStatus> const& qStatus,
              std::shared_ptr<qproc::QuerySession> const& querySession);

private:
    void _setupLimit();

    bool _track(int refNum, std::shared_ptr<JobQuery> const& r);
    void _unTrack(int refNum);
    bool _addJobToMap(std::shared_ptr<JobQuery> const& job);
    std::string _getIncompleteJobsString(int maxToList);

    void _waitAllUntilEmpty();

    void _squashSuperfluous();

    /// @return previous value of _rowLimitComplete while setting it to true.
    ///  This indicates that enough rows have been read to complete the user query
    ///  with a LIMIT clause, and no group by or order by clause.
    bool _setLimitRowComplete() { return _rowLimitComplete.exchange(true); }

    // for debugging
    void _printState(std::ostream& os);

    /// The method performs the non-blocking sampling of the query monitoring stats.
    /// The stats are pushed to qdisp::CzarStats.
    void _updateStats() const;

    std::atomic<bool> _empty{true};
    std::shared_ptr<qmeta::MessageStore> _messageStore;  ///< MessageStore for logging

    JobMap _jobMap;          ///< Contains information about all jobs.
    JobMap _incompleteJobs;  ///< Map of incomplete jobs.
    /// How many jobs are used in this query. 1 avoids possible 0 of 0 jobs completed race condition.
    /// The correct value is set when it is available.
    std::atomic<int> _totalJobs{1};
    std::shared_ptr<util::QdispPool>
            _qdispPool;  ///< Shared thread pool for handling commands to and from workers.

    std::deque<std::shared_ptr<util::PriorityCommand>> _jobStartCmdList;  ///< list of jobs to start.

    /** Execution errors */
    util::MultiError _multiError;

    std::atomic<int> _requestCount{0};   ///< Count of submitted jobs
    util::Flag<bool> _cancelled{false};  ///< Has execution been cancelled.

    // Mutexes
    mutable std::mutex _incompleteJobsMutex;  ///< protect incompleteJobs map.

    /// Used to record execution errors
    mutable std::mutex _errorsMutex;

    std::condition_variable _allJobsComplete;
    // TODO:UJ see what it takes to make this a normal mutex, before
    //  xrootd resulted in things being called in difficult to predict
    //  ways. That shouldn't be an issue any more.
    mutable std::recursive_mutex _jobMapMtx;

    QueryId _id = 0;  ///< Unique identifier for this query.
    std::string _idStr{QueryIdHelper::makeIdStr(0, true)};

    std::shared_ptr<qmeta::QStatus> _qMeta;
    /// Last time Executive updated QMeta, defaults to epoch for clock.
    std::chrono::system_clock::time_point _lastQMetaUpdate;
    /// Minimum number of seconds between QMeta chunk updates (set by config)
    std::chrono::seconds _secondsBetweenQMetaUpdates;
    std::mutex _lastQMetaMtx;  ///< protects _lastQMetaUpdate.

    /// true for interactive scans, once set it doesn't change.
    bool _scanInteractive = false;

    // Add a job to the _chunkToJobMap
    // TODO:UJ This may need review as large changes were made to this part of the code.
    //     code is no longer destructive to _chunkToJobMap
    void _addToChunkJobMap(std::shared_ptr<JobQuery> const& job);
    std::mutex _chunkToJobMapMtx;      ///< protects _chunkToJobMap
    ChunkIdJobMapType _chunkToJobMap;  ///< Map of jobs ordered by chunkId

    /// Map of all UberJobs. Failed UberJobs remain in the map as new ones are created
    /// to handle failed UberJobs.
    std::map<UberJobId, std::shared_ptr<UberJob>> _uberJobsMap;
    mutable std::mutex _uberJobsMapMtx;  ///< protects _uberJobs.

    /// True if enough rows were read to satisfy a LIMIT query with
    /// no ORDER BY or GROUP BY clauses.
    std::atomic<bool> _rowLimitComplete{false};

    std::atomic<int64_t> _totalResultRows{0};
    std::weak_ptr<qproc::QuerySession> _querySession;
    std::shared_ptr<util::AsyncTimer> _asyncTimer;  ///< for non-blocking updates of stats
    int64_t _limit = 0;                             ///< Limit to number of rows to return. 0 means no limit.

    /// true if query can be returned as soon as _limit rows have been read.
    bool _limitSquashApplies = false;

    /// Number of time data has been ignored for for this user query.
    std::atomic<int> _dataIgnoredCount{0};

    std::atomic<bool> _queryIdSet{false};  ///< Set to true when _id is set.

    /// Weak pointer to the UserQuerySelect object for this query.
    std::weak_ptr<ccontrol::UserQuerySelect> _userQuerySelect;

    /// Flag that is set to true when all jobs have been created.
    std::atomic<bool> _allJobsCreated{false};

    protojson::ScanInfo::Ptr _scanInfo;  ///< Scan rating and tables.

    std::atomic<uint64_t> _totalResultFileSize{0};  ///< Total size of all UberJob result files.
};

}  // namespace qdisp
}  // namespace lsst::qserv

#endif  // LSST_QSERV_QDISP_EXECUTIVE_H
