// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
 * @brief QueryRunner instances perform single-shot query execution with the
 * result reflected in the db state or returned via a SendChannel. Works with
 * new XrdSsi API.
 *
 * @author Daniel L. Wang, SLAC; John Gates, SLAC
 */

// System headers
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <thread>

// Third-party headers
#include <google/protobuf/arena.h>
#include <mysql/mysql.h>

// Class header
#include "wdb/QueryRunner.h"

// LSST headers
#include "lsst/log/Log.h"
#include "global/DbTable.h"
#include "global/LogContext.h"
#include "global/UnsupportedError.h"
#include "mysql/MySqlConfig.h"
#include "mysql/MySqlConnection.h"
#include "mysql/SchemaFactory.h"
#include "proto/ProtoHeaderWrap.h"
#include "proto/worker.pb.h"
#include "sql/Schema.h"
#include "sql/SqlErrorObject.h"
#include "util/Bug.h"
#include "util/common.h"
#include "util/IterableFormatter.h"
#include "util/HoldTrack.h"
#include "util/MultiError.h"
#include "util/Timer.h"
#include "util/threadSafe.h"
#include "wbase/Base.h"
#include "wbase/FileChannelShared.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/SqlConnMgr.h"
#include "wdb/ChunkResource.h"
#include "wpublish/QueriesAndChunks.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wdb.QueryRunner");
}

using namespace std;

namespace lsst::qserv::wdb {

QueryRunner::Ptr QueryRunner::newQueryRunner(wbase::Task::Ptr const& task,
                                             ChunkResourceMgr::Ptr const& chunkResourceMgr,
                                             mysql::MySqlConfig const& mySqlConfig,
                                             shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr,
                                             shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks) {
    Ptr qr(new QueryRunner(task, chunkResourceMgr, mySqlConfig, sqlConnMgr,
                           queriesAndChunks));  // Private constructor.
    return qr;
}

/// New instances need to be made with QueryRunner to ensure registration with the task
/// and correct setup of enable_shared_from_this.
QueryRunner::QueryRunner(wbase::Task::Ptr const& task, ChunkResourceMgr::Ptr const& chunkResourceMgr,
                         mysql::MySqlConfig const& mySqlConfig,
                         shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr,
                         shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks)
        : _task(task),
          _chunkResourceMgr(chunkResourceMgr),
          _mySqlConfig(mySqlConfig),
          _sqlConnMgr(sqlConnMgr),
          _queriesAndChunks(queriesAndChunks) {
    [[maybe_unused]] int rc = mysql_thread_init();
    assert(rc == 0);
}

/// Initialize the db connection
bool QueryRunner::_initConnection() {
    mysql::MySqlConfig localMySqlConfig(_mySqlConfig);
    localMySqlConfig.username = _task->user;  // Override with czar-passed username.
    if (_mysqlConn != nullptr) {
        LOGS(_log, LOG_LVL_ERROR,
             "QueryRunner::_initConnection _mysqlConn not nullptr _mysqlConn=" << _mysqlConn.get());
    }
    _mysqlConn.reset(new mysql::MySqlConnection(localMySqlConfig));

    if (not _mysqlConn->connect()) {
        LOGS(_log, LOG_LVL_ERROR, "Unable to connect to MySQL: " << localMySqlConfig);
        util::Error error(-1, "Unable to connect to MySQL; " + localMySqlConfig.toString());
        _multiError.push_back(error);
        return false;
    }
    _task->setMySqlThreadId(_mysqlConn->threadId());
    return true;
}

/// Override _dbName with _msg->db() if available.
void QueryRunner::_setDb() {
    if (_task->getDb() != "") {
        _dbName = _task->getDb();
        LOGS(_log, LOG_LVL_TRACE, "QueryRunner overriding dbName with " << _dbName);
    }
}

bool QueryRunner::runQuery() {
    util::HoldTrack::Mark runQueryMarkA(ERR_LOC, "runQuery " + to_string(_task->getQueryId()));
    QSERV_LOGCONTEXT_QUERY_JOB(_task->getQueryId(), _task->getJobId());
    LOGS(_log, LOG_LVL_TRACE,
         "QueryRunner " << _task->cName(__func__) << " scsId=" << _task->getSendChannel()->getScsId());

    // Start tracking the task.
    auto now = chrono::system_clock::now();
    _task->started(now);

    // Make certain our Task knows that this object is no longer in use when this function exits.
    class Release {
    public:
        Release(wbase::Task::Ptr t, wbase::TaskQueryRunner* tqr,
                shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks)
                : _t{t}, _tqr{tqr}, _queriesAndChunks(queriesAndChunks) {}
        ~Release() {
            _queriesAndChunks->finishedTask(_t);
            _t->freeTaskQueryRunner(_tqr);
        }

    private:
        wbase::Task::Ptr _t;
        wbase::TaskQueryRunner* _tqr;
        shared_ptr<wpublish::QueriesAndChunks> const _queriesAndChunks;
    };
    Release release(_task, this, _queriesAndChunks);

    if (_task->checkCancelled()) {
        LOGS(_log, LOG_LVL_TRACE, "runQuery, task was cancelled before it started." << _task->getIdStr());
        return false;
    }

    _czarId = _task->getCzarId();

    _setDb();
    LOGS(_log, LOG_LVL_TRACE, "Exec in flight for Db=" << _dbName << " sqlConnMgr " << _sqlConnMgr->dump());
    // Queries that span multiple tasks should not be high priority for the SqlConMgr as it risks deadlock.
    bool interactive = _task->getScanInteractive() && !(_task->getSendChannel()->getTaskCount() > 1);
    wcontrol::SqlConnLock sqlConnLock(*_sqlConnMgr, not interactive, _task->getSendChannel());

    bool connOk = _initConnection();
    if (!connOk) {
        // Since there's an error, this will be the last transmit from this QueryRunner.
        _task->getSendChannel()->buildAndTransmitError(_multiError, _task, _cancelled);
        return false;
    }

    // Run the query and send the results back.
    return _dispatchChannel();
}

MYSQL_RES* QueryRunner::_primeResult(string const& query) {
    util::HoldTrack::Mark mark(ERR_LOC, "QR _primeResult() QID=" + _task->getIdStr());
    bool queryOk = _mysqlConn->queryUnbuffered(query);
    if (!queryOk) {
        sql::SqlErrorObject errObj;
        errObj.setErrNo(_mysqlConn->getErrno());
        errObj.addErrMsg("primeResult error " + _mysqlConn->getError());
        throw errObj;
    }
    return _mysqlConn->getResult();
}

class ChunkResourceRequest {
public:
    using Ptr = std::shared_ptr<ChunkResourceRequest>;

    ChunkResourceRequest(shared_ptr<ChunkResourceMgr> const& mgr, wbase::Task& task)
            // Use old-school member initializers because gcc 4.8.5
            // miscompiles the code when using brace initializers (DM-4704).
            : _mgr(mgr), _task(task) {}

    // Since each Task has only one subchunk, fragment number isn't needed.
    ChunkResource getResourceFragment() {
        if (!_task.getFragmentHasSubchunks()) {
            /// Why acquire anything if there are no subchunks in the fragment?
            /// Future: Need to be certain this never happens before removing.
            return _mgr->acquire(_task.getDb(), _task.getChunkId(), _task.getDbTbls());
        }

        return _mgr->acquire(_task.getDb(), _task.getChunkId(), _task.getDbTbls(), _task.getSubchunksVect());
    }

private:
    shared_ptr<ChunkResourceMgr> const _mgr;
    wbase::Task& _task;
};

bool QueryRunner::_dispatchChannel() {
    bool erred = false;
    bool needToFreeRes = false;  // set to true once there are results to be freed.
    // Collect the result in _transmitData. When a reasonable amount of data has been collected,
    // or there are no more rows to collect, pass _transmitData to _sendChannel.
    ChunkResourceRequest::Ptr req;
    ChunkResource::Ptr cr;
    try {
        util::Timer subChunkT;
        subChunkT.start();
        req.reset(new ChunkResourceRequest(_chunkResourceMgr, *_task));
        cr.reset(new ChunkResource(req->getResourceFragment()));
        subChunkT.stop();
        // TODO: Hold onto this for longer period of time as the odds of reuse are pretty low at this scale
        //       Ideally, hold it until moving on to the next chunk. Try to clean up ChunkResource code.

        auto taskSched = _task->getTaskScheduler();
        if (!_cancelled && !_task->checkCancelled()) {
            string const& query = _task->getQueryString();
            util::Timer primeT;
            primeT.start();
            _task->queryExecutionStarted();
            LOGS(_log, LOG_LVL_TRACE, "QueryRunner " << _task->cName(__func__) << " sql start");
            MYSQL_RES* res = _primeResult(query);  // This runs the SQL query, throws SqlErrorObj on failure.
            LOGS(_log, LOG_LVL_TRACE, "QueryRunner " << _task->cName(__func__) << " sql end");
            primeT.stop();
            needToFreeRes = true;
            if (taskSched != nullptr) {
                taskSched->histTimeOfRunningTasks->addEntry(primeT.getElapsed());
                LOGS(_log, LOG_LVL_TRACE, "QR " << taskSched->histTimeOfRunningTasks->getString("run"));
            } else {
                LOGS(_log, LOG_LVL_ERROR, "QR runtaskSched == nullptr");
            }
            double runTimeSeconds = primeT.getElapsed();
            double subchunkRunTimeSeconds = subChunkT.getElapsed();
            auto qStats = _task->getQueryStats();
            if (qStats != nullptr) qStats->addTaskRunQuery(runTimeSeconds, subchunkRunTimeSeconds);

            // Transition task's state to the next one (reading data from MySQL and sending them to Czar).
            _task->queried();
            // Pass all information on to the shared object to add on to
            // an existing message or build a new one as needed.
            {
                auto sendChan = _task->getSendChannel();
                if (sendChan == nullptr) {
                    throw util::Bug(ERR_LOC, "QueryRunner::_dispatchChannel() sendChan==null");
                }
                erred = sendChan->buildAndTransmitResult(res, _task, _multiError, _cancelled);
            }
        }
    } catch (sql::SqlErrorObject const& e) {
        LOGS(_log, LOG_LVL_ERROR, "dispatchChannel " << e.errMsg() << " " << _task->getIdStr());
        util::Error worker_err(e.errNo(), e.errMsg());
        _multiError.push_back(worker_err);
        erred = true;
    }

    // IMPORTANT, do not leave this function before this check has been made.
    if (needToFreeRes) {
        needToFreeRes = false;
        // All rows have been read out or there was an error. In
        // either case resources need to be freed.
        _mysqlConn->freeResult();
    }
    // Transmit errors, if needed.
    if (!_cancelled && _multiError.size() > 0) {
        LOGS(_log, LOG_LVL_WARN, "Transmitting error " << _task->getIdStr());
        erred = true;
        // Send results. This needs to happen after the error check.
        // If any errors were found, send an error back.
        _task->getSendChannel()->buildAndTransmitError(_multiError, _task, _cancelled);
    }
    return !erred;
}

void QueryRunner::cancel() {
    // QueryRunner::cancel() should only be called by Task::cancel()
    // to keep the bookkeeping straight.
    LOGS(_log, LOG_LVL_TRACE, "Trying QueryRunner::cancel() call " << _task->getIdStr());

    bool alreadyCancelled = _cancelled.exchange(true);
    if (alreadyCancelled) {
        LOGS(_log, LOG_LVL_WARN, "already cancelled" << _task->getIdStr());
        return;
    }

    if (_mysqlConn == nullptr) {
        LOGS(_log, LOG_LVL_TRACE, "QueryRunner::cancel() no MysqlConn");
    } else {
        int status = _mysqlConn->cancel();
        switch (status) {
            case -1:
                LOGS(_log, LOG_LVL_ERROR, "QueryRunner::cancel() NOP");
                break;
            case 0:
                LOGS(_log, LOG_LVL_WARN, "QueryRunner::cancel() success");
                break;
            case 1:
                LOGS(_log, LOG_LVL_ERROR, "QueryRunner::cancel() Error connecting to kill query.");
                break;
            case 2:
                LOGS(_log, LOG_LVL_ERROR, "QueryRunner::cancel() Error processing kill query.");
                break;
            default:
                LOGS(_log, LOG_LVL_ERROR, "QueryRunner::cancel() unknown error");
                break;
        }
    }
}

QueryRunner::~QueryRunner() {}

}  // namespace lsst::qserv::wdb

// Future idea: Query cache
// Pseudocode: Record query in query cache table
/*
  result = runQuery(db.get(),
  "INSERT INTO qcache.Queries "
  "(queryTime, query, db, path) "
  "VALUES (NOW(), ?, "
  "'" + dbName + "'"
  ", "
  "'" + _task->resultPath + "'"
  ")",
  script);
  if (result.size() != 0) {
  _errorNo = EIO;
  _errorDesc += result;
  return false;
  }
*/
