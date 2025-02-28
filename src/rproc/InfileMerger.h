// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015 LSST Corporation.
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
#ifndef LSST_QSERV_RPROC_INFILEMERGER_H
#define LSST_QSERV_RPROC_INFILEMERGER_H
/// InfileMerger.h declares:
///
/// class InfileMergerConfig
/// class InfileMerger
/// (see individual class documentation for more information)

// System headers
#include <memory>
#include <mutex>
#include <set>
#include <string>

// Qserv headers
#include "mysql/LocalInfile.h"
#include "mysql/MySqlConfig.h"
#include "mysql/MySqlConnection.h"
#include "sql/SqlConnection.h"
#include "util/Error.h"
#include "util/EventThread.h"
#include "util/SemaMgr.h"

// Forward declarations
namespace lsst::qserv {
namespace mysql {
class MysqlConfig;
}
namespace proto {
class ResponseData;
class ResponseSummary;
}  // namespace proto
namespace qdisp {
class JobQuery;
class UberJob;
}  // namespace qdisp
namespace QMeta {
class MessageStore;
}
namespace qproc {
class DatabaseModels;
}
namespace query {
class SelectStmt;
}
namespace sql {
class Schema;
class SqlConnection;
class SqlResults;
}  // namespace sql
}  // namespace lsst::qserv

namespace lsst::qserv::rproc {

/// class InfileMergerConfig - value class for configuring a InfileMerger
class InfileMergerConfig {
public:
    InfileMergerConfig() = delete;
    InfileMergerConfig(mysql::MySqlConfig const& mySqlConfig_) : mySqlConfig(mySqlConfig_) {}

    // for final result, and imported result
    mysql::MySqlConfig const mySqlConfig;
    std::string targetTable;
    std::shared_ptr<query::SelectStmt> mergeStmt;
    bool debugNoMerge = false;
};

/// This class is used to remove invalid rows from cancelled job attempts.
/// Removing the invalid rows from the result table can be very expensive,
/// so steps are taken to only do it when rows are known to exist in the
/// result table.
///
/// The rows can only be safely deleted from the result table when
/// nothing is writing to the table. To minimize the time locking the mutex
/// and allow multiple entities to write to the table concurrently, the
/// number of task writing to the table is tracked with _concurrentMergeCount.
/// Deletes are only to be allowed when _concurrentMergeCount is 0.
class InvalidJobAttemptMgr {
public:
    using jASetType = std::set<int>;
    using deleteFuncType = std::function<bool(jASetType const&)>;

    InvalidJobAttemptMgr() {}
    void setDeleteFunc(deleteFuncType func) { _deleteFunc = func; }

    /// @return true if jobIdAttempt is invalid.
    /// Wait if rows need to be deleted.
    /// Then, add job-attempt to _jobIdAttemptsHaveRows and increment
    /// _concurrentMergeCount to keep rows from being deleted before
    /// decrConcurrentMergeCount is called.
    bool incrConcurrentMergeCount(int jobIdAttempt);
    void decrConcurrentMergeCount();

    /// @return true if query results are valid. If it returns false, the query results are invalid.
    /// This function will stop all merging to the result table and delete all invalid
    /// rows in the table. If it returns false, invalid rows remain in the result table,
    /// and the query should probably be cancelled.
    bool holdMergingForRowDelete(std::string const& msg = "");

    /// @return true if jobIdAttempt is in the invalid set.
    bool isJobAttemptInvalid(int jobIdAttempt);

    bool prepScrub(int jobIdAttempt);

private:
    /// Precondition: must hold _iJAMtx before calling.
    /// @return true if jobIdAttempt is in the invalid set.
    bool _isJobAttemptInvalid(int jobIdAttempt);
    void _cleanupIJA();  ///< Helper to send notice to all waiting on _cv.

    std::mutex _iJAMtx;
    jASetType _invalidJobAttempts;     ///< Set of job-attempts that failed.
    jASetType _invalidJAWithRows;      ///< Set of job-attempts that failed and have rows in result table.
    jASetType _jobIdAttemptsHaveRows;  ///< Set of job-attempts that have rows in result table.
    int _concurrentMergeCount{0};
    bool _waitFlag{false};
    std::condition_variable _cv;
    deleteFuncType _deleteFunc;
};

/// InfileMerger is a row-based merger that imports rows from result messages
/// and inserts them into a MySQL table, as specified during construction by
/// InfileMergerConfig.
///
/// To use, construct a configured instance, then call merge() to kick off the
/// merging process, and finalize() to wait for outstanding merging processes
/// and perform the appropriate post-processing before returning.  merge() right
/// now expects a parsed ResponseData message.
/// At present, Result messages are not chained.
class InfileMerger {
public:
    explicit InfileMerger(InfileMergerConfig const& c, std::shared_ptr<qproc::DatabaseModels> const& dm,
                          std::shared_ptr<util::SemaMgr> const& semaMgrConn);
    InfileMerger() = delete;
    InfileMerger(InfileMerger const&) = delete;
    InfileMerger& operator=(InfileMerger const&) = delete;
    ~InfileMerger();

    enum DbEngine { MYISAM, INNODB, MEMORY };

    std::string engineToStr(InfileMerger::DbEngine engine);

    /// Merge the result data collected over Http.
    bool mergeHttp(std::shared_ptr<qdisp::UberJob> const& uberJob, proto::ResponseData const& responseData);

    /// Indicate the merge for the job is complete.
    void mergeCompleteFor(int jobId);

    /// @return error details if finalize() returns false
    util::Error const& getError() const { return _error; }
    /// @return final target table name  storing results after post processing
    std::string getTargetTable() const { return _config.targetTable; }

    /// Finalize a "merge" and perform postprocessing.
    /// `collectedBytes` is the number of bytes collected in worker results for
    ///    this user query. Its value is set by this function.
    /// `rowCount` is the number of rows in the final result, as determined by this
    ///    function. A negative value indicates that its value is meaningless,
    ///    which happens when there is no postprocessing of the result table.
    bool finalize(size_t& collectedBytes, int64_t& rowCount);
    /// Check if the object has completed all processing.
    bool isFinished() const;

    bool prepScrub(int jobId, int attempt);
    bool scrubResults(int jobId, int attempt);
    int makeJobIdAttempt(int jobId, int attemptCount);

    void setMergeStmtFromList(std::shared_ptr<query::SelectStmt> const& mergeStmt) const;

    /**
     * @brief Make a schema that matches the results of the given query.
     *
     * @param stmt The statement to make a schema for.
     * @param schema The schema object to hold the schema.
     * @return true If a schema was created.
     * @return false If a schema was NOT created. Call InfileMerger::getError() to get specific information
     *               about the failure.
     */
    bool getSchemaForQueryResults(query::SelectStmt const& stmt, sql::Schema& schema);

    /**
     * @brief Make the results table for the given query.
     *
     * Calculates the schema of the results table for the given query, and makes the results table for that
     * query.
     *
     * @param stmt The statment to make a results table for.
     * @return true If the results table was created.
     * @return false If the results table was NOT created. Call InfileMerger::getError() to get specific
     *               information about the failure.
     */
    bool makeResultsTableForQuery(query::SelectStmt const& stmt);

    int sqlConnectionAttempts() { return _maxSqlConnectionAttempts; }

    size_t getTotalResultSize() const;

private:
    bool _applyMysqlMyIsam(std::string const& query, size_t resultSize);
    bool _applyMysqlInnoDb(std::string const& query, size_t resultSize);
    void _setupRow();
    bool _applySql(std::string const& sql);
    bool _applySqlLocal(std::string const& sql, std::string const& logMsg, sql::SqlResults& results);
    bool _applySqlLocal(std::string const& sql, std::string const& logMsg);
    bool _applySqlLocal(std::string const& sql, sql::SqlResults& results);
    bool _applySqlLocal(std::string const& sql, sql::SqlResults& results, sql::SqlErrorObject& errObj);
    bool _sqlConnect(sql::SqlErrorObject& errObj);
    std::string _getQueryIdStr();
    void _setQueryIdStr(std::string const& qIdStr);
    void _fixupTargetName();

    /// Set the engine name from the string engineName. Default to MYISAM.
    void _setEngineFromStr(std::string const& engineName);

    bool _setupConnectionMyIsam() {
        if (_mysqlConn.connect()) {
            _infileMgr.attach(_mysqlConn.getMySql());
            return true;
        }
        return false;
    }

    bool _setupConnectionInnoDb(mysql::MySqlConnection& mySConn);

    InfileMergerConfig _config;                    ///< Configuration
    DbEngine _dbEngine = MYISAM;                   ///< ENGINE used for aggregating results.
    std::shared_ptr<sql::SqlConnection> _sqlConn;  ///< SQL connection
    std::string _mergeTable;                       ///< Table for result loading
    util::Error _error;                            ///< Error state
    bool _isFinished = false;                      ///< Completed?
    std::mutex _sqlMutex;                          ///< Protection for SQL connection

    /**
     * @brief Put a "jobId" column first in the provided schema.
     *
     * The jobId column is used to keep track of what job number and attempt number each row in the results
     * table came from.
     *
     * The schema must match the schema of the results returned by workers (and workers add the JobId column
     * first in the schema).
     *
     * @note This will change _jobIdColName if it conflicts with a column name in the user query.
     *
     * @param schema The schema to be modified.
     */
    void _addJobIdColumnToSchema(sql::Schema& schema);

    mysql::MySqlConnection _mysqlConn;

    std::mutex _mysqlMutex;
    mysql::LocalInfile::Mgr _infileMgr;

    std::shared_ptr<qproc::DatabaseModels> _databaseModels;  ///< Used to create result table.

    std::mutex _queryIdStrMtx;  ///< protects _queryIdStr
    std::atomic<bool> _queryIdStrSet{false};
    std::string _queryIdStr{"QID=?"};  ///< Unknown until results start coming back from workers.

    std::string _jobIdColName;                   ///< Name of the jobId column in the result table.
    int const _jobIdMysqlType{MYSQL_TYPE_LONG};  ///< 4 byte integer.
    std::string const _jobIdSqlType{"INT(9)"};   ///< The 9 only affects '0' padding with ZEROFILL.

    InvalidJobAttemptMgr _invalidJobAttemptMgr;
    bool _deleteInvalidRows(std::set<int> const& jobIdAttempts);
    int const _maxSqlConnectionAttempts =
            10;  ///< maximum number of times to retry connecting to the SQL database.

    /// Variable to track result size. Each
    size_t const _maxResultTableSizeBytes;    ///< Max result table size in bytes.
    size_t _totalResultSize = 0;              ///< Size of result so far in bytes.
    std::map<int, size_t> _perJobResultSize;  ///< Result size for each job
    std::mutex _mtxResultSizeMtx;             ///< Protects _perJobResultSize and _totalResultSize.

    std::shared_ptr<util::SemaMgr> _semaMgrConn;  ///< Used to limit the number of open mysql connections.
};

}  // namespace lsst::qserv::rproc

#endif  // LSST_QSERV_RPROC_INFILEMERGER_H
