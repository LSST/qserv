// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2017 AURA/LSST.
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
 * @brief Executive. It executes and tracks jobs from a user query.
 *
 * TODO: Consider merging RequesterMap and StatusMap. Originally, RequesterMap
 * was separate from StatusMap to reduce contention when things are just
 * updating statuses, but if the contention is small, we can simplify by
 * combining them (Requester, status) into a single map.
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "qdisp/Executive.h"

// System headers
#include <algorithm>
#include <cassert>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <sstream>

// Third-party headers
#include "boost/format.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "ccontrol/MergingHandler.h"
#include "ccontrol/msgCode.h"
#include "ccontrol/TmpTableName.h"
#include "ccontrol/UserQuerySelect.h"
#include "czar/Czar.h"
#include "global/LogContext.h"
#include "global/ResourceUnit.h"
#include "qdisp/CzarStats.h"
#include "qdisp/JobQuery.h"
#include "qdisp/ResponseHandler.h"
#include "query/QueryContext.h"
#include "qproc/QuerySession.h"
#include "qmeta/Exceptions.h"
#include "qmeta/MessageStore.h"
#include "qmeta/QStatus.h"
#include "query/SelectStmt.h"
#include "rproc/InfileMerger.h"
#include "util/AsyncTimer.h"
#include "util/Bug.h"
#include "util/EventThread.h"
#include "util/QdispPool.h"

using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.Executive");

}  // anonymous namespace

namespace lsst::qserv::qdisp {

////////////////////////////////////////////////////////////////////////
// class Executive implementation
////////////////////////////////////////////////////////////////////////
Executive::Executive(int secondsBetweenUpdates, shared_ptr<qmeta::MessageStore> const& ms,
                     util::QdispPool::Ptr const& qdispPool, shared_ptr<qmeta::QStatus> const& qStatus,
                     shared_ptr<qproc::QuerySession> const& querySession)
        : _messageStore(ms),
          _qdispPool(qdispPool),
          _qMeta(qStatus),
          _secondsBetweenQMetaUpdates(chrono::seconds(secondsBetweenUpdates)),
          _querySession(querySession) {
    _setupLimit();
    qdisp::CzarStats::get()->addQuery();
}

Executive::~Executive() {
    LOGS(_log, LOG_LVL_DEBUG, "Executive::~Executive() " << getIdStr());
    qdisp::CzarStats::get()->deleteQuery();
    qdisp::CzarStats::get()->deleteJobs(_incompleteJobs.size());
    // Remove this executive from the map.
    auto cz = czar::Czar::getCzar();  // cz can be null in unit tests.
    if (cz != nullptr && cz->getExecutiveFromMap(getId()) != nullptr) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) + " pointer in map should be invalid QID=" << getId());
    }
    if (_asyncTimer != nullptr) {
        _asyncTimer->cancel();
    }
    qdisp::CzarStats::get()->untrackQueryProgress(_id);
}

Executive::Ptr Executive::create(int secsBetweenUpdates, shared_ptr<qmeta::MessageStore> const& ms,
                                 std::shared_ptr<util::QdispPool> const& qdispPool,
                                 shared_ptr<qmeta::QStatus> const& qMeta,
                                 shared_ptr<qproc::QuerySession> const& querySession,
                                 boost::asio::io_service& asioIoService) {
    LOGS(_log, LOG_LVL_DEBUG, "Executive::" << __func__);
    Executive::Ptr ptr(new Executive(secsBetweenUpdates, ms, qdispPool, qMeta, querySession));

    // Start the query progress monitoring timer (if enabled). The query status
    // will be sampled on each expiration event of the timer. Note that the timer
    // gets restarted automatically for as long as the context (the current
    // Executive object) still exists.
    //
    // IMPORTANT: The weak pointer dependency (unlike the regular shared pointer)
    // is required here to allow destroying the Executive object without explicitly
    // stopping the timer.
    auto const czarStatsUpdateIvalSec = cconfig::CzarConfig::instance()->czarStatsUpdateIvalSec();
    if (czarStatsUpdateIvalSec > 0) {
        // AsyncTimer has a 'self' keep alive in AsyncTimer::start() that keeps it safe when
        // this Executive is deleted.
        ptr->_asyncTimer = util::AsyncTimer::create(
                asioIoService, std::chrono::milliseconds(czarStatsUpdateIvalSec * 1000),
                [self = std::weak_ptr<Executive>(ptr)](auto expirationIvalMs) -> bool {
                    auto ptr = self.lock();
                    string const msg = string("Executive::") + __func__ +
                                       " expirationIvalMs: " + to_string(expirationIvalMs.count()) + " ms";
                    if (ptr != nullptr) {
                        ptr->_updateStats();
                        LOGS(_log, LOG_LVL_DEBUG, msg + " " + ptr->getIdStr());
                        return true;
                    }
                    LOGS(_log, LOG_LVL_DEBUG, msg);
                    return false;
                });
        ptr->_asyncTimer->start();
    }
    return ptr;
}

void Executive::_updateStats() const {
    LOGS(_log, LOG_LVL_DEBUG, "Executive::" << __func__);
    qdisp::CzarStats::get()->updateQueryProgress(_id, getNumInflight());
}

void Executive::setQueryId(QueryId id) {
    if (_queryIdSet.exchange(true) == true) {
        throw util::Bug(ERR_LOC, "Executive::setQueryId called more than once _id=" + to_string(_id) +
                                         " id=" + to_string(id));
    }
    _id = id;
    _idStr = QueryIdHelper::makeIdStr(_id);

    // Insert into the global executive map.
    czar::Czar::getCzar()->insertExecutive(_id, shared_from_this());
    qdisp::CzarStats::get()->trackQueryProgress(_id);
}

UberJob::Ptr Executive::findUberJob(UberJobId ujId) {
    lock_guard<mutex> lgMap(_uberJobsMapMtx);
    auto iter = _uberJobsMap.find(ujId);
    if (iter == _uberJobsMap.end()) {
        return nullptr;
    }
    return iter->second;
}

/// Add a new job to executive queue, if not already in. Not thread-safe.
///
JobQuery::Ptr Executive::add(JobDescription::Ptr const& jobDesc) {
    JobQuery::Ptr jobQuery;
    {
        // Create the JobQuery and put it in the map.
        auto jobStatus = make_shared<qmeta::JobStatus>();
        Ptr thisPtr = shared_from_this();
        jobQuery = JobQuery::create(thisPtr, jobDesc, jobStatus, _id);

        QSERV_LOGCONTEXT_QUERY_JOB(jobQuery->getQueryId(), jobQuery->getJobId());

        {
            {
                lock_guard lock(_cancelled.getMutex());
                if (_cancelled) {
                    LOGS(_log, LOG_LVL_DEBUG,
                         "Executive already cancelled, ignoring add(" << jobDesc->id() << ")");
                    return nullptr;
                }
            }

            if (!_track(jobQuery->getJobId(), jobQuery)) {
                LOGS(_log, LOG_LVL_ERROR, "Executive ignoring duplicate track add");
                return jobQuery;
            }

            if (!_addJobToMap(jobQuery)) {
                LOGS(_log, LOG_LVL_ERROR, "Executive ignoring duplicate job add");
                return jobQuery;
            }

            _addToChunkJobMap(jobQuery);
        }

        if (_empty.exchange(false)) {
            LOGS(_log, LOG_LVL_DEBUG, "Flag _empty set to false");
        }
        ++_requestCount;
    }

    QSERV_LOGCONTEXT_QUERY_JOB(jobQuery->getQueryId(), jobQuery->getJobId());

    return jobQuery;
}

void Executive::queueFileCollect(util::PriorityCommand::Ptr const& cmd) {
    if (_scanInteractive) {
        _qdispPool->queCmd(cmd, 2);
    } else {
        _qdispPool->queCmd(cmd, 3);
    }
}

void Executive::addAndQueueUberJob(shared_ptr<UberJob> const& uj) {
    {
        lock_guard<mutex> lck(_uberJobsMapMtx);
        UberJobId ujId = uj->getUjId();
        _uberJobsMap[ujId] = uj;
        LOGS(_log, LOG_LVL_INFO, cName(__func__) << " ujId=" << ujId << " uj.sz=" << uj->getJobCount());
    }

    auto runUberJobFunc = [uj](util::CmdData*) { uj->runUberJob(); };

    auto cmd = util::PriorityCommand::Ptr(new util::PriorityCommand(runUberJobFunc));
    _jobStartCmdList.push_back(cmd);
    if (_scanInteractive) {
        _qdispPool->queCmd(cmd, 0);
    } else {
        _qdispPool->queCmd(cmd, 1);
    }
}

void Executive::waitForAllJobsToStart() {
    LOGS(_log, LOG_LVL_INFO, "waitForAllJobsToStart");
    // Wait for each command to start.
    while (true) {
        bool empty = _jobStartCmdList.empty();
        if (empty) break;
        auto cmd = move(_jobStartCmdList.front());
        _jobStartCmdList.pop_front();
        cmd->waitComplete();
    }
    LOGS(_log, LOG_LVL_INFO, "waitForAllJobsToStart done");
}

Executive::ChunkIdJobMapType Executive::unassignedChunksInQuery() {
    lock_guard<mutex> lck(_chunkToJobMapMtx);

    ChunkIdJobMapType unassignedMap;
    for (auto const& [key, jobPtr] : _chunkToJobMap) {
        if (!jobPtr->isInUberJob()) {
            unassignedMap[key] = jobPtr;
        }
    }
    return unassignedMap;
}

string Executive::dumpUberJobCounts() const {
    stringstream os;
    os << "exec=" << getIdStr();
    int totalJobs = 0;
    {
        lock_guard<mutex> ujmLck(_uberJobsMapMtx);
        for (auto const& [ujKey, ujPtr] : _uberJobsMap) {
            int jobCount = ujPtr->getJobCount();
            totalJobs += jobCount;
            os << "{" << ujKey << ":" << ujPtr->getIdStr() << " jobCount=" << jobCount << "}";
        }
    }
    {
        lock_guard<recursive_mutex> jmLck(_jobMapMtx);
        os << " ujTotalJobs=" << totalJobs << " execJobs=" << _jobMap.size();
    }
    return os.str();
}

void Executive::assignJobsToUberJobs() {
    auto uqs = _userQuerySelect.lock();
    if (uqs != nullptr) {
        uqs->buildAndSendUberJobs();
    }
}

void Executive::addMultiError(int errorCode, std::string const& errorMsg, int errorState) {
    util::Error err(errorCode, errorMsg, errorState);
    {
        lock_guard<mutex> lock(_errorsMutex);
        _multiError.push_back(err);
        LOGS(_log, LOG_LVL_DEBUG,
             cName(__func__) + " multiError:" << _multiError.size() << ":" << _multiError);
    }
}

/// Add a JobQuery to this Executive.
/// Return true if it was successfully added to the map.
///
bool Executive::_addJobToMap(JobQuery::Ptr const& job) {
    auto entry = pair<int, JobQuery::Ptr>(job->getJobId(), job);
    lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
    bool res = _jobMap.insert(entry).second;
    _totalJobs = _jobMap.size();
    return res;
}

bool Executive::join() {
    // To join, we make sure that all of the chunks added so far are complete.
    // Check to see if _requesters is empty, if not, then sleep on a condition.
    _waitAllUntilEmpty();
    LOGS(_log, LOG_LVL_INFO, cName(__func__) << " wait done");
    // Okay to merge. probably not the Executive's responsibility
    struct successF {
        static bool func(Executive::JobMap::value_type const& entry) {
            qmeta::JobStatus::Info const& esI = entry.second->getStatus()->getInfo();
            LOGS(_log, LOG_LVL_TRACE, "entry state:" << (void*)entry.second.get() << " " << esI);
            return (esI.state == qmeta::JobStatus::RESPONSE_DONE) ||
                   (esI.state == qmeta::JobStatus::COMPLETE);
        }
    };

    int sCount = 0;
    {
        lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
        sCount = count_if(_jobMap.begin(), _jobMap.end(), successF::func);
    }
    if (sCount == _requestCount) {
        LOGS(_log, LOG_LVL_INFO,
             "Query execution succeeded all: " << _requestCount << " jobs dispatched and completed.");
    } else if (isRowLimitComplete()) {
        LOGS(_log, LOG_LVL_INFO,
             "Query execution succeeded enough (LIMIT): " << sCount << " jobs out of " << _requestCount
                                                          << " completed.");
    } else {
        LOGS(_log, LOG_LVL_ERROR,
             "Query execution failed: " << _requestCount << " jobs dispatched, but only " << sCount
                                        << " jobs completed");
    }
    _empty = (sCount == _requestCount);
    LOGS(_log, LOG_LVL_DEBUG,
         cName(__func__) << " "
                         << "Flag set to _empty=" << _empty << ", sCount=" << sCount
                         << ", requestCount=" << _requestCount);
    return _empty || isRowLimitComplete();
}

void Executive::markCompleted(JobId jobId, bool success) {
    ResponseHandler::Error err;
    string idStr = QueryIdHelper::makeIdStr(_id, jobId);
    LOGS(_log, LOG_LVL_DEBUG, "Executive::markCompleted " << success);
    if (!success && !isRowLimitComplete()) {
        {
            lock_guard<mutex> lock(_incompleteJobsMutex);
            auto iter = _incompleteJobs.find(jobId);
            if (iter != _incompleteJobs.end()) {
                auto jobQuery = iter->second;
                err = jobQuery->getDescription()->respHandler()->getError();
            } else {
                string msg = "Executive::markCompleted failed to find TRACKED " + idStr +
                             " size=" + to_string(_incompleteJobs.size());
                // If the user query has been cancelled, this is expected for jobs that have not yet
                // been tracked. Otherwise, this indicates a serious problem.
                if (!getCancelled()) {
                    LOGS(_log, LOG_LVL_WARN, msg << " " << _getIncompleteJobsString(-1));
                    throw util::Bug(ERR_LOC, msg);
                } else {
                    LOGS(_log, LOG_LVL_DEBUG, msg);
                }
                return;
            }
        }
        LOGS(_log, LOG_LVL_WARN,
             "Executive: error executing " << err << " (status: " << err.getStatus() << ")");
        {
            lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
            auto job = _jobMap[jobId];
            string id = job->getIdStr() + "<>" + idStr;

            // Don't overwrite existing error states.
            job->getStatus()->updateInfoNoErrorOverwrite(id, qmeta::JobStatus::RESULT_ERROR, "EXECFAIL",
                                                         err.getCode(), err.getMsg());
        }
        {
            lock_guard<mutex> lock(_errorsMutex);
            _multiError.push_back(err);
            LOGS(_log, LOG_LVL_TRACE,
                 "Currently " << _multiError.size() << " registered errors: " << _multiError);
        }
    }
    _unTrack(jobId);
    if (!success && !isRowLimitComplete()) {
        auto logLvl = (_cancelled) ? LOG_LVL_ERROR : LOG_LVL_TRACE;
        LOGS(_log, logLvl,
             "Executive: requesting cancel, cause: " << " failed (code=" << err.getCode() << " "
                                                     << err.getMsg() << ")");
        squash(string("markComplete error ") + err.getMsg());  // ask to squash
    }
}

void Executive::squash(string const& note) {
    bool alreadyCancelled = _cancelled.exchange(true);
    if (alreadyCancelled) {
        LOGS(_log, LOG_LVL_DEBUG, "Executive::squash() already cancelled! refusing. qid=" << getId());
        return;
    }

    LOGS(_log, LOG_LVL_WARN,
         "Executive::squash Trying to cancel all queries... qid=" << getId() << " " << note);
    deque<JobQuery::Ptr> jobsToCancel;
    {
        lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
        for (auto const& jobEntry : _jobMap) {
            jobsToCancel.push_back(jobEntry.second);
        }
    }

    for (auto const& job : jobsToCancel) {
        job->cancel();
    }

    // Send a message to all workers saying this czarId + queryId is cancelled.
    // The workers will just mark all associated tasks as cancelled, and that should be it.
    // Any message to this czar about this query should result in an error sent back to
    // the worker as soon it can't locate an executive or the executive says it was
    // cancelled.
    bool const deleteResults = true;
    sendWorkersEndMsg(deleteResults);
    LOGS(_log, LOG_LVL_DEBUG, "Executive::squash done");
}

void Executive::_squashSuperfluous() {
    if (_cancelled) {
        LOGS(_log, LOG_LVL_INFO, "squashSuperfluous() irrelevant as query already cancelled");
        return;
    }

    LOGS(_log, LOG_LVL_INFO, "Executive::squashSuperflous Trying to cancel incomplete jobs");
    deque<JobQuery::Ptr> jobsToCancel;
    {
        lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
        for (auto const& jobEntry : _jobMap) {
            JobQuery::Ptr jq = jobEntry.second;
            // It's important that none of the cancelled queries
            // try to remove their rows from the result.
            if (jq->getStatus()->getInfo().state != qmeta::JobStatus::COMPLETE &&
                jq->getStatus()->getInfo().state != qmeta::JobStatus::CANCEL) {
                jobsToCancel.push_back(jobEntry.second);
            }
        }
    }

    for (auto const& job : jobsToCancel) {
        job->cancel(true);
    }

    bool const keepResults = false;
    sendWorkersEndMsg(keepResults);
    LOGS(_log, LOG_LVL_DEBUG, "Executive::squashSuperfluous done");
}

void Executive::sendWorkersEndMsg(bool deleteResults) {
    LOGS(_log, LOG_LVL_INFO, cName(__func__) << " terminating this query deleteResults=" << deleteResults);
    auto cz = czar::Czar::getCzar();
    if (cz != nullptr) {  // Possible in unit tests.
        cz->getCzarRegistry()->endUserQueryOnWorkers(_id, deleteResults);
    }
}

void Executive::killIncompleteUberJobsOnWorker(std::string const& workerId) {
    if (_cancelled) {
        LOGS(_log, LOG_LVL_INFO, cName(__func__) << " irrelevant as query already cancelled");
        return;
    }

    LOGS(_log, LOG_LVL_INFO, cName(__func__) << " killing incomplete UberJobs on " << workerId);
    deque<UberJob::Ptr> ujToCancel;
    {
        lock_guard<mutex> lockUJMap(_uberJobsMapMtx);
        for (auto const& [ujKey, ujPtr] : _uberJobsMap) {
            auto ujStatus = ujPtr->getStatus()->getState();
            if (ujStatus != qmeta::JobStatus::RESPONSE_DONE && ujStatus != qmeta::JobStatus::COMPLETE) {
                // RESPONSE_DONE indicates the result file has been read by
                // the czar, so before that point the worker's data is
                // likely destroyed. COMPLETE indicates all jobs in the
                // UberJob are complete.
                if (ujPtr->getWorkerContactInfo()->wId == workerId) {
                    ujToCancel.push_back(ujPtr);
                }
            }
        }
    }

    for (auto const& uj : ujToCancel) {
        uj->killUberJob();
        uj->setStatusIfOk(qmeta::JobStatus::CANCEL, getIdStr() + " killIncomplete on worker=" + workerId);
    }
}

int Executive::getNumInflight() const {
    unique_lock<mutex> lock(_incompleteJobsMutex);
    return _incompleteJobs.size();
}

string Executive::getProgressDesc() const {
    ostringstream os;
    {
        lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
        auto first = true;
        for (auto entry : _jobMap) {
            JobQuery::Ptr job = entry.second;
            if (!first) {
                os << "\n";
            }
            first = false;
            os << "Ref=" << entry.first << " " << job;
        }
    }
    string msg_progress = os.str();
    LOGS(_log, LOG_LVL_ERROR, msg_progress);
    return msg_progress;
}

/** Add (jobId,r) entry to _requesters map if not here yet
 *  else leave _requesters untouched.
 *
 *  @param jobId id of the job related to current chunk query
 *  @param r pointer to job which will store chunk query result
 *
 *  @return true if (jobId,r) was added to _requesters
 *          false if this entry was previously in the map
 */
bool Executive::_track(int jobId, shared_ptr<JobQuery> const& r) {
    int size = -1;
    {
        lock_guard<mutex> lock(_incompleteJobsMutex);
        if (_incompleteJobs.find(jobId) != _incompleteJobs.end()) {
            LOGS(_log, LOG_LVL_WARN,
                 "Attempt for TRACKING " << " failed as jobId already found in incomplete jobs. "
                                         << _getIncompleteJobsString(-1));
            return false;
        }
        _incompleteJobs[jobId] = r;
        size = _incompleteJobs.size();
        qdisp::CzarStats::get()->addJob();
    }
    LOGS(_log, LOG_LVL_DEBUG, "Success TRACKING size=" << size);
    return true;
}

void Executive::_unTrack(int jobId) {
    bool untracked = false;
    int incompleteJobs = _totalJobs;
    string s;
    bool logSome = false;
    {
        lock_guard<mutex> lock(_incompleteJobsMutex);
        auto i = _incompleteJobs.find(jobId);
        if (i != _incompleteJobs.end()) {
            _incompleteJobs.erase(i);
            untracked = true;
            incompleteJobs = _incompleteJobs.size();
            if (_incompleteJobs.empty()) _allJobsComplete.notify_all();
            qdisp::CzarStats::get()->deleteJobs(1);
        }
        auto sz = _incompleteJobs.size();
        logSome = (sz < 50) || (sz % 1000 == 0) || !untracked;
        if (logSome || LOG_CHECK_LVL(_log, LOG_LVL_DEBUG)) {
            // Log up to 5 incomplete jobs. Very useful when jobs do not finish.
            s = _getIncompleteJobsString(5);
        }
    }
    bool logDebug = untracked || isRowLimitComplete();
    LOGS(_log, (logDebug ? LOG_LVL_DEBUG : LOG_LVL_WARN),
         "Executive UNTRACKING " << (untracked ? "success" : "failed") << "::" << s);
    // Every time a chunk completes, consider sending an update to QMeta.
    // Important chunks to log: first, last, middle
    // limiting factors: no more than one update a minute (config)
    if (untracked) {
        auto now = chrono::system_clock::now();
        unique_lock<mutex> lastUpdateLock(_lastQMetaMtx);
        if (now - _lastQMetaUpdate > _secondsBetweenQMetaUpdates || incompleteJobs == _totalJobs / 2 ||
            incompleteJobs == 0) {
            _lastQMetaUpdate = now;
            lastUpdateLock.unlock();  // unlock asap, _qMeta write can be slow.
            int completedJobs = _totalJobs - incompleteJobs;
            if (_qMeta != nullptr) {
                // This is not vital (logging), if it fails keep going.
                try {
                    _qMeta->queryStatsTmpChunkUpdate(_id, completedJobs);
                } catch (qmeta::SqlError const& e) {
                    LOGS(_log, LOG_LVL_WARN, "Failed to update StatsTmp " << e.what());
                }
            }
        }
    }
}

/// _incompleteJobsMutex must be held before calling this function.
/// @return: a string containing a list of incomplete jobs containing up to 'maxToList' jobs.
///          If maxToList is less than 0, all jobs are printed
string Executive::_getIncompleteJobsString(int maxToList) {
    ostringstream os;
    int c = 0;
    if (maxToList < 0) maxToList = _incompleteJobs.size();
    os << "_incompleteJobs listing first" << maxToList << " of (size=" << _incompleteJobs.size() << ") ";
    for (auto j = _incompleteJobs.begin(), e = _incompleteJobs.end(); j != e && c < maxToList; ++j, ++c) {
        os << j->first << " ";
    }
    return os.str();
}

void Executive::updateProxyMessages() {
    {
        // Add all messages to the message store. These will
        // be used to populate qservMeta.QMessages for this query.
        lock_guard<recursive_mutex> lockJobMap(_jobMapMtx);
        for (auto const& entry : _jobMap) {
            JobQuery::Ptr const& job = entry.second;
            auto const& info = job->getStatus()->getInfo();
            ostringstream os;
            os << info.state << " " << info.stateCode;
            if (!info.stateDesc.empty()) {
                os << " (" << info.stateDesc << ")";
            }
            os << " " << info.timeStr();
            _messageStore->addMessage(job->getDescription()->resource().chunk(), info.source, info.state,
                                      os.str(), info.severity, info.stateTime);
        }
    }
    {
        lock_guard<mutex> lock(_errorsMutex);
        // If there were any errors, combine them into one string and add that to
        // the _messageStore. This will be passed to the proxy for the user, if
        // there's an error.
        if (not _multiError.empty()) {
            _messageStore->addErrorMessage("MULTIERROR", _multiError.toString());
            LOGS(_log, LOG_LVL_INFO, "MULTIERROR:" << _multiError.toString());
        }
    }
}

/// This function blocks until it has reaped all the requesters.
/// Typically the requesters are handled by markCompleted().
/// _reapRequesters() deals with cases that involve errors.
void Executive::_waitAllUntilEmpty() {
    unique_lock<mutex> lock(_incompleteJobsMutex);
    int lastCount = -1;
    int count;
    int moreDetailThreshold = 10;
    int complainCount = 0;
    const chrono::seconds statePrintDelay(5);
    // Loop until all jobs have completed and all jobs have been created.
    while (!_incompleteJobs.empty()) {
        count = _incompleteJobs.size();
        if (count != lastCount) {
            lastCount = count;
            ++complainCount;
            if (LOG_CHECK_LVL(_log, LOG_LVL_DEBUG)) {
                ostringstream os;
                if (complainCount > moreDetailThreshold) {
                    _printState(os);
                    os << "\n";
                }
                os << "Still " << count << " in flight.";
                complainCount = 0;
                lock.unlock();  // release the lock while we trigger logging.
                LOGS(_log, LOG_LVL_INFO, os.str());
                lock.lock();
            }
        }
        _allJobsComplete.wait_for(lock, statePrintDelay);
    }
}

void Executive::_addToChunkJobMap(JobQuery::Ptr const& job) {
    int chunkId = job->getDescription()->resource().chunk();
    auto entry = pair<ChunkIdType, JobQuery::Ptr>(chunkId, job);
    lock_guard<mutex> lck(_chunkToJobMapMtx);
    bool inserted = _chunkToJobMap.insert(entry).second;
    if (!inserted) {
        throw util::Bug(ERR_LOC, "map insert FAILED ChunkId=" + to_string(chunkId) + " already existed");
    }
}

void Executive::_setupLimit() {
    // Figure out the limit situation.
    auto qSession = _querySession.lock();
    // if qSession is nullptr, this is probably a unit test.
    if (qSession == nullptr) return;
    auto const& selectStatement = qSession->getStmt();
    bool allChunksRequired = qSession->dbgGetContext()->allChunksRequired;
    bool groupBy = selectStatement.hasGroupBy();
    bool orderBy = selectStatement.hasOrderBy();
    bool hasLimit = selectStatement.hasLimit();
    if (hasLimit) {
        _limit = selectStatement.getLimit();
        if (_limit <= 0) hasLimit = false;
    }
    _limitSquashApplies = hasLimit && !(groupBy || orderBy || allChunksRequired);
}

int Executive::getUjRowLimit() const {
    if (_limitSquashApplies) {
        return _limit;
    }
    return 0;
}

void Executive::addResultRows(int64_t rowCount) { _totalResultRows += rowCount; }

void Executive::checkLimitRowComplete() {
    if (!_limitSquashApplies) return;
    if (_totalResultRows < _limit) return;
    bool previousVal = _setLimitRowComplete();
    if (previousVal) {
        // already squashing etc, just return
        return;
    }
    // Set flags so queries can be squashed without canceling the entire query.
    // To explain WARN messages in the log related to this action, this
    // message is LOG_LVL_WARN.
    LOGS(_log, LOG_LVL_WARN, "LIMIT query has enough rows, canceling superfluous jobs.");
    _squashSuperfluous();
}

void Executive::checkResultFileSize(uint64_t fileSize) {
    _totalResultFileSize += fileSize;
    if (_cancelled) return;

    size_t const MB_SIZE_BYTES = 1024 * 1024;
    uint64_t maxResultTableSizeBytes = cconfig::CzarConfig::instance()->getMaxTableSizeMB() * MB_SIZE_BYTES;
    LOGS(_log, LOG_LVL_TRACE,
         cName(__func__) << " sz=" << fileSize << " total=" << _totalResultFileSize
                         << " max=" << maxResultTableSizeBytes);
    if (_totalResultFileSize > maxResultTableSizeBytes) {
        LOGS(_log, LOG_LVL_WARN,
             cName(__func__) << " total=" << _totalResultFileSize << " max=" << maxResultTableSizeBytes);
        // _totalResultFileSize may include non zero values from dead UberJobs,
        // so recalculate it to verify.
        uint64_t total = 0;
        {
            lock_guard<mutex> lck(_uberJobsMapMtx);
            for (auto const& [ujId, ujPtr] : _uberJobsMap) {
                total += ujPtr->getResultFileSize();
            }
            _totalResultFileSize = total;
        }
        LOGS(_log, LOG_LVL_WARN,
             cName(__func__) << "recheck total=" << total << " max=" << maxResultTableSizeBytes);
        if (total > maxResultTableSizeBytes) {
            LOGS(_log, LOG_LVL_ERROR, "Executive: requesting squash, result file size too large " << total);
            ResponseHandler::Error err(util::ErrorCode::CZAR_RESULT_TOO_LARGE,
                                       string("Incomplete result already too large ") + to_string(total));
            _multiError.push_back(err);
            squash("czar, file too large");
        }
    }
}

ostream& operator<<(ostream& os, Executive::JobMap::value_type const& v) {
    auto const& status = v.second->getStatus();
    os << v.first << ": " << *status;
    return os;
}

/// precondition: _incompleteJobsMutex is held by current thread.
void Executive::_printState(ostream& os) {
    for (auto const& entry : _incompleteJobs) {
        JobQuery::Ptr job = entry.second;
        os << *job << "\n";
    }
}

}  // namespace lsst::qserv::qdisp
