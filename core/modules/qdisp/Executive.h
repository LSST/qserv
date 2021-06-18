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

//&&& replace with better enable/disable feature.
#define uberJobsEnabled 1 //&&&

// System headers
#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

// Qserv headers
#include "global/intTypes.h"
#include "global/ResourceUnit.h"
#include "global/stringTypes.h"
#include "qdisp/JobDescription.h"
#include "qdisp/JobStatus.h"
#include "qdisp/ResponseHandler.h"
#include "qdisp/QdispPool.h"
#include "util/EventThread.h"
#include "util/InstanceCount.h"
#include "util/MultiError.h"
#include "util/threadSafe.h"
#include "util/ThreadPool.h"

// Forward declarations
class XrdSsiService;

namespace lsst {
namespace qserv {
namespace qmeta {
class QStatus;
}

namespace qdisp {

class JobQuery;
class LargeResultMgr;
class MessageStore;
class UberJob;

struct ExecutiveConfig {
    typedef std::shared_ptr<ExecutiveConfig> Ptr;
    ExecutiveConfig(std::string const& serviceUrl_, int secsBetweenChunkUpdates_)
        : serviceUrl(serviceUrl_), secondsBetweenChunkUpdates(secsBetweenChunkUpdates_) {}
    ExecutiveConfig(int,int) : serviceUrl(getMockStr()) {}

    std::string serviceUrl; ///< XrdSsi service URL, e.g. localhost:1094
    int secondsBetweenChunkUpdates; ///< Seconds between QMeta chunk updates.
    static std::string getMockStr() { return "Mock"; }
};


/// class Executive manages the execution of jobs for a UserQuery, while
/// maintaining minimal information about the jobs themselves.
class Executive : public std::enable_shared_from_this<Executive> {
public:
    typedef std::shared_ptr<Executive> Ptr;
    typedef std::unordered_map<int, std::shared_ptr<JobQuery>> JobMap;
    typedef int ChunkIdType; //&&& TODO:UJ probably needs to be ResourceUnit
    typedef std::unordered_map<ChunkIdType, JobQuery*> ChunkIdJobMapType;

    /// Construct an Executive.
    /// If c->serviceUrl == ExecutiveConfig::getMockStr(), then use XrdSsiServiceMock
    /// instead of a real XrdSsiService
    static Executive::Ptr create(ExecutiveConfig const& c, std::shared_ptr<MessageStore> const& ms,
                std::shared_ptr<QdispPool> const& qdispPool, std::shared_ptr<qmeta::QStatus> const& qMeta);

    ~Executive();

    /// Add an item with a reference number
    std::shared_ptr<JobQuery> add(JobDescription::Ptr const& s);

    /// Queue a job to be sent to a worker so it can be started.
    void queueJobStart(PriorityCommand::Ptr const& cmd);

    /// Waits for all jobs on _jobStartCmdList to start. This should not be called
    /// before ALL jobs have been added to the pool.
    void waitForAllJobsToStart();


    /// Block until execution is completed
    /// @return true if execution was successful
    bool join();

    /// Notify the executive that an item has completed
    void markCompleted(int refNum, bool success);

    /// Squash all the jobs.
    void squash();

    bool getEmpty() { return _empty; }

    void setQueryId(QueryId id);
    QueryId getId() const { return _id; }
    std::string const& getIdStr() const { return _idStr; }

    void setScanInteractive(bool interactive) { _scanInteractive = interactive; }

    /// @return number of items in flight.
    int getNumInflight(); // non-const, requires a mutex.

    /// @return a description of the current execution progress.
    std::string getProgressDesc() const;

    /// @return true if cancelled
    bool getCancelled() { return _cancelled; }

    XrdSsiService* getXrdSsiService() { return _xrdSsiService; }

    std::shared_ptr<QdispPool> getQdispPool() { return _qdispPool; }

    bool startQuery(std::shared_ptr<JobQuery> const& jobQuery);

    /// Add UbjerJobs to this user query.
    void addUberJobs(std::vector<std::shared_ptr<UberJob>> const& jobsToAdd);
    ChunkIdJobMapType& getChunkJobMapAndInvalidate();
    bool startUberJob(std::shared_ptr<UberJob> const& uJob);
    std::shared_ptr<JobQuery> getSharedPtrForRawJobPtr(JobQuery* jqRaw);

private:
    Executive(ExecutiveConfig const& c, std::shared_ptr<MessageStore> const& ms,
              std::shared_ptr<QdispPool> const& qdispPool, std::shared_ptr<qmeta::QStatus> const& qStatus);

    void _setup();

    bool _track(int refNum, std::shared_ptr<JobQuery> const& r);
    void _unTrack(int refNum);
    bool _addJobToMap(std::shared_ptr<JobQuery> const& job);
    std::string _getIncompleteJobsString(int maxToList);

    void _updateProxyMessages();

    void _waitAllUntilEmpty();

    // for debugging
    void _printState(std::ostream& os);

    ExecutiveConfig _config; ///< Personal copy of config
    std::atomic<bool> _empty{true};
    std::shared_ptr<MessageStore> _messageStore; ///< MessageStore for logging

    /// RPC interface, static to avoid getting every time a user query starts and separate
    /// from _xrdSsiService to avoid conflicts with XrdSsiServiceMock.
    static XrdSsiService* _xrdSsiServiceStatic;
    XrdSsiService* _xrdSsiService; ///< RPC interface
    JobMap _jobMap; ///< Contains information about all jobs.
    JobMap _incompleteJobs; ///< Map of incomplete jobs.
    /// How many jobs are used in this query. 1 avoids possible 0 of 0 jobs completed race condition.
    /// The correct value is set when it is available.
    std::atomic<int> _totalJobs{1};
    QdispPool::Ptr _qdispPool; ///< Shared thread pool for handling commands to and from workers.

    std::deque<PriorityCommand::Ptr> _jobStartCmdList; ///< list of jobs to start.

    /** Execution errors */
    util::MultiError _multiError;

    std::atomic<int> _requestCount; ///< Count of submitted jobs
    util::Flag<bool> _cancelled{false}; ///< Has execution been cancelled.

    // Mutexes
    std::mutex _incompleteJobsMutex; ///< protect incompleteJobs map.

    /** Used to record execution errors */
    mutable std::mutex _errorsMutex;

    std::condition_variable _allJobsComplete;
    mutable std::recursive_mutex _jobMapMtx;

    QueryId _id{0}; ///< Unique identifier for this query.
    std::string _idStr{QueryIdHelper::makeIdStr(0, true)};
    util::InstanceCount _instC{"Executive"};

    std::shared_ptr<qmeta::QStatus> _qMeta;
    /// Last time Executive updated QMeta, defaults to epoch for clock.
    std::chrono::system_clock::time_point _lastQMetaUpdate;
    /// Minimum number of seconds between QMeta chunk updates (set by config)
    std::chrono::seconds _secondsBetweenQMetaUpdates{60};
    std::mutex _lastQMetaMtx; ///< protects _lastQMetaUpdate.

    bool _scanInteractive = false; ///< true for interactive scans.

    // Add a job to the _chunkToJobMap
    void _addToChunkJobMap(std::shared_ptr<JobQuery> const& job);
    /// _chunkToJobMap is created once and then destroyed when used.
    std::atomic<bool> _chunkToJobMapInvalid{false}; ///< true indicates the map is no longer valid.
    std::mutex _chunkToJobMapMtx; ///< protects _chunkToJobMap
    ChunkIdJobMapType _chunkToJobMap; ///< Map of jobs ordered by chunkId


    std::vector<std::shared_ptr<UberJob>> _uberJobs; ///< List of UberJobs
    std::mutex _uberJobsMtx; ///< protects _uberJobs.
};

class MarkCompleteFunc {
public:
    typedef std::shared_ptr<MarkCompleteFunc> Ptr;

    MarkCompleteFunc(Executive::Ptr const& e, int jobId) : _executive(e), _jobId(jobId) {}
    virtual ~MarkCompleteFunc() {}

    virtual void operator()(bool success) {
        auto exec = _executive.lock();
        if (exec != nullptr) {
            exec->markCompleted(_jobId, success);
        }
    }

private:
    std::weak_ptr<Executive> _executive;
    int _jobId;
};

}}} // namespace lsst::qserv::qdisp

#endif // LSST_QSERV_QDISP_EXECUTIVE_H
