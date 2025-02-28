// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2019 LSST.
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
 * @brief InfileMerger implementation
 *
 * InfileMerger is a class that is responsible for the organized
 * merging of query results into a single table that can be returned
 * to the user. The current strategy loads dumped chunk result tables
 * from workers into a single table, followed by a
 * merging/aggregation query (as needed) to produce the final user
 * result table.
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "rproc/InfileMerger.h"

// System headers
#include <chrono>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <sys/time.h>
#include <thread>

// Third-party headers
#include "boost/format.hpp"
#include "boost/regex.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "global/intTypes.h"
#include "proto/worker.pb.h"
#include "qdisp/CzarStats.h"
#include "qdisp/Executive.h"
#include "qdisp/JobQuery.h"
#include "qdisp/UberJob.h"
#include "qproc/DatabaseModels.h"
#include "query/ColumnRef.h"
#include "query/SelectStmt.h"
#include "rproc/ProtoRowBuffer.h"
#include "sql/Schema.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "sql/SqlResults.h"
#include "sql/SqlErrorObject.h"
#include "sql/statement.h"
#include "util/Bug.h"
#include "util/Error.h"
#include "util/IterableFormatter.h"
#include "util/Timer.h"

using namespace std;
namespace util = lsst::qserv::util;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.rproc.InfileMerger");

/// @return a timestamp id for use in generating temporary result table names.
std::string getTimeStampId() {
    struct timeval now;
    int rc = gettimeofday(&now, nullptr);
    if (rc != 0) {
        throw util::Error(util::ErrorCode::INTERNAL, "Failed to get timestamp.");
    }
    std::ostringstream s;
    s << (now.tv_sec % 10000) << now.tv_usec;
    return s.str();
    // Use the lower digits as pseudo-unique (usec, sec % 10000)
    // Alternative (for production?) Use boost::uuid to construct ids that are
    // guaranteed to be unique.
}

const char JOB_ID_BASE_NAME[] = "jobId";
size_t const MB_SIZE_BYTES = 1024 * 1024;

/// @return Error info on the last operation with MySQL
string lastMysqlError(MYSQL* mysql) {
    return "error: " + string(mysql_error(mysql)) + ", errno: " + to_string(mysql_errno(mysql));
}

/**
 * Instances of this class are used to update statistic counter on starting
 * and finishing operations with merging results into the database.
 */
class ResultMergeTracker {
public:
    ResultMergeTracker() { lsst::qserv::qdisp::CzarStats::get()->addResultMerge(); }
    ~ResultMergeTracker() { lsst::qserv::qdisp::CzarStats::get()->deleteResultMerge(); }
};

lsst::qserv::TimeCountTracker<double>::CALLBACKFUNC const reportMergeRate =
        [](lsst::qserv::TIMEPOINT start, lsst::qserv::TIMEPOINT end, double bytes, bool success) {
            if (!success) return;
            if (chrono::duration<double> const seconds = end - start; seconds.count() > 0) {
                lsst::qserv::qdisp::CzarStats::get()->addMergeRate(bytes / seconds.count());
            }
        };

}  // anonymous namespace

namespace lsst::qserv::rproc {

////////////////////////////////////////////////////////////////////////
// InfileMerger public
////////////////////////////////////////////////////////////////////////
InfileMerger::InfileMerger(rproc::InfileMergerConfig const& c,
                           std::shared_ptr<qproc::DatabaseModels> const& dm,
                           util::SemaMgr::Ptr const& semaMgrConn)
        : _config(c),
          _mysqlConn(_config.mySqlConfig),
          _databaseModels(dm),
          _jobIdColName(JOB_ID_BASE_NAME),
          _maxSqlConnectionAttempts(cconfig::CzarConfig::instance()->getMaxSqlConnectionAttempts()),
          _maxResultTableSizeBytes(cconfig::CzarConfig::instance()->getMaxTableSizeMB() * MB_SIZE_BYTES),
          _semaMgrConn(semaMgrConn) {
    _fixupTargetName();
    _setEngineFromStr(cconfig::CzarConfig::instance()->getResultEngine());
    if (_dbEngine == MYISAM) {
        LOGS(_log, LOG_LVL_INFO, "Engine is MYISAM, serial");
        if (!_setupConnectionMyIsam()) {
            throw util::Error(util::ErrorCode::MYSQLCONNECT, "InfileMerger mysql connect failure.");
        }
    } else {
        if (_dbEngine == INNODB) {
            LOGS(_log, LOG_LVL_INFO, "Engine is INNODB, parallel, semaMgrConn=" << *_semaMgrConn);
        } else if (_dbEngine == MEMORY) {
            LOGS(_log, LOG_LVL_INFO, "Engine is MEMORY, parallel, semaMgrConn=" << *_semaMgrConn);
        } else {
            throw util::Error(util::ErrorCode::INTERNAL, "SQL engine is unknown" + std::to_string(_dbEngine));
        }
        // Shared connection not used for parallel inserts.
        _mysqlConn.closeMySqlConn();
    }

    // The DEBUG level is good here since this report will be made onces per query,
    // not per each chunk.
    LOGS(_log, LOG_LVL_DEBUG,
         "InfileMerger maxResultTableSizeBytes=" << _maxResultTableSizeBytes
                                                 << " maxSqlConnexctionAttempts=" << _maxSqlConnectionAttempts
                                                 << " debugNoMerge=" << (_config.debugNoMerge ? "1" : " 0"));
    _invalidJobAttemptMgr.setDeleteFunc([this](InvalidJobAttemptMgr::jASetType const& jobAttempts) -> bool {
        return _deleteInvalidRows(jobAttempts);
    });
}

InfileMerger::~InfileMerger() {}

void InfileMerger::_setEngineFromStr(std::string const& engineName) {
    std::string eName;
    for (auto&& c : engineName) {
        eName += toupper(c);
    }
    if (eName == "INNODB") {
        _dbEngine = INNODB;
    } else if (eName == "MEMORY") {
        _dbEngine = MEMORY;
    } else if (eName == "MYISAM") {
        _dbEngine = MYISAM;
    } else {
        LOGS(_log, LOG_LVL_ERROR, "unknown dbEngine=" << engineName << " using default MYISAM");
        _dbEngine = MYISAM;
    }
    LOGS(_log, LOG_LVL_INFO, "set engine to " << engineToStr(_dbEngine));
}

std::string InfileMerger::engineToStr(InfileMerger::DbEngine engine) {
    switch (engine) {
        case MYISAM:
            return "MYISAM";
        case INNODB:
            return "INNODB";
        case MEMORY:
            return "MEMORY";
        default:
            return "UNKNOWN";
    }
}

std::string InfileMerger::_getQueryIdStr() {
    std::lock_guard<std::mutex> lk(_queryIdStrMtx);
    return _queryIdStr;
}

void InfileMerger::_setQueryIdStr(std::string const& qIdStr) {
    std::lock_guard<std::mutex> lk(_queryIdStrMtx);
    _queryIdStr = qIdStr;
    _queryIdStrSet = true;
}

void InfileMerger::mergeCompleteFor(int jobId) {
    std::lock_guard<std::mutex> resultSzLock(_mtxResultSizeMtx);
    _totalResultSize += _perJobResultSize[jobId];  // TODO:UJ this can probably be simplified
}

bool InfileMerger::mergeHttp(qdisp::UberJob::Ptr const& uberJob, proto::ResponseData const& responseData) {
    UberJobId const uJobId = uberJob->getUjId();
    std::string queryIdJobStr = uberJob->getIdStr();
    if (!_queryIdStrSet) {
        _setQueryIdStr(QueryIdHelper::makeIdStr(uberJob->getQueryId()));
    }

    // Nothing to do if size is zero.
    if (responseData.row_size() == 0) {
        return true;
    }

    // Do nothing if the query got cancelled for any reason.
    if (uberJob->isQueryCancelled()) {
        return true;
    }
    auto executive = uberJob->getExecutive();
    if (executive == nullptr || executive->getCancelled() || executive->isRowLimitComplete()) {
        return true;
    }

    std::unique_ptr<util::SemaLock> semaLock;
    if (_dbEngine != MYISAM) {
        // needed for parallel merging with INNODB and MEMORY
        semaLock.reset(new util::SemaLock(*_semaMgrConn));
    }

    TimeCountTracker<double>::CALLBACKFUNC cbf = [](TIMEPOINT start, TIMEPOINT end, double bytes,
                                                    bool success) {
        if (!success) return;
        if (std::chrono::duration<double> const seconds = end - start; seconds.count() > 0) {
            qdisp::CzarStats::get()->addXRootDSSIRecvRate(bytes / seconds.count());
        }
    };
    auto tct = make_shared<TimeCountTracker<double>>(cbf);

    bool ret = false;
    // Add columns to rows in virtFile.
    util::Timer virtFileT;
    virtFileT.start();
    // UberJobs only get one attempt
    int resultJobId = makeJobIdAttempt(uberJob->getUjId(), 0);
    ProtoRowBuffer::Ptr pRowBuffer = std::make_shared<ProtoRowBuffer>(
            responseData, resultJobId, _jobIdColName, _jobIdSqlType, _jobIdMysqlType);
    std::string const virtFile = _infileMgr.prepareSrc(pRowBuffer);
    std::string const infileStatement = sql::formLoadInfile(_mergeTable, virtFile);
    virtFileT.stop();

    // If the job attempt is invalid, exit without adding rows.
    // It will wait here if rows need to be deleted.
    if (_invalidJobAttemptMgr.incrConcurrentMergeCount(resultJobId)) {
        return true;
    }

    size_t const resultSize = responseData.transmitsize();
    size_t tResultSize;
    {
        std::lock_guard<std::mutex> resultSzLock(_mtxResultSizeMtx);
        _perJobResultSize[uJobId] += resultSize;
        tResultSize = _totalResultSize + _perJobResultSize[uJobId];
    }
    if (tResultSize > _maxResultTableSizeBytes) {
        std::ostringstream os;
        os << queryIdJobStr << " cancelling the query, queryResult table " << _mergeTable
           << " is too large at " << tResultSize << " bytes, max allowed size is " << _maxResultTableSizeBytes
           << " bytes";
        LOGS(_log, LOG_LVL_ERROR, os.str());
        _error = util::Error(-1, os.str(), -1);
        return false;
    }

    tct->addToValue(resultSize);
    tct->setSuccess();
    tct.reset();  // stop transmit recieve timer before merging happens.

    qdisp::CzarStats::get()->addTotalBytesRecv(resultSize);
    qdisp::CzarStats::get()->addTotalRowsRecv(responseData.rowcount());

    // Stop here (if requested) after collecting stats on the amount of data collected
    // from workers.
    if (_config.debugNoMerge) {
        return true;
    }

    auto start = CLOCK::now();
    switch (_dbEngine) {
        case MYISAM:
            ret = _applyMysqlMyIsam(infileStatement, resultSize);
            break;
        case INNODB:
            [[fallthrough]];
        case MEMORY:
            ret = _applyMysqlInnoDb(infileStatement, resultSize);
            break;
        default:
            throw std::invalid_argument("InfileMerger::_dbEngine is unknown =" + engineToStr(_dbEngine));
    }
    auto end = CLOCK::now();
    auto mergeDur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    LOGS(_log, LOG_LVL_TRACE,
         "mergeDur=" << mergeDur.count() << " sema(total=" << _semaMgrConn->getTotalCount()
                     << " used=" << _semaMgrConn->getUsedCount() << ")");

    if (not ret) {
        LOGS(_log, LOG_LVL_ERROR, "InfileMerger::merge mysql applyMysql failure");
    }
    _invalidJobAttemptMgr.decrConcurrentMergeCount();

    LOGS(_log, LOG_LVL_TRACE, "virtFileT=" << virtFileT.getElapsed() << " mergeDur=" << mergeDur.count());

    return ret;
}

bool InfileMerger::_applyMysqlMyIsam(std::string const& query, size_t resultSize) {
    std::unique_lock<std::mutex> lock(_mysqlMutex);
    for (int j = 0; !_mysqlConn.connected(); ++j) {
        // should have connected during construction
        // Try reconnecting--maybe we timed out.
        if (!_setupConnectionMyIsam()) {
            if (j < sqlConnectionAttempts()) {
                lock.unlock();
                sleep(1);
                lock.lock();
            } else {
                LOGS(_log, LOG_LVL_ERROR,
                     "InfileMerger::_applyMysqlMyIsam _setupConnectionMyIsam() failed!!!");
                return false;  // Reconnection failed. This is an error.
            }
        }
    }

    // Track the operation while the control flow is staying within the function.
    ::ResultMergeTracker const resultMergeTracker;

    // This starts a timer of the result merge rate tracker. The tracker will report
    // the counter (if set) upon leaving the method.
    lsst::qserv::TimeCountTracker<double> mergeRateTracker(::reportMergeRate);

    int rc = mysql_real_query(_mysqlConn.getMySql(), query.data(), query.size());
    if (rc == 0) {
        mergeRateTracker.addToValue(resultSize);
        mergeRateTracker.setSuccess();
        return true;
    }
    LOGS(_log, LOG_LVL_ERROR,
         "InfileMerger::_applyMysqlMyIsam mysql_real_query() " + ::lastMysqlError(_mysqlConn.getMySql()));
    return false;
}

bool InfileMerger::_applyMysqlInnoDb(std::string const& query, size_t resultSize) {
    mysql::MySqlConnection mySConn(_config.mySqlConfig);
    if (!mySConn.connected()) {
        if (!_setupConnectionInnoDb(mySConn)) {
            LOGS(_log, LOG_LVL_ERROR, "InfileMerger::_applyMysqlInnoDb _setupConnectionInnoDb() failed!!!");
            return false;  // Reconnection failed. This is an error.
        }
    }

    // Track the operation while the control flow is staying within the function.
    ::ResultMergeTracker const resultMergeTracker;

    // This starts a timer of the result merge rate tracker. The tracker will report
    // the counter (if set) upon leaving the method.
    lsst::qserv::TimeCountTracker<double> mergeRateTracker(::reportMergeRate);

    int rc = mysql_real_query(mySConn.getMySql(), query.data(), query.size());
    if (rc == 0) {
        mergeRateTracker.addToValue(resultSize);
        mergeRateTracker.setSuccess();
        return true;
    }
    LOGS(_log, LOG_LVL_ERROR,
         "InfileMerger::_applyMysqlInnoDb mysql_real_query() " + ::lastMysqlError(mySConn.getMySql()));
    return false;
}

bool InfileMerger::_setupConnectionInnoDb(mysql::MySqlConnection& mySConn) {
    // Make 10 attempts to open the connection. They can fail when the
    // system is busy.
    for (int j = 0; j < sqlConnectionAttempts(); ++j) {
        if (mySConn.connect()) {
            _infileMgr.attach(mySConn.getMySql());
            return true;
        } else {
            LOGS(_log, LOG_LVL_ERROR, "_setupConnectionInnoDb failed connect attempt " << j);
            sleep(1);
        }
    }
    return false;
}

size_t InfileMerger::getTotalResultSize() const { return _totalResultSize; }

bool InfileMerger::finalize(size_t& collectedBytes, int64_t& rowCount) {
    bool finalizeOk = true;
    collectedBytes = _totalResultSize;
    // TODO: Should check for error condition before continuing.
    if (_isFinished) {
        LOGS(_log, LOG_LVL_ERROR, "InfileMerger::finalize(), but _isFinished == true");
    }
    // Delete all invalid rows in the table.
    if (not _invalidJobAttemptMgr.holdMergingForRowDelete("finalize")) {
        LOGS(_log, LOG_LVL_ERROR, " failed to remove invalid rows.");
        return false;
    }
    if (_mergeTable != _config.targetTable) {
        // Aggregation needed: Do the aggregation.
        std::string mergeSelect = _config.mergeStmt->getQueryTemplate().sqlFragment();
        // Using MyISAM as single thread writing with no need to recover from errors.
        std::string createMerge = "CREATE TABLE " + _config.targetTable + " ENGINE=MyISAM " + mergeSelect;
        LOGS(_log, LOG_LVL_TRACE, "Prepping merging w/" << *_config.mergeStmt);
        LOGS(_log, LOG_LVL_DEBUG, "Merging w/" << createMerge);
        finalizeOk = _applySqlLocal(createMerge, "createMerge");

        // Find the number of rows in the new table.
        string countRowsSql = "SELECT COUNT(*) FROM " + _config.targetTable;
        sql::SqlResults countRowsResults;
        sql::SqlErrorObject countRowsErrObj;
        bool countSuccess = _applySqlLocal(countRowsSql, countRowsResults, countRowsErrObj);
        if (countSuccess) {
            // should be 1 column, 1 row in the results
            vector<string> counts;
            if (countRowsResults.extractFirstColumn(counts, countRowsErrObj) && counts.size() > 0) {
                rowCount = std::stoll(counts[0]);
                LOGS(_log, LOG_LVL_DEBUG, "rowCount=" << rowCount << " " << countRowsSql);
            } else {
                LOGS(_log, LOG_LVL_ERROR, "Failed to extract row count result");
                rowCount = 0;  // Return 0 rows since there was a problem.
            }
        } else {
            LOGS(_log, LOG_LVL_ERROR,
                 "InfileMerger::finalize countRows query failed " << countRowsErrObj.printErrMsg());
            rowCount = 0;  // Return 0 rows since there was a problem.
        }

        // Cleanup merge table.
        sql::SqlErrorObject eObj;
        // Don't report failure on not exist
        LOGS(_log, LOG_LVL_TRACE, "Cleaning up " << _mergeTable);
#if 1  // Set to 0 when we want to retain mergeTables for debugging.
        bool cleanupOk = _sqlConn->dropTable(_mergeTable, eObj, false, _config.mySqlConfig.dbName);
#else
        bool cleanupOk = true;
#endif
        if (!cleanupOk) {
            LOGS(_log, LOG_LVL_WARN, "Failure cleaning up table " << _mergeTable);
        }
    } else {
        // Remove jobId and attemptCount information from the result table.
        // Returning a view could be faster, but is more complicated.
        std::string sqlDropCol = std::string("ALTER TABLE ") + _mergeTable + " DROP COLUMN " + _jobIdColName;
        LOGS(_log, LOG_LVL_TRACE, "Removing w/" << sqlDropCol);
        finalizeOk = _applySqlLocal(sqlDropCol, "dropCol Removing");
        rowCount = -1;  // rowCount is meaningless since there was no postprocessing.
    }
    LOGS(_log, LOG_LVL_TRACE, "Merged " << _mergeTable << " into " << _config.targetTable);
    _isFinished = true;
    return finalizeOk;
}

bool InfileMerger::isFinished() const { return _isFinished; }

bool InfileMerger::_deleteInvalidRows(InvalidJobAttemptMgr::jASetType const& jobIdAttempts) {
    // delete several rows at a time
    unsigned int maxSize = 950000;  /// default 1mb limit
    auto iter = jobIdAttempts.begin();
    auto end = jobIdAttempts.end();
    while (iter != end) {
        bool first = true;
        std::string invalidStr;
        while (invalidStr.size() < maxSize && iter != end) {
            if (!first) {
                invalidStr += ",";
            } else {
                first = false;
            }
            invalidStr += std::to_string(*iter);
            ++iter;
        }
        std::string sqlDelRows = std::string("DELETE FROM ") + _mergeTable + " WHERE " + _jobIdColName +
                                 " IN (" + invalidStr + ")";
        bool ok = _applySqlLocal(sqlDelRows, "deleteInvalidRows");
        if (!ok) {
            LOGS(_log, LOG_LVL_ERROR, "Failed to drop columns w/" << sqlDelRows);
            return false;
        }
    }
    return true;
}

int InfileMerger::makeJobIdAttempt(int jobId, int attemptCount) {
    int jobIdAttempt = jobId * MAX_JOB_ATTEMPTS;
    if (attemptCount >= MAX_JOB_ATTEMPTS) {
        std::string msg = _queryIdStr + " jobId=" + std::to_string(jobId) +
                          " Canceling query attemptCount too large at " + std::to_string(attemptCount);
        LOGS(_log, LOG_LVL_ERROR, msg);
        throw util::Bug(ERR_LOC, msg);
    }
    jobIdAttempt += attemptCount;
    return jobIdAttempt;
}

void InfileMerger::setMergeStmtFromList(std::shared_ptr<query::SelectStmt> const& mergeStmt) const {
    if (mergeStmt != nullptr) {
        mergeStmt->setFromListAsTable(_mergeTable);
    }
}

bool InfileMerger::getSchemaForQueryResults(query::SelectStmt const& stmt, sql::Schema& schema) {
    sql::SqlResults results;
    sql::SqlErrorObject getSchemaErrObj;
    std::string query = stmt.getQueryTemplate().sqlFragment();
    bool ok = _databaseModels->applySql(query, results, getSchemaErrObj);
    if (not ok) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to get schema:" << getSchemaErrObj.errMsg());
        _error = util::Error(util::ErrorCode::INTERNAL, "Failed to get schema: " + getSchemaErrObj.errMsg());
        return false;
    }
    sql::SqlErrorObject errObj;
    if (errObj.isSet()) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract schema from result: " << errObj.errMsg());
        _error = util::Error(util::ErrorCode::INTERNAL,
                             "Failed to extract schema from result: " + errObj.errMsg());
        return false;
    }
    schema = results.makeSchema(errObj);
    LOGS(_log, LOG_LVL_TRACE, "InfileMerger extracted schema: " << schema);
    return true;
}

bool InfileMerger::makeResultsTableForQuery(query::SelectStmt const& stmt) {
    sql::Schema schema;
    if (not getSchemaForQueryResults(stmt, schema)) {
        return false;
    }
    _addJobIdColumnToSchema(schema);
    std::string createStmt = sql::formCreateTable(_mergeTable, schema);
    switch (_dbEngine) {
        case MYISAM:
            createStmt += " ENGINE=MyISAM";
            break;
        case INNODB:
            createStmt += " ENGINE=InnoDB";
            break;
        case MEMORY:
            createStmt += " ENGINE=MEMORY";
            break;
        default:
            throw std::invalid_argument("InfileMerger::makeResultsTableForQuery unknown engine " +
                                        engineToStr(_dbEngine));
    }
    LOGS(_log, LOG_LVL_TRACE, "InfileMerger make results table query: " << createStmt);
    if (not _applySqlLocal(createStmt, "makeResultsTableForQuery")) {
        _error = util::Error(util::ErrorCode::CREATE_TABLE,
                             "Error creating table:" + _mergeTable + ": " + _error.getMsg());
        _isFinished = true;  // Cannot continue.
        LOGS(_log, LOG_LVL_ERROR, "InfileMerger sql error: " << _error.getMsg());
        return false;
    }
    return true;
}

void InfileMerger::_addJobIdColumnToSchema(sql::Schema& schema) {
    unsigned int attempt = 0;
    auto columnItr = schema.columns.begin();
    while (columnItr != schema.columns.end()) {
        if (columnItr->name == _jobIdColName) {
            _jobIdColName = JOB_ID_BASE_NAME + std::to_string(attempt++);
            columnItr = schema.columns.begin();  // start over
        } else {
            ++columnItr;
        }
    }
    sql::ColSchema scs;
    scs.name = _jobIdColName;
    scs.colType.mysqlType = _jobIdMysqlType;
    scs.colType.sqlType = _jobIdSqlType;
    schema.columns.insert(schema.columns.begin(), scs);
}

bool InfileMerger::prepScrub(int jobId, int attemptCount) {
    int jobIdAttempt = makeJobIdAttempt(jobId, attemptCount);
    return _invalidJobAttemptMgr.prepScrub(jobIdAttempt);
}

bool InfileMerger::_applySqlLocal(std::string const& sql, std::string const& logMsg,
                                  sql::SqlResults& results) {
    auto begin = std::chrono::system_clock::now();
    bool success = _applySqlLocal(sql, results);
    auto end = std::chrono::system_clock::now();
    LOGS(_log, LOG_LVL_TRACE,
         logMsg << " success=" << success << " microseconds="
                << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
    return success;
}

bool InfileMerger::_applySqlLocal(std::string const& sql, std::string const& logMsg) {
    sql::SqlResults results(true);  // true = throw results away immediately
    return _applySqlLocal(sql, logMsg, results);
}

bool InfileMerger::_applySqlLocal(std::string const& sql, sql::SqlResults& results) {
    sql::SqlErrorObject errObj;
    return _applySqlLocal(sql, results, errObj);
}

/// Apply a SQL query, setting the appropriate error upon failure.
bool InfileMerger::_applySqlLocal(std::string const& sql, sql::SqlResults& results,
                                  sql::SqlErrorObject& errObj) {
    std::lock_guard<std::mutex> m(_sqlMutex);

    if (not _sqlConnect(errObj)) {
        return false;
    }
    if (not _sqlConn->runQuery(sql, results, errObj)) {
        _error = util::Error(errObj.errNo(), "Error applying sql: " + errObj.printErrMsg(),
                             util::ErrorCode::MYSQLEXEC);
        LOGS(_log, LOG_LVL_ERROR, "InfileMerger error: " << _error.getMsg());
        return false;
    }
    LOGS(_log, LOG_LVL_TRACE, "InfileMerger query success: " << sql);
    return true;
}

bool InfileMerger::_sqlConnect(sql::SqlErrorObject& errObj) {
    if (_sqlConn == nullptr) {
        _sqlConn = sql::SqlConnectionFactory::make(_config.mySqlConfig);
        if (not _sqlConn->connectToDb(errObj)) {
            _error = util::Error(errObj.errNo(), "Error connecting to db: " + errObj.printErrMsg(),
                                 util::ErrorCode::MYSQLCONNECT);
            _sqlConn.reset();
            LOGS(_log, LOG_LVL_ERROR, "InfileMerger error: " << _error.getMsg());
            return false;
        }
        LOGS(_log, LOG_LVL_TRACE, "InfileMerger " << (void*)this << " connected to db");
    }
    return true;
}

/// Choose the appropriate target name, depending on whether post-processing is
/// needed on the result rows.
void InfileMerger::_fixupTargetName() {
    if (_config.targetTable.empty()) {
        assert(not _config.mySqlConfig.dbName.empty());
        _config.targetTable =
                (boost::format("%1%.result_%2%") % _config.mySqlConfig.dbName % getTimeStampId()).str();
    }

    if (_config.mergeStmt) {
        // Set merging temporary if needed.
        _mergeTable = _config.targetTable + "_m";
    } else {
        _mergeTable = _config.targetTable;
    }
}

bool InvalidJobAttemptMgr::incrConcurrentMergeCount(int jobIdAttempt) {
    std::unique_lock<std::mutex> uLock(_iJAMtx);
    if (_isJobAttemptInvalid(jobIdAttempt)) {
        LOGS(_log, LOG_LVL_DEBUG, jobIdAttempt << " invalid, not merging");
        return true;
    }
    if (_waitFlag) {
        LOGS(_log, LOG_LVL_DEBUG, "InvalidJobAttemptMgr waiting");
        /// wait for flag to clear
        _cv.wait(uLock, [this]() { return !_waitFlag; });
        // Since wait lets the mutex go, this must be checked again.
        if (_isJobAttemptInvalid(jobIdAttempt)) {
            LOGS(_log, LOG_LVL_DEBUG, jobIdAttempt << " invalid after wait, not merging");
            return true;
        }
    }
    _jobIdAttemptsHaveRows.insert(jobIdAttempt);
    ++_concurrentMergeCount;
    // No rows can be deleted until after decrConcurrentMergeCount() is called, which
    // should ensure that all rows added for this job attempt can be deleted by
    // calls to holdMergingForRowDelete() if needed.
    return false;
}

void InvalidJobAttemptMgr::decrConcurrentMergeCount() {
    std::lock_guard<std::mutex> uLock(_iJAMtx);
    --_concurrentMergeCount;
    assert(_concurrentMergeCount >= 0);
    if (_concurrentMergeCount == 0) {
        // Notify any threads waiting that no merging is occurring.
        _cv.notify_all();
    }
}

bool InvalidJobAttemptMgr::prepScrub(int jobIdAttempt) {
    std::unique_lock<std::mutex> lockJA(_iJAMtx);
    _waitFlag = true;
    _invalidJobAttempts.insert(jobIdAttempt);
    bool invalidRowsInResult = false;
    if (_jobIdAttemptsHaveRows.find(jobIdAttempt) != _jobIdAttemptsHaveRows.end()) {
        invalidRowsInResult = true;
        _invalidJAWithRows.insert(jobIdAttempt);
    }
    _cleanupIJA();
    return invalidRowsInResult;
}

void InvalidJobAttemptMgr::_cleanupIJA() {
    _waitFlag = false;
    _cv.notify_all();
}

bool InvalidJobAttemptMgr::holdMergingForRowDelete(std::string const& msg) {
    std::unique_lock<std::mutex> lockJA(_iJAMtx);
    _waitFlag = true;

    // If this jobAttempt hasn't had any rows added, no need to delete rows.
    if (_invalidJAWithRows.empty()) {
        LOGS(_log, LOG_LVL_DEBUG, msg << " should not have any invalid rows, no delete needed.");
        _cleanupIJA();
        return true;
    }

    if (_concurrentMergeCount > 0) {
        _cv.wait(lockJA, [this]() { return _concurrentMergeCount == 0; });
    }

    LOGS(_log, LOG_LVL_DEBUG, "Deleting rows for " << util::printable(_invalidJAWithRows));
    bool res = _deleteFunc(_invalidJAWithRows);
    if (res) {
        // Successful removal of all invalid rows, clear _invalidJAWithRows.
        _invalidJAWithRows.clear();
        // Table scrubbed, continue merging results.
    } else {
        LOGS(_log, LOG_LVL_ERROR,
             "holdMergingForRowDelete failed to remove rows! " << util::printable(_invalidJAWithRows));
    }
    _cleanupIJA();
    return res;
}

bool InvalidJobAttemptMgr::isJobAttemptInvalid(int jobIdAttempt) {
    // Return true if jobIdAttempt is in the invalid set.
    std::lock_guard<std::mutex> iJALock(_iJAMtx);
    return _isJobAttemptInvalid(jobIdAttempt);
}

/// Precondition, must hold _iJAMtx.
bool InvalidJobAttemptMgr::_isJobAttemptInvalid(int jobIdAttempt) {
    return _invalidJobAttempts.find(jobIdAttempt) != _invalidJobAttempts.end();
}

}  // namespace lsst::qserv::rproc
