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

// Third-party headers
#include <google/protobuf/arena.h>
#include <mysql/mysql.h>

// Class header
#include "wdb/QueryRunner.h"

// LSST headers
#include "lsst/log/Log.h"

#include "wcontrol/TransmitMgr.h"
#include "global/Bug.h"
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
#include "util/common.h"
#include "util/IterableFormatter.h"
#include "util/MultiError.h"
#include "util/StringHash.h"
#include "util/Timer.h"
#include "util/threadSafe.h"
#include "wbase/Base.h"
#include "wbase/SendChannelShared.h"
#include "wdb/ChunkResource.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wdb.QueryRunner");
}

using namespace std;

namespace lsst {
namespace qserv {
namespace wdb {

QueryRunner::Ptr QueryRunner::newQueryRunner(wbase::Task::Ptr const& task,
                                             ChunkResourceMgr::Ptr const& chunkResourceMgr,
                                             mysql::MySqlConfig const& mySqlConfig,
                                             shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr) {
    Ptr qr(new QueryRunner(task, chunkResourceMgr, mySqlConfig, sqlConnMgr)); // Private constructor.
    // Let the Task know this is its QueryRunner.
    bool cancelled = qr->_task->setTaskQueryRunner(qr);
    if (cancelled) {
        qr->_cancelled = true;
        // runQuery will return quickly if the Task has been cancelled.
    }
    return qr;
}

/// New instances need to be made with QueryRunner to ensure registration with the task
/// and correct setup of enable_shared_from_this.
QueryRunner::QueryRunner(wbase::Task::Ptr const& task,
                         ChunkResourceMgr::Ptr const& chunkResourceMgr,
                         mysql::MySqlConfig const& mySqlConfig,
                         shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr)
    : _task(task), _chunkResourceMgr(chunkResourceMgr), _mySqlConfig(mySqlConfig),
      _sqlConnMgr(sqlConnMgr) {
    int rc = mysql_thread_init();
    assert(rc == 0);
    //&&& assert(_task->msg);
}

/// Initialize the db connection
bool QueryRunner::_initConnection() {
    mysql::MySqlConfig localMySqlConfig(_mySqlConfig);
    localMySqlConfig.username = _task->user; // Override with czar-passed username.
    if (_mysqlConn != nullptr) {
        LOGS(_log, LOG_LVL_ERROR, "QueryRunner::_initConnection _mysqlConn not nullptr _mysqlConn=" << _mysqlConn.get());
    }
    _mysqlConn.reset(new mysql::MySqlConnection(localMySqlConfig));

    if (not _mysqlConn->connect()) {
        LOGS(_log, LOG_LVL_ERROR, "Unable to connect to MySQL: " << localMySqlConfig);
        util::Error error(-1, "Unable to connect to MySQL; " + localMySqlConfig.toString());
        _multiError.push_back(error);
        return false;
    }
    return true;
}

/// Override _dbName with _msg.db() if available.
void QueryRunner::_setDb() {
    if (_task->msg.has_db()) {
        _dbName = _task->msg.db();
        LOGS(_log, LOG_LVL_DEBUG, "QueryRunner overriding dbName with " << _dbName);
    }
}


util::TimerHistogram memWaitHisto("memWait Hist", {1, 5, 10, 20, 40});


bool QueryRunner::runQuery() {
    QSERV_LOGCONTEXT_QUERY_JOB(_task->getQueryId(), _task->getJobId());
    LOGS(_log, LOG_LVL_DEBUG, "QueryRunner::runQuery()");
    if (_runQueryCalled.exchange(true)) {
        LOGS(_log, LOG_LVL_ERROR, "QueryRunner::runQuery already called for task="
                << _task->getQueryId() << " job=" <<  _task->getJobId());
        throw Bug("runQuery called twice");
    }

    // Make certain our Task knows that this object is no longer in use when this function exits.
    class Release {
    public:
        Release(wbase::Task::Ptr t, wbase::TaskQueryRunner *tqr) : _t{t}, _tqr{tqr} {}
        ~Release() { _t->freeTaskQueryRunner(_tqr); }
    private:
        wbase::Task::Ptr _t;
        wbase::TaskQueryRunner *_tqr;
    };
    Release release(_task, this);

    if (_task->checkCancelled()) {
        LOGS(_log, LOG_LVL_DEBUG, "runQuery, task was cancelled before it started.");
        return false;
    }

    _czarId = _task->msg.czarid();

    // Wait for memman to finish reserving resources. This can take several seconds.
    util::Timer memTimer;
    memTimer.start();
    _task->waitForMemMan();
    memTimer.stop();
    auto logMsg = memWaitHisto.addTime(memTimer.getElapsed(), _task->getIdStr());
    LOGS(_log, LOG_LVL_DEBUG, logMsg);

    if (_task->checkCancelled()) {
        LOGS(_log, LOG_LVL_DEBUG, "runQuery, task was cancelled after locking tables.");
        return false;
    }

    _setDb();
    LOGS(_log, LOG_LVL_DEBUG,  "Exec in flight for Db=" << _dbName
        << " sqlConnMgr total=" << _sqlConnMgr->getTotalCount()
        << " conn=" << _sqlConnMgr->getSqlConnCount());
    wcontrol::SqlConnLock sqlConnLock(*_sqlConnMgr, not _task->getScanInteractive());
    bool connOk = _initConnection();
    if (!connOk) {
        // Transmit the mysql connection error to the czar, which should trigger a re-try.
        _initTransmit();
        // Put the error from _initConnection in _transmitData via _multiError.
        _buildDataMsg(0, 0);
        // Since there's an error, this will be the last transmit from this QueryRunner.
        //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch transmit err start");
        if (!_transmit(true)) {
            LOGS(_log, LOG_LVL_WARN, " Could not report error to czar as sendChannel not accepting msgs.");
        }
        //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch transmit err end");
        return false;
    }

    if (_task->msg.has_protocol()) {
        switch(_task->msg.protocol()) {
        case 2:
            return _dispatchChannel(); // Run the query and send the results back.
        case 1:
            throw UnsupportedError(_task->getIdStr() + " QueryRunner: Expected protocol > 1 in TaskMsg");
        default:
            throw UnsupportedError(_task->getIdStr() + " QueryRunner: Invalid protocol in TaskMsg");
        }
    } else {
        throw UnsupportedError(_task->getIdStr() + " QueryRunner: Expected protocol > 1 in TaskMsg");
    }
    LOGS(_log, LOG_LVL_INFO, "QueryRunner::runQuery() END"); //&&& revert to DEBUG
    return false;
}

MYSQL_RES* QueryRunner::_primeResult(string const& query) {
        bool queryOk = _mysqlConn->queryUnbuffered(query);
        if (!queryOk) {
            sql::SqlErrorObject errObj;
            errObj.setErrNo(_mysqlConn->getErrno());
            errObj.addErrMsg("primeResult error " + _mysqlConn->getError());
            throw errObj;
        }
        return _mysqlConn->getResult();
}


void QueryRunner::_fillSchema(MYSQL_RES* result) {
    // Build schema obj from result
    auto const schema = mysql::SchemaFactory::newFromResult(result);
    // Fill _schemaCols from Schema obj
    for (auto&& col:schema.columns) {
        string name = col.name;
        string sqltype = col.colType.sqlType;
        int mysqltype = col.colType.mysqlType;
        _schemaCols.emplace_back(name, sqltype, mysqltype);
    }
}


/// Transmit result data with its header.
/// If 'last' is true, this is the last message in the result set
/// and flags are set accordingly.
void QueryRunner::_buildDataMsg(unsigned int rowCount, size_t tSize) {
    QSERV_LOGCONTEXT_QUERY_JOB(_task->getQueryId(), _task->getJobId());
    LOGS(_log, LOG_LVL_INFO, "QueryRunner rowCount=" << rowCount << " tSize=" << tSize);
    assert(_transmitData != nullptr);
    assert(_transmitData->result != nullptr);

    proto::Result* result = _transmitData->result;
    result->set_rowcount(rowCount);
    result->set_transmitsize(tSize);
    result->set_attemptcount(_task->getAttemptCount());

    if (!_multiError.empty()) {
        string chunkId = to_string(_task->msg.chunkid());
        string msg = "Error(s) in result for chunk #" + chunkId + ": " + _multiError.toOneLineString();
        result->set_errormsg(msg);
        LOGS(_log, LOG_LVL_ERROR, msg);
    }
    result->SerializeToString(&(_transmitData->dataMsg));
    // Build the header for this message, but this message can't be transmitted until the
    // next header has been built and appended to _transmitData->dataMsg. That happens
    // later in SendChannelShared.
    _buildHeader();

}


bool QueryRunner::_transmit(bool lastIn) {
    if (_task->sendChannel->isDead()) {
        LOGS(_log, LOG_LVL_INFO, "aborting transmit since sendChannel is dead.");
        return false;
    }

    // Have all rows already been read, or an error?
    bool erred = _transmitData->result->has_errormsg();

    _transmitData->scanInteractive = _task->getScanInteractive();
    _transmitData->erred = erred;
    _transmitData->largeResult = _largeResult;

    int qId = _task->getQueryId();
    int jId = _task->getJobId();
    //LOGS(_log, LOG_LVL_INFO, "&&&QR::_transmit start lastIn=" << lastIn);
    bool success = _task->sendChannel->addTransmit(_cancelled, erred, lastIn, _largeResult, _transmitData, qId, jId);
    //LOGS(_log, LOG_LVL_INFO, "&&&QR::_transmit end lastIn=" << lastIn);

    // Large results get priority, but new large results should not get priority until
    // after they have started transmitting.
    _largeResult = true;
    return success;
}


void QueryRunner::_initTransmit() {
    _transmitData = wbase::TransmitData::createTransmitData(_czarId);
    _transmitData->result = _initResult();
}


proto::Result* QueryRunner::_initResult() {
    proto::Result* result = _transmitData->result;
    result->set_queryid(_task->getQueryId());
    result->set_jobid(_task->getJobId());
    _transmitData->result = result;
    result->mutable_rowschema();
    if (_task->msg.has_session()) {
        result->set_session(_task->msg.session());
    }
    // Load schema from _schemaCols
    for(auto&& col:_schemaCols) {
        proto::ColumnSchema* cs = result->mutable_rowschema()->add_columnschema();
        cs->set_name(col.colName);
        cs->set_sqltype(col.colSqlType);
        cs->set_mysqltype(col.colMysqlType);
    }
    return result;
}


void QueryRunner::_buildHeader() {
    LOGS(_log, LOG_LVL_DEBUG, "_buildHeaderThis");

    proto::ProtoHeader* header = _transmitData->header;

    // The size of the dataMsg must include space for the header for the next dataMsg.
    header->set_size(_transmitData->dataMsg.size() + proto::ProtoHeaderWrap::getProtoHeaderSize());
    // The md5 hash must not include the header for the next dataMsg.
    header->set_md5(util::StringHash::getMd5(_transmitData->dataMsg.data(), _transmitData->dataMsg.size()));
    header->set_largeresult(_largeResult);
    header->set_endnodata(false);
}


class ChunkResourceRequest {
public:
    ChunkResourceRequest(shared_ptr<ChunkResourceMgr> const& mgr,
                         proto::TaskMsg const& msg)
        // Use old-school member initializers because gcc 4.8.5
        // miscompiles the code when using brace initializers (DM-4704).
        : _mgr(mgr), _msg(msg) {}

    ChunkResource getResourceFragment(int i) {
        proto::TaskMsg_Fragment const& fragment(_msg.fragment(i));
        LOGS(_log, LOG_LVL_DEBUG, "fragment i=" << i);
        if (!fragment.has_subchunks()) {
            DbTableSet dbTbls;
            for (auto const& scanTbl : _msg.scantable()) {
                dbTbls.emplace(scanTbl.db(), scanTbl.table());
            }
            assert(_msg.has_db());
            LOGS(_log, LOG_LVL_DEBUG, "fragment a db=" << _msg.db() << ":" << _msg.chunkid()
                    << " dbTbls=" << util::printable(dbTbls));
            return _mgr->acquire(_msg.db(), _msg.chunkid(), dbTbls);
        }

        string db;
        proto::TaskMsg_Subchunk const& sc = fragment.subchunks();
        DbTableSet dbTableSet;
        for (int j=0; j < sc.dbtbl_size(); j++) {
            dbTableSet.emplace(sc.dbtbl(j).db(), sc.dbtbl(j).tbl());
        }
        IntVector subchunks(sc.id().begin(), sc.id().end());
        if (sc.has_database()) {
            db = sc.database();
        } else {
            db = _msg.db();
        }
        LOGS(_log, LOG_LVL_DEBUG, "fragment b db=" << db << ":" << _msg.chunkid()
                               << " dbTableSet" << util::printable(dbTableSet)
                               << " subChunks=" << util::printable(subchunks));
        return _mgr->acquire(db, _msg.chunkid(), dbTableSet, subchunks);

    }
private:
    shared_ptr<ChunkResourceMgr> _mgr;
    proto::TaskMsg const& _msg;
};


/// Fill one row in the Result msg from one row in MYSQL_RES*
/// If the message has gotten larger than the desired message size,
/// return false. If all rows have been read, return true.
bool QueryRunner::_fillRows(MYSQL_RES* result, int numFields, uint& rowCount, size_t& tSize) {
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        auto lengths = mysql_fetch_lengths(result);
        proto::RowBundle* rawRow = _transmitData->result->add_row();
        for(int i=0; i < numFields; ++i) {
            if (row[i]) {
                rawRow->add_column(row[i], lengths[i]);
                rawRow->add_isnull(false);
            } else {
                rawRow->add_column();
                rawRow->add_isnull(true);
            }
        }
        tSize += rawRow->ByteSizeLong();
        ++rowCount;

        unsigned int szLimit = std::min(proto::ProtoHeaderWrap::PROTOBUFFER_DESIRED_LIMIT,
                                        proto::ProtoHeaderWrap::PROTOBUFFER_HARD_LIMIT);

        // This thread may have already been removed from the pool for
        // other reasons, such as taking too long.
        if (not _removedFromThreadPool) {
            // This query has been answered by the database and the
            // scheduler for this worker should stop waiting for it.
            // leavePool() will tell the scheduler this task is finished
            // and create a new thread in the pool to replace this one.
            // This thread will wait for the czar to read all of the
            // results of the query and then die.
            auto pet = _task->getAndNullPoolEventThread();
            _removedFromThreadPool = true;
            if (pet != nullptr) {
                pet->leavePool();
            } else {
                LOGS(_log, LOG_LVL_WARN, "Result PoolEventThread was null. Probably already moved.");
            }
        }

        // Each element needs to be mysql-sanitized
        // Break the loop if the result is too big so this part can be transmitted.
        if (tSize > szLimit) {
            return false;
        }
    }
    return true;
}


bool QueryRunner::_dispatchChannel() {
    int const fragNum = _task->getQueryFragmentNum();
    proto::TaskMsg const& tMsg = _task->msg;
    bool erred = false;
    int numFields = -1;
    if (tMsg.fragment_size() < 1) {
        throw Bug("QueryRunner: No fragments to execute in TaskMsg");
    }

    unsigned int rowCount = 0;
    size_t tSize = 0;
    bool needToFreeRes = false;

    // Collect the result in _transmitData. When a reasonable amount of data has been collected,
    // or there are no more rows to collect, pass _transmitData to _sendChannel.
    try {
        _initTransmit(); // set _transmit
        ChunkResourceRequest req(_chunkResourceMgr, tMsg);
        ChunkResource cr(req.getResourceFragment(fragNum));
        // TODO: Hold onto this for longer period of time as the odds of reuse are pretty low at this scale
        //       Ideally, hold it until moving on to the next chunk. Try to clean up ChunkResource code.

        if (!_cancelled &&  !_task->sendChannel->isDead()) {
            string const& query = _task->getQueryString();
            util::Timer sqlTimer;
            sqlTimer.start();
            MYSQL_RES* res = _primeResult(query); // This runs the SQL query, throws SqlErrorObj on failure.
            needToFreeRes = true;
            _task->freeResourceMonitorLock();
            sqlTimer.stop();
            LOGS(_log, LOG_LVL_DEBUG, " fragment time=" << sqlTimer.getElapsed() << " query=" << query);
            _fillSchema(res);
            numFields = mysql_num_fields(res);
            // TODO fritzm: revisit this error strategy
            // (see pull-request for DM-216)
            // Now get rows...
            while (!_fillRows(res, numFields, rowCount, tSize)) {
                //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch loop start");
                if (tSize > proto::ProtoHeaderWrap::PROTOBUFFER_HARD_LIMIT) {
                    LOGS_ERROR("Message single row too large to send using protobuffer");
                    erred = true;
                    break;
                }
                LOGS(_log, LOG_LVL_TRACE, "Splitting message size=" << tSize << ", rowCount=" << rowCount);
                _buildDataMsg(rowCount, tSize);
                //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch transmit false start");
                if (!_transmit(false)) {
                    LOGS(_log, LOG_LVL_ERROR, "Could not transmit intermediate results.");
                    return false;
                }
                //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch transmit false end");
                rowCount = 0;
                tSize = 0;
                _initTransmit(); // reset _transmitData
                //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch loop end");
            }

        }
    } catch(sql::SqlErrorObject const& e) {
        LOGS(_log, LOG_LVL_ERROR, "dispatchChannel " << e.errMsg());
        util::Error worker_err(e.errNo(), e.errMsg());
        _multiError.push_back(worker_err);
        erred = true;
    }
    if (needToFreeRes) {
        needToFreeRes = false;
        // All rows have been read out or there was an error.
        _mysqlConn->freeResult();
    }
    if (!_cancelled) {
        //util::InstanceCount icTransmit("_dispatchChannel::Transmit&&&");
        // Send results. This needs to happen after the error check.
        _buildDataMsg(rowCount, tSize);
        //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch transmit true start");
        if (!_transmit(true)) { // All remaining rows/errors for this QueryRunner should be in this transmit.
            LOGS(_log, LOG_LVL_ERROR, "Could not transmit last results.");
            return false;
        }
        //LOGS(_log, LOG_LVL_INFO, "&&&QR::_dispatch transmit true end");
    } else {
        erred = true;
        // Set poison error, no point in sending.
        LOGS(_log, LOG_LVL_ERROR, "dispatchChannel Poisoned");
        _multiError.push_back(util::Error(-1, "Poisoned."));
        // Is more cleanup needed?
    }
    return !erred;
}

void QueryRunner::cancel() {
    LOGS(_log, LOG_LVL_WARN, "Trying QueryRunner::cancel() call");
    _cancelled = true;

    // QueryRunner::cancel() should only be called by Task::cancel()
    // to keep the booking straight.

    if (!_mysqlConn.get()) {
        LOGS(_log, LOG_LVL_WARN, "QueryRunner::cancel() no MysqlConn");
        return;
    }
    int status = _mysqlConn->cancel();
    switch (status) {
      case -1:
          LOGS(_log, LOG_LVL_ERROR, "QueryRunner::cancel() NOP");
          break;
      case 0:
          LOGS(_log, LOG_LVL_ERROR, "QueryRunner::cancel() success");
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

QueryRunner::~QueryRunner() {
}

}}} // namespace lsst::qserv::wdb

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
