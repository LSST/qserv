// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2016 AURA/LSST.
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
 * @brief Task is a bundle of query task fields
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "wbase/Task.h"

// System headers
#include <ctime>
#include <stdexcept>

// Third-party headers
#include <boost/algorithm/string/replace.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/constants.h"
#include "global/LogContext.h"
#include "global/UnsupportedError.h"
#include "http/RequestBodyJSON.h"
#include "mysql/MySqlConfig.h"
#include "proto/worker.pb.h"
#include "protojson/UberJobMsg.h"
#include "util/Bug.h"
#include "util/common.h"
#include "util/HoldTrack.h"
#include "util/IterableFormatter.h"
#include "util/TimeUtils.h"
#include "wbase/Base.h"
#include "wbase/FileChannelShared.h"
#include "wbase/UberJobData.h"
#include "wbase/UserQueryInfo.h"
#include "wconfig/WorkerConfig.h"
#include "wdb/QueryRunner.h"
#include "wpublish/QueriesAndChunks.h"

using namespace std;
using namespace std::chrono_literals;
using namespace nlohmann;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.wbase.Task");

size_t const MB_SIZE_BYTES = 1024 * 1024;

}  // namespace

namespace lsst::qserv::wbase {

// Task::ChunkEqual functor
bool Task::ChunkEqual::operator()(Task::Ptr const& x, Task::Ptr const& y) {
    if (!x || !y) {
        return false;
    }
    return x->_chunkId == y->_chunkId;
}

// Task::PtrChunkIdGreater functor
bool Task::ChunkIdGreater::operator()(Task::Ptr const& x, Task::Ptr const& y) {
    if (!x || !y) {
        return false;
    }
    return x->_chunkId > y->_chunkId;
}

string const Task::defaultUser = "qsmaster";

TaskScheduler::TaskScheduler() {
    auto hour = chrono::milliseconds(1h);
    histTimeOfRunningTasks = util::HistogramRolling::Ptr(
            new util::HistogramRolling("RunningTaskTimes", {0.1, 1.0, 10.0, 100.0, 200.0}, hour, 10'000));
    histTimeOfTransmittingTasks = util::HistogramRolling::Ptr(new util::HistogramRolling(
            "TransmittingTaskTime", {0.1, 1.0, 10.0, 60.0, 600.0, 1200.0}, hour, 10'000));
}

atomic<uint32_t> taskSequence{0};  ///< Unique identifier source for Task.

/// When the constructor is called, there is not enough information
/// available to define the action to take when this task is run, so
/// Command::setFunc() is used set the action later. This is why
/// the util::CommandThreadPool is not called here.
Task::Task(UberJobData::Ptr const& ujData, int jobId, int attemptCount, int chunkId, int fragmentNumber,
           size_t templateId, bool hasSubchunks, int subchunkId, string const& db,
           vector<TaskDbTbl> const& fragSubTables, vector<int> const& fragSubchunkIds,
           shared_ptr<FileChannelShared> const& sc,
           std::shared_ptr<wpublish::QueryStatistics> const& queryStats_)
        : _sendChannel(sc),
          _tSeq(++taskSequence),
          _qId(ujData->getQueryId()),
          _templateId(templateId),
          _hasChunkId((chunkId >= 0)),
          _chunkId(chunkId),
          _subchunkId(subchunkId),
          _jId(jobId),
          _attemptCount(attemptCount),
          _queryFragmentNum(fragmentNumber),
          _fragmentHasSubchunks(hasSubchunks),
          _db(db),
          _czarId(ujData->getCzarId()),
          _queryStats(queryStats_),
          _rowLimit(ujData->getRowLimit()),
          _ujData(ujData),
          _idStr(ujData->getIdStr() + " jId=" + to_string(_jId) + " sc=" + to_string(_subchunkId)) {
    user = defaultUser;

    // Create sets and vectors for 'aquiring' subchunk temporary tables.
    // Fill in _dbTblsAndSubchunks
    DbTableSet dbTbls_;
    IntVector subchunksVect_;
    if (!_fragmentHasSubchunks) {
        /// FUTURE: Why acquire anything if there are no subchunks in the fragment?
        ///   This branch never seems to happen, but this needs to be proven beyond any doubt.
        auto scanInfo = _ujData->getScanInfo();
        for (auto const& scanTbl : scanInfo->infoTables) {
            dbTbls_.emplace(scanTbl.db, scanTbl.table);
            LOGS(_log, LOG_LVL_TRACE,
                 "Task::Task scanTbl.db=" << scanTbl.db << " scanTbl.table=" << scanTbl.table);
        }
        LOGS(_log, LOG_LVL_TRACE,
             "fragment a db=" << _db << ":" << _chunkId << " dbTbls=" << util::printable(dbTbls_));
    } else {
        for (TaskDbTbl const& fDbTbl : fragSubTables) {
            /// Different subchunk fragments can require different tables.
            /// FUTURE: It may save space to store these in UserQueryInfo as it seems
            ///         database and table names are consistent across chunks.
            dbTbls_.emplace(fDbTbl.db, fDbTbl.tbl);
            LOGS(_log, LOG_LVL_TRACE,
                 "Task::Task subchunk fDbTbl.db=" << fDbTbl.db << " fDbTbl.tbl=" << fDbTbl.tbl);
        }
        subchunksVect_ = fragSubchunkIds;

        LOGS(_log, LOG_LVL_TRACE,
             "fragment b db=" << _db << ":" << _chunkId << " dbTableSet" << util::printable(dbTbls_)
                              << " subChunks=" << util::printable(subchunksVect_));
    }

    _dbTblsAndSubchunks = make_unique<DbTblsAndSubchunks>(dbTbls_, subchunksVect_);

    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " created");
}

Task::~Task() {}

std::vector<Task::Ptr> Task::createTasksFromUberJobMsg(
        std::shared_ptr<protojson::UberJobMsg> const& ujMsg, std::shared_ptr<UberJobData> const& ujData,
        std::shared_ptr<wbase::FileChannelShared> const& sendChannel,
        std::shared_ptr<wdb::ChunkResourceMgr> const& chunkResourceMgr, mysql::MySqlConfig const& mySqlConfig,
        std::shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr,
        std::shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks) {
    QueryId qId = ujData->getQueryId();
    UberJobId ujId = ujData->getUberJobId();
    CzarIdType czId = ujData->getCzarId();

    vector<Task::Ptr> vect;  // List of created tasks to be returned.
    wpublish::QueryStatistics::Ptr queryStats = queriesAndChunks->addQueryId(qId, czId);
    UserQueryInfo::Ptr userQueryInfo = queryStats->getUserQueryInfo();

    string funcN(__func__);
    funcN += " QID=" + to_string(qId) + " ";

    if (ujMsg->getQueryId() != qId) {
        throw util::Bug(ERR_LOC, "Task::createTasksFromUberJobMsg qId(" + to_string(qId) +
                                         ") did not match ujMsg->qId(" + to_string(ujMsg->getQueryId()) +
                                         ")");
    }
    if (ujMsg->getUberJobId() != ujId) {
        throw util::Bug(ERR_LOC, "Task::createTasksFromUberJobMsg ujId(" + to_string(ujId) +
                                         ") did not match ujMsg->qId(" + to_string(ujMsg->getUberJobId()) +
                                         ")");
    }

    std::string workerId = ujMsg->getWorkerId();
    auto jobSubQueryTempMap = ujMsg->getJobSubQueryTempMap();
    auto jobDbTablesMap = ujMsg->getJobDbTablesMap();
    auto jobMsgVect = ujMsg->getJobMsgVect();

    for (auto const& jobMsg : *jobMsgVect) {
        JobId jobId = jobMsg->getJobId();
        int attemptCount = jobMsg->getAttemptCount();
        std::string chunkQuerySpecDb = jobMsg->getChunkQuerySpecDb();
        int chunkId = jobMsg->getChunkId();

        std::vector<int> chunkScanTableIndexes = jobMsg->getChunkScanTableIndexes();
        auto jobFragments = jobMsg->getJobFragments();
        int fragmentNumber = 0;

        for (auto const& fMsg : *jobFragments) {
            // These need to be constructed for the fragment
            vector<string> fragSubQueries;
            vector<TaskDbTbl> fragSubTables;
            vector<int> fragSubchunkIds;

            vector<int> fsqIndexes = fMsg->getJobSubQueryTempIndexes();
            for (int fsqIndex : fsqIndexes) {
                string fsqStr = jobSubQueryTempMap->getSubQueryTemp(fsqIndex);
                fragSubQueries.push_back(fsqStr);
            }

            vector<int> dbTblIndexes = fMsg->getJobDbTablesIndexes();
            for (int dbTblIndex : dbTblIndexes) {
                auto [scDb, scTable] = jobDbTablesMap->getDbTable(dbTblIndex);
                TaskDbTbl scDbTbl(scDb, scTable);
                fragSubTables.push_back(scDbTbl);
            }

            fragSubchunkIds = fMsg->getSubchunkIds();

            for (string const& fragSubQ : fragSubQueries) {
                size_t templateId = userQueryInfo->addTemplate(fragSubQ);
                if (fragSubchunkIds.empty()) {
                    bool const noSubchunks = false;
                    int const subchunkId = -1;
                    auto task = Task::Ptr(new Task(ujData, jobId, attemptCount, chunkId, fragmentNumber,
                                                   templateId, noSubchunks, subchunkId, chunkQuerySpecDb,
                                                   fragSubTables, fragSubchunkIds, sendChannel, queryStats));

                    vect.push_back(task);
                } else {
                    for (auto subchunkId : fragSubchunkIds) {
                        bool const hasSubchunks = true;
                        auto task =
                                Task::Ptr(new Task(ujData, jobId, attemptCount, chunkId, fragmentNumber,
                                                   templateId, hasSubchunks, subchunkId, chunkQuerySpecDb,
                                                   fragSubTables, fragSubchunkIds, sendChannel, queryStats));
                        vect.push_back(task);
                    }
                }
            }
            ++fragmentNumber;
        }
    }

    for (auto taskPtr : vect) {
        // newQueryRunner sets the `_taskQueryRunner` pointer in `task`.
        taskPtr->setTaskQueryRunner(wdb::QueryRunner::newQueryRunner(taskPtr, chunkResourceMgr, mySqlConfig,
                                                                     sqlConnMgr, queriesAndChunks));
    }

    return vect;
}

std::vector<Task::Ptr> Task::createTasksForUnitTest(
        std::shared_ptr<UberJobData> const& ujData, nlohmann::json const& jsJobs,
        std::shared_ptr<wbase::FileChannelShared> const& sendChannel, int maxTableSizeMb,
        std::shared_ptr<wdb::ChunkResourceMgr> const& chunkResourceMgr) {
    QueryId qId = ujData->getQueryId();
    UberJobId ujId = ujData->getUberJobId();
    CzarIdType czId = ujData->getCzarId();
    string funcN(__func__);
    funcN += " QID=" + to_string(qId) + " czId=" + to_string(czId);

    vector<Task::Ptr> vect;
    for (auto const& job : jsJobs) {
        json const& jsJobDesc = job["jobdesc"];
        http::RequestBodyJSON rbJobDesc(jsJobDesc);
        // See qproc::TaskMsgFactory::makeMsgJson for message construction.
        auto const jdCzarId = rbJobDesc.required<qmeta::CzarId>("czarId");
        auto const jdQueryId = rbJobDesc.required<QueryId>("queryId");
        if (jdQueryId != qId) {
            throw TaskException(ERR_LOC, string("ujId=") + to_string(ujId) + " qId=" + to_string(qId) +
                                                 " QueryId mismatch Job qId=" + to_string(jdQueryId));
        }
        auto const jdJobId = rbJobDesc.required<int>("jobId");
        auto const jdAttemptCount = rbJobDesc.required<int>("attemptCount");
        auto const jdQuerySpecDb = rbJobDesc.required<string>("querySpecDb");
        auto const jdMaxTableSizeMb = rbJobDesc.required<int>("maxTableSize");
        auto const jdChunkId = rbJobDesc.required<int>("chunkId");
        LOGS(_log, LOG_LVL_TRACE,
             funcN << " jd cid=" << jdCzarId << " jdQId=" << jdQueryId << " jdJobId=" << jdJobId
                   << " jdAtt=" << jdAttemptCount << " jdQDb=" << jdQuerySpecDb
                   << " maxTblSz=" << jdMaxTableSizeMb << " chunkId=" << jdChunkId);

        auto const jdQueryFragments = rbJobDesc.required<json>("queryFragments");
        int fragmentNumber = 0;
        for (auto const& frag : jdQueryFragments) {
            vector<string> fragSubQueries;
            vector<int> fragSubchunkIds;
            vector<TaskDbTbl> fragSubTables;
            LOGS(_log, LOG_LVL_DEBUG, funcN << " frag=" << frag);
            http::RequestBodyJSON rbFrag(frag);
            auto const& jsQueries = rbFrag.required<json>("queries");
            // TODO:UJ move to uberjob???, these should be the same for all jobs
            for (auto const& subQ : jsQueries) {
                http::RequestBodyJSON rbSubQ(subQ);
                auto const subQuery = rbSubQ.required<string>("subQuery");
                LOGS(_log, LOG_LVL_DEBUG, funcN << " subQuery=" << subQuery);
                fragSubQueries.push_back(subQuery);
            }
            auto const& resultTable = rbFrag.required<string>("resultTable");
            auto const& jsSubIds = rbFrag.required<json>("subchunkIds");
            for (auto const& scId : jsSubIds) {
                fragSubchunkIds.push_back(scId);
            }
            auto const& jsSubTables = rbFrag.required<json>("subchunkTables");

            for (auto const& scDbTable : jsSubTables) {  // TODO:UJ are these the same for all jobs?
                http::RequestBodyJSON rbScDbTable(scDbTable);
                string scDb = rbScDbTable.required<string>("scDb");
                string scTable = rbScDbTable.required<string>("scTable");
                TaskDbTbl scDbTbl(scDb, scTable);
                fragSubTables.push_back(scDbTbl);
            }

            for (string const& fragSubQ : fragSubQueries) {
                LOGS(_log, LOG_LVL_DEBUG, "ignoring " << fragSubQ);
                if (fragSubchunkIds.empty()) {
                    bool const noSubchunks = false;
                    int const subchunkId = -1;
                    auto task = Task::Ptr(new Task(ujData, jdJobId, jdAttemptCount, jdChunkId, fragmentNumber,
                                                   0, noSubchunks, subchunkId, jdQuerySpecDb, fragSubTables,
                                                   fragSubchunkIds, sendChannel, nullptr));

                    vect.push_back(task);
                } else {
                    for (auto subchunkId : fragSubchunkIds) {
                        bool const hasSubchunks = true;
                        auto task =
                                Task::Ptr(new Task(ujData, jdJobId, jdAttemptCount, jdChunkId, fragmentNumber,
                                                   0, hasSubchunks, subchunkId, jdQuerySpecDb, fragSubTables,
                                                   fragSubchunkIds, sendChannel, nullptr));
                        vect.push_back(task);
                    }
                }
            }
            ++fragmentNumber;
        }
    }

    return vect;
}

protojson::ScanInfo::Ptr Task::getScanInfo() const { return _ujData->getScanInfo(); }

bool Task::getScanInteractive() const { return _ujData->getScanInteractive(); }

void Task::action(util::CmdData* data) {
    string tIdStr = getIdStr();
    if (_queryStarted.exchange(true)) {
        LOGS(_log, LOG_LVL_WARN, "task was already started " << tIdStr);
        return;
    }

    if (_unitTest) {
        LOGS(_log, LOG_LVL_ERROR,
             __func__ << " Command::_func has been set, this should only happen in unit tests.");
        _func(data);
        return;
    }

    // Get a local copy for safety.
    auto qr = _taskQueryRunner;
    bool success = false;
    string errStr;
    try {
        success = qr->runQuery();
    } catch (UnsupportedError const& e) {
        LOGS(_log, LOG_LVL_ERROR, __func__ << " runQuery threw UnsupportedError " << e.what() << tIdStr);
        errStr = e.what();
    }
    if (not success) {
        LOGS(_log, LOG_LVL_ERROR, "runQuery failed " << tIdStr);
        if (not getSendChannel()->kill("Foreman::_setRunFunc")) {
            LOGS(_log, LOG_LVL_WARN, "runQuery sendChannel already killed " << tIdStr);
        }
        // Send a message back saying this UberJobFailed, redundant error messages should be
        // harmless.
        util::MultiError multiErr;
        util::Error err(_chunkId, string("UberJob run error ") + errStr);
        multiErr.push_back(err);
        _ujData->responseError(multiErr, -1, false);
    }

    // The QueryRunner class access to sendChannel for results is over by this point.
    // 'task' contains statistics that are still useful. However, the resources used
    // by sendChannel need to be freed quickly.
    LOGS(_log, LOG_LVL_TRACE, __func__ << " calling resetSendChannel() for " << tIdStr);
    resetSendChannel();  // Frees the SendChannel instance
}

string Task::getQueryString() const {
    auto qStats = _queryStats.lock();
    if (qStats == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " _queryStats could not be locked");
        return string("");
    }

    auto uQInfo = qStats->getUserQueryInfo();
    string qs = uQInfo->getTemplate(_templateId);
    boost::algorithm::replace_all(qs, CHUNK_TAG, to_string(_chunkId));
    boost::algorithm::replace_all(qs, SUBCHUNK_TAG, to_string(_subchunkId));
    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " qs=" << qs);
    return qs;
}

wpublish::QueryStatistics::Ptr Task::getQueryStats() const {
    auto qStats = _queryStats.lock();
    if (qStats == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, "Task::getQueryStats() _queryStats==null " << getIdStr());
    }
    return qStats;
}

/// Flag the Task as cancelled, try to stop the SQL query, and try to remove it from the schedule.
void Task::cancel() {
    if (_cancelled.exchange(true)) {
        // Was already cancelled.
        return;
    }

    util::HoldTrack::Mark markA(ERR_LOC, "Task::cancel");
    LOGS(_log, LOG_LVL_DEBUG, "Task::cancel " << getIdStr());
    auto qr = _taskQueryRunner;  // Need a copy in case _taskQueryRunner is reset.
    if (qr != nullptr) {
        qr->cancel();
    }

    // At this point, this code doesn't do anything. It may be
    // useful to remove this task from the scheduler, but it
    // seems doubtful that that would improve performance.
    auto sched = _taskScheduler.lock();
    if (sched != nullptr) {
        sched->taskCancelled(this);
    }
}

bool Task::checkCancelled() {
    // The czar does tell the worker a query id is cancelled.
    // Returning true here indicates there's no point in doing
    // any more processing for this Task.
    if (_cancelled) return true;
    if (_sendChannel == nullptr || _sendChannel->isDead() || _sendChannel->isRowLimitComplete()) {
        cancel();
    }
    return _cancelled;
}

/// @return true if task has already been cancelled.
bool Task::setTaskQueryRunner(TaskQueryRunner::Ptr const& taskQueryRunner) {
    _taskQueryRunner = taskQueryRunner;
    return checkCancelled();
}

void Task::freeTaskQueryRunner(TaskQueryRunner* tqr) {
    if (_taskQueryRunner.get() == tqr) {
        _taskQueryRunner.reset();
    } else {
        LOGS(_log, LOG_LVL_WARN, "Task::freeTaskQueryRunner pointer didn't match!");
    }
}

/// Set values associated with the Task being put on the queue.
void Task::queued(chrono::system_clock::time_point const& now) {
    lock_guard<mutex> guard(_stateMtx);
    _state = TaskState::QUEUED;
    _queueTime = now;
}

bool Task::isRunning() const {
    lock_guard<mutex> lock(_stateMtx);
    switch (_state) {
        case TaskState::STARTED:
        case TaskState::EXECUTING_QUERY:
        case TaskState::READING_DATA:
            return true;
        default:
            return false;
    }
}

void Task::started(chrono::system_clock::time_point const& now) {
    LOGS(_log, LOG_LVL_TRACE, __func__ << " " << getIdStr() << " started");
    lock_guard<mutex> guard(_stateMtx);
    _state = TaskState::STARTED;
    _startTime = now;
}

void Task::queryExecutionStarted() {
    LOGS(_log, LOG_LVL_TRACE, __func__ << " " << getIdStr() << " executing");
    lock_guard<mutex> guard(_stateMtx);
    _state = TaskState::EXECUTING_QUERY;
    _queryExecTime = chrono::system_clock::now();
}

void Task::queried() {
    LOGS(_log, LOG_LVL_TRACE, __func__ << " " << getIdStr() << " reading");
    lock_guard<mutex> guard(_stateMtx);
    _state = TaskState::READING_DATA;
    _queryTime = chrono::system_clock::now();
    // Reset finish time as it might be already set when the task got booted off
    // a scheduler.
    _finishTime = chrono::system_clock::time_point();
}

/// Set values associated with the Task being finished.
/// @return milliseconds to complete the Task, system clock time.
chrono::milliseconds Task::finished(chrono::system_clock::time_point const& now) {
    LOGS(_log, LOG_LVL_TRACE, __func__ << " " << getIdStr() << " finished");
    chrono::milliseconds duration;
    {
        lock_guard<mutex> guard(_stateMtx);
        _finishTime = now;
        _state = TaskState::FINISHED;
        duration = chrono::duration_cast<chrono::milliseconds>(_finishTime - _startTime);
    }
    // Ensure that the duration is greater than 0.
    if (duration.count() < 1) {
        duration = chrono::milliseconds{1};
    }
    LOGS(_log, LOG_LVL_TRACE, "processing millisecs=" << duration.count());
    return duration;
}

chrono::milliseconds Task::getRunTime() const {
    lock_guard<mutex> guard(_stateMtx);
    switch (_state) {
        case TaskState::FINISHED:
            return chrono::duration_cast<chrono::milliseconds>(_finishTime - _startTime);
        case TaskState::STARTED:
        case TaskState::EXECUTING_QUERY:
        case TaskState::READING_DATA:
            return chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - _startTime);
        default:
            return chrono::milliseconds(0);
    }
}

/// Wait for MemMan to finish reserving resources. The mlock call can take several seconds
/// and only one mlock call can be running at a time. Further, queries finish slightly faster
/// if they are mlock'ed in the same order they were scheduled, hence the ulockEvents
/// EventThread and CommandMlock class.
void Task::waitForMemMan() {
    if (_memMan != nullptr) {
        if (_memMan->lock(_memHandle, true)) {
            int errorCode = (errno == EAGAIN ? ENOMEM : errno);
            LOGS(_log, LOG_LVL_WARN,
                 "mlock err=" << errorCode << " " << _memMan->getStatistics().logString() << " "
                              << _memMan->getStatus(_memHandle).logString());
        }
        LOGS(_log, LOG_LVL_TRACE,
             "waitForMemMan " << _memMan->getStatistics().logString() << " "
                              << _memMan->getStatus(_memHandle).logString());
    }
    setSafeToMoveRunning(true);
}

memman::MemMan::Status Task::getMemHandleStatus() {
    if (_memMan == nullptr || !hasMemHandle()) {
        return memman::MemMan::Status();
    }
    return _memMan->getStatus(_memHandle);
}

bool Task::setBooted() {
    bool alreadyBooted = _booted.exchange(true);
    if (!alreadyBooted) {
        _bootedTime = CLOCK::now();
    }
    return alreadyBooted;
}

nlohmann::json Task::getJson() const {
    // It would be nice to have the _queryString in this, but that could make the results very large.
    nlohmann::json js;
    js["czarId"] = _czarId;
    js["queryId"] = _qId;
    js["templateId"] = getTemplateId();
    js["jobId"] = _jId;
    js["chunkId"] = getChunkId();
    js["subChunkId"] = getSubchunkId();
    js["fragmentId"] = _queryFragmentNum;
    js["attemptId"] = _attemptCount;
    js["sequenceId"] = _tSeq;
    js["maxTableSize"] = _ujData->getMaxTableSizeBytes();
    js["cancelled"] = to_string(_cancelled);
    js["state"] = static_cast<uint64_t>(_state.load());
    js["createTime_msec"] = util::TimeUtils::tp2ms(_createTime);
    js["queueTime_msec"] = util::TimeUtils::tp2ms(_queueTime);
    js["startTime_msec"] = util::TimeUtils::tp2ms(_startTime);
    js["queryExecTime_msec"] = util::TimeUtils::tp2ms(_queryExecTime);
    js["queryTime_msec"] = util::TimeUtils::tp2ms(_queryTime);
    js["finishTime_msec"] = util::TimeUtils::tp2ms(_finishTime);
    js["sizeSoFar"] = _totalSize;
    js["mysqlThreadId"] = _mysqlThreadId.load();
    js["booted"] = _booted.load() ? 1 : 0;
    js["bootedTime_msec"] = util::TimeUtils::tp2ms(_bootedTime);
    auto const scheduler = getTaskScheduler();
    js["scheduler"] = scheduler == nullptr ? "" : scheduler->getName();
    return js;
}

int64_t Task::getMaxTableSize() const { return _ujData->getMaxTableSizeBytes(); }

ostream& operator<<(ostream& os, Task const& t) {
    os << "Task: "
       << "msg: " << t.getIdStr() << " chunk=" << t._chunkId << " db=" << t._db << " " << t.getQueryString();

    return os;
}

}  // namespace lsst::qserv::wbase
