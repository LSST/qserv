/*
 * LSST Data Management System
 * Copyright 2015-2016 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */

// Class header
#include "qmeta/QMetaMysql.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Third-party headers
#include "boost/lexical_cast.hpp"
#include <boost/algorithm/string/replace.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/stringUtil.h"
#include "qmeta/Exceptions.h"
#include "qmeta/JobStatus.h"
#include "qmeta/MessageStore.h"
#include "qmeta/QMetaTransaction.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "sql/SqlResults.h"
#include "util/TimeUtils.h"

using namespace std;

namespace {

// Current version of QMeta schema
char const VERSION_STR[] = "11";

LOG_LOGGER _log = LOG_GET("lsst.qserv.qmeta.QMetaMysql");

using lsst::qserv::qmeta::QInfo;

char const* status2string(QInfo::QStatus qStatus) {
    switch (qStatus) {
        case QInfo::EXECUTING:
            return "'EXECUTING'";
        case QInfo::COMPLETED:
            return "'COMPLETED'";
        case QInfo::FAILED:
            return "'FAILED'";
        case QInfo::ABORTED:
            return "'ABORTED'";
        default:
            return "NULL";
    }
}

QInfo::QStatus string2status(char const* statusStr) {
    if (not statusStr) {
        // we do not have enum for that (and it should not really happen),
        // just pretend we are executing
        return QInfo::EXECUTING;
    } else if (strcmp(statusStr, "EXECUTING") == 0) {
        return QInfo::EXECUTING;
    } else if (strcmp(statusStr, "COMPLETED") == 0) {
        return QInfo::COMPLETED;
    } else if (strcmp(statusStr, "FAILED") == 0) {
        return QInfo::FAILED;
    } else if (strcmp(statusStr, "ABORTED") == 0) {
        return QInfo::ABORTED;
    } else {
        // some unexpected string, say we are still executing
        return QInfo::EXECUTING;
    }
}

}  // namespace

namespace lsst::qserv::qmeta {

// Constructors
QMetaMysql::QMetaMysql(mysql::MySqlConfig const& mysqlConf, int maxMsgSourceStore)
        : QMeta(), _conn(sql::SqlConnectionFactory::make(mysqlConf)) {
    // _maxMsgSourceStore must be >= 1
    _maxMsgSourceStore = (maxMsgSourceStore < 1) ? 1 : maxMsgSourceStore;
    // Check that database is in consistent state
    _checkDb();
}

// Destructor
QMetaMysql::~QMetaMysql() {}

// Return czar ID given czar "name".
CzarId QMetaMysql::getCzarID(string const& name) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string const query = "SELECT czarId FROM QCzar WHERE czar = '" + name + "'";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // get results of the query
    vector<string> ids;
    if (not results.extractFirstColumn(ids, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract czar ID from query result");
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();

    // check number of results and convert to integer
    if (ids.empty()) {
        LOGS(_log, LOG_LVL_DEBUG, "Result set is empty");
        return 0;
    } else if (ids.size() > 1) {
        throw ConsistencyError(
                ERR_LOC, "More than one czar ID found for czar name " + name + ": " + to_string(ids.size()));
    } else {
        LOGS(_log, LOG_LVL_DEBUG, "Found czar ID: " << ids[0]);
        return boost::lexical_cast<CzarId>(ids[0]);
    }
}

// Register new czar, return czar ID.
CzarId QMetaMysql::registerCzar(string const& name) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // check if czar is already defined
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string query = "SELECT czarId FROM QCzar WHERE czar = '" + name + "'";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // get results of the query
    vector<string> ids;
    if (not results.extractFirstColumn(ids, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract czar ID from query result");
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of results and convert to integer
    CzarId czarId = 0;
    if (ids.size() > 1) {
        throw ConsistencyError(
                ERR_LOC, "More than one czar ID found for czar name " + name + ": " + to_string(ids.size()));
    } else if (ids.empty()) {
        // no such czar, make new one
        LOGS(_log, LOG_LVL_DEBUG, "Create new czar with name: " << name);
        results.freeResults();
        query = "INSERT INTO QCzar (czar, active) VALUES ('" + name + "', b'1')";
        LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
        if (not _conn->runQuery(query, results, errObj)) {
            LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
            throw SqlError(ERR_LOC, errObj);
        }
        auto newId = _conn->getInsertId();
        LOGS(_log, LOG_LVL_DEBUG, "Created czar ID: " << newId);
        czarId = static_cast<CzarId>(newId);

    } else {
        // its exists, get its ID
        czarId = boost::lexical_cast<CzarId>(ids[0]);
        LOGS(_log, LOG_LVL_DEBUG, "Use existing czar with ID: " << czarId);

        // make sure it's active
        results.freeResults();
        query = "UPDATE QCzar SET active = b'1' WHERE czarId = " + ids[0];
        LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
        if (not _conn->runQuery(query, results, errObj)) {
            LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
            throw SqlError(ERR_LOC, errObj);
        }
    }

    trans->commit();

    return czarId;
}

// Mark specified czar as active or inactive.
void QMetaMysql::setCzarActive(CzarId czarId, bool active) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string const query = "UPDATE QCzar SET active = b'" + string(active ? "1" : "0") +
                         "' WHERE czarId = " + to_string(czarId);
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of rows updated, expect exactly one
    if (results.getAffectedRows() == 0) {
        throw CzarIdError(ERR_LOC, czarId);
    } else if (results.getAffectedRows() > 1) {
        throw ConsistencyError(ERR_LOC, "More than one row updated for czar ID " + to_string(czarId) + ": " +
                                                to_string(results.getAffectedRows()));
    }

    trans->commit();
}

// Cleanup of query status.
void QMetaMysql::cleanup(CzarId czarId) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string const query =
            "UPDATE QInfo SET status = 'ABORTED', completed = NOW() "
            " WHERE czarId = " +
            to_string(czarId) + " AND status = 'EXECUTING'";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();
}

// Register new query.
QueryId QMetaMysql::registerQuery(QInfo const& qInfo, TableNames const& tables) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // build query
    sql::SqlErrorObject errObj;
    string const qType(qInfo.queryType() == QInfo::SYNC ? "'SYNC'" : "'ASYNC'");
    string const user = "'" + _conn->escapeString(qInfo.user()) + "'";
    string const queryText = "'" + _conn->escapeString(qInfo.queryText()) + "'";
    string const queryTemplate = "'" + _conn->escapeString(qInfo.queryTemplate()) + "'";
    string const resultLocation = "'" + _conn->escapeString(qInfo.resultLocation()) + "'";
    string const msgTableName = "'" + _conn->escapeString(qInfo.msgTableName()) + "'";
    string qMerge = "NULL";
    string resultQuery = "'" + _conn->escapeString(qInfo.resultQuery()) + "'";
    if (not qInfo.mergeQuery().empty()) {
        qMerge = "'" + _conn->escapeString(qInfo.mergeQuery()) + "'";
    }
    string query =
            "INSERT INTO QInfo (qType, czarId, user, query, qTemplate, qMerge, "
            "status, messageTable, resultLocation, resultQuery, chunkCount) VALUES (";

    query += qType;
    query += ", ";
    query += to_string(qInfo.czarId());
    query += ", ";
    query += user;
    query += ", ";
    query += queryText;
    query += ", ";
    query += queryTemplate;
    query += ", ";
    query += qMerge;
    query += ", 'EXECUTING',";
    query += msgTableName;
    query += ", ";
    query += resultLocation;
    query += ", ";
    query += resultQuery;
    query += ", ";
    query += to_string(qInfo.chunkCount());
    query += ")";

    // run query
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // return value of the auto-increment column
    QueryId queryId = static_cast<QueryId>(_conn->getInsertId());

    // register all tables, first remove all duplicates from a list
    TableNames uniqueTables = tables;
    auto end = unique(uniqueTables.begin(), uniqueTables.end());
    for (auto itr = uniqueTables.begin(); itr != end; ++itr) {
        query = "INSERT INTO QTable (queryId, dbName, tblName) VALUES (";
        query += to_string(queryId);
        query += ", '";
        query += _conn->escapeString(itr->first);
        query += "', '";
        query += _conn->escapeString(itr->second);
        query += "')";

        LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
        if (not _conn->runQuery(query, errObj)) {
            LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
            throw SqlError(ERR_LOC, errObj);
        }
    }

    trans->commit();
    LOGS(_log, LOG_LVL_DEBUG, "assigned to UserQuery:" << qInfo.queryText());

    return queryId;
}

// Add list of chunks to query.
void QMetaMysql::addChunks(QueryId queryId, vector<int> const& chunks) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // register all tables
    sql::SqlErrorObject errObj;
    for (vector<int>::const_iterator itr = chunks.begin(); itr != chunks.end(); ++itr) {
        string query = "INSERT INTO QWorker (queryId, chunk) VALUES (";
        query += to_string(queryId);
        query += ", ";
        query += to_string(*itr);
        query += ")";

        LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
        if (not _conn->runQuery(query, errObj)) {
            LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
            throw SqlError(ERR_LOC, errObj);
        }
    }

    trans->commit();
}

// Assign or re-assign chunk to a worker.
void QMetaMysql::assignChunk(QueryId queryId, int chunk, string const& xrdEndpoint) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // find and update chunk info
    sql::SqlErrorObject errObj;
    string query = "UPDATE QWorker SET wxrd = '";
    query += _conn->escapeString(xrdEndpoint);
    query += "', submitted = NOW() WHERE queryId = ";
    query += to_string(queryId);
    query += " AND chunk = ";
    query += to_string(chunk);

    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    sql::SqlResults results;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of rows updated, expect exactly one
    if (results.getAffectedRows() == 0) {
        throw ChunkIdError(ERR_LOC, queryId, chunk);
    } else if (results.getAffectedRows() > 1) {
        throw ConsistencyError(ERR_LOC, "More than one row updated for query/chunk ID " + to_string(queryId) +
                                                "/" + to_string(chunk) + ": " +
                                                to_string(results.getAffectedRows()));
    }

    trans->commit();
}

// Mark chunk as completed.
void QMetaMysql::finishChunk(QueryId queryId, int chunk) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // find and update query info
    sql::SqlErrorObject errObj;
    string query = "UPDATE QWorker SET completed = NOW() WHERE queryId = ";
    query += to_string(queryId);
    query += " AND chunk = ";
    query += to_string(chunk);

    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    sql::SqlResults results;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of rows updated, expect exactly one
    if (results.getAffectedRows() == 0) {
        throw ChunkIdError(ERR_LOC, queryId, chunk);
    } else if (results.getAffectedRows() > 1) {
        throw ConsistencyError(ERR_LOC, "More than one row updated for query/chunk ID " + to_string(queryId) +
                                                "/" + to_string(chunk) + ": " +
                                                to_string(results.getAffectedRows()));
    }

    trans->commit();
}

// Mark query as completed or failed.
void QMetaMysql::completeQuery(QueryId queryId, QInfo::QStatus qStatus, int64_t collectedRows,
                               size_t collectedBytes, size_t finalRows) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // find and update query info
    string query = "UPDATE QInfo SET completed = NOW(), status = ";
    query += ::status2string(qStatus);
    query += ", collectedBytes = " + to_string(collectedBytes);
    query += ", collectedRows = " + to_string(collectedRows);
    query += ", finalRows = " + to_string(finalRows);
    query += " WHERE queryId = ";
    query += to_string(queryId);

    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of rows updated, expect exactly one
    if (results.getAffectedRows() == 0) {
        throw QueryIdError(ERR_LOC, queryId);
    } else if (results.getAffectedRows() > 1) {
        throw ConsistencyError(ERR_LOC, "More than one row updated for query ID " + to_string(queryId) +
                                                ": " + to_string(results.getAffectedRows()));
    }

    trans->commit();
}

// Mark query as finished and returned to client.
void QMetaMysql::finishQuery(QueryId queryId) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // find and update chunk info
    string query = "UPDATE QInfo SET returned = NOW() WHERE queryId = ";
    query += to_string(queryId);

    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of rows updated, expect exactly one
    if (results.getAffectedRows() == 0) {
        throw QueryIdError(ERR_LOC, queryId);
    } else if (results.getAffectedRows() > 1) {
        throw ConsistencyError(ERR_LOC, "More than one row updated for query ID " + to_string(queryId) +
                                                ": " + to_string(results.getAffectedRows()));
    }

    trans->commit();
}

// Generic interface for finding queries.
vector<QueryId> QMetaMysql::findQueries(CzarId czarId, QInfo::QType qType, string const& user,
                                        vector<QInfo::QStatus> const& status, int completed, int returned) {
    lock_guard<mutex> sync(_dbMutex);

    vector<QueryId> result;

    auto trans = QMetaTransaction::create(*_conn);

    // all conditions for query
    vector<string> cond;
    if (czarId != 0) {
        cond.push_back("czarId = " + to_string(czarId));
    }
    if (qType != QInfo::ANY) {
        string const qTypeStr(qType == QInfo::SYNC ? "SYNC" : "ASYNC");
        cond.push_back("qType = '" + qTypeStr + "'");
    }
    if (not user.empty()) {
        cond.push_back("user = '" + _conn->escapeString(user) + "'");
    }
    if (not status.empty()) {
        string condStr = "status IN (";
        for (auto itr = status.begin(), end = status.end(); itr != end; ++itr) {
            if (itr != status.begin()) condStr += ", ";
            condStr += status2string(*itr);
        }
        condStr += ")";
        cond.push_back(condStr);
    }
    if (completed >= 0) {
        cond.push_back(completed ? "completed IS NOT NULL" : "completed IS NULL");
    }
    if (returned >= 0) {
        cond.push_back(returned ? "returned IS NOT NULL" : "returned IS NULL");
    }

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string query = "SELECT queryId FROM QInfo";
    for (auto itr = cond.begin(), end = cond.end(); itr != end; ++itr) {
        if (itr == cond.begin()) {
            query += " WHERE ";
        } else {
            query += " AND ";
        }
        query += *itr;
    }
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // get results of the query
    vector<string> ids;
    if (not results.extractFirstColumn(ids, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract query ID from query result");
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();

    // convert strings to numbers
    result.reserve(ids.size());
    std::transform(ids.begin(), ids.end(), std::back_inserter(result), boost::lexical_cast<QueryId, string>);

    return result;
}

// Find all pending queries.
vector<QueryId> QMetaMysql::getPendingQueries(CzarId czarId) {
    vector<QueryId> result;

    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string query = "SELECT queryId FROM QInfo WHERE czarId = ";
    query += to_string(czarId);
    query += " AND returned IS NULL";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // get results of the query
    vector<string> ids;
    if (not results.extractFirstColumn(ids, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract query ID from query result");
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();

    // convert strings to numbers
    result.reserve(ids.size());
    std::transform(ids.begin(), ids.end(), std::back_inserter(result), boost::lexical_cast<QueryId, string>);

    return result;
}

// Get full query information.
QInfo QMetaMysql::getQueryInfo(QueryId queryId) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string query =
            "SELECT qType, czarId, user, query, qTemplate, qMerge, resultQuery, status,"
            " UNIX_TIMESTAMP(submitted), UNIX_TIMESTAMP(completed), UNIX_TIMESTAMP(returned), "
            " messageTable, resultLocation, chunkCount"
            " FROM QInfo WHERE queryId = ";
    query += to_string(queryId);
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    sql::SqlResults::iterator rowIter = results.begin();
    if (rowIter == results.end()) {
        // no records found
        throw QueryIdError(ERR_LOC, queryId);
    }

    // make sure that iterator does not move until we are done with row
    sql::SqlResults::value_type const& row = *rowIter;

    // convert query result into QInfo instance
    QInfo::QType qType = QInfo::SYNC;
    if (strcmp(row[0].first, "ASYNC") == 0) qType = QInfo::ASYNC;
    CzarId czarId = boost::lexical_cast<CzarId>(row[1].first);
    string user(row[2].first);
    string rQuery(row[3].first);
    string qTemplate(row[4].first);
    string qMerge(row[5].first ? row[5].first : "");
    string resultQuery(row[6].first);
    QInfo::QStatus qStatus = ::string2status(row[7].first);
    std::time_t submitted(row[8].first ? boost::lexical_cast<std::time_t>(row[8].first) : std::time_t(0));
    std::time_t completed(row[9].first ? boost::lexical_cast<std::time_t>(row[9].first) : std::time_t(0));
    std::time_t returned(row[10].first ? boost::lexical_cast<std::time_t>(row[10].first) : std::time_t(0));
    string messageTable(row[11].first ? row[11].first : "");
    string resultLocation(row[12].first ? row[12].first : "");
    int chunkCount = boost::lexical_cast<int>(row[13].first);
    // result location may contain #QID# token to be replaced with query ID
    boost::replace_all(resultLocation, "#QID#", to_string(queryId));

    if (++rowIter != results.end()) {
        // something else found
        throw ConsistencyError(ERR_LOC, "More than one row returned for query ID " + to_string(queryId));
    }

    trans->commit();

    return QInfo(qType, czarId, user, rQuery, qTemplate, qMerge, resultLocation, messageTable, resultQuery,
                 chunkCount, qStatus, submitted, completed, returned);
}

// Get queries which use specified database.
vector<QueryId> QMetaMysql::getQueriesForDb(string const& dbName) {
    vector<QueryId> result;

    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string query = "SELECT QInfo.queryId FROM QInfo NATURAL JOIN QTable WHERE QTable.dbName = '";
    query += _conn->escapeString(dbName);
    query += "' AND QInfo.completed IS NULL";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // get results of the query
    vector<string> ids;
    if (not results.extractFirstColumn(ids, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract query ID from query result");
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();

    // convert strings to numbers
    result.reserve(ids.size());
    std::transform(ids.begin(), ids.end(), std::back_inserter(result), boost::lexical_cast<QueryId, string>);

    return result;
}

// Get queries which use specified table.
vector<QueryId> QMetaMysql::getQueriesForTable(string const& dbName, string const& tableName) {
    vector<QueryId> result;

    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // run query
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string query = "SELECT QInfo.queryId FROM QInfo NATURAL JOIN QTable WHERE QTable.dbName = '";
    query += _conn->escapeString(dbName);
    query += "' AND QTable.tblName = '";
    query += _conn->escapeString(tableName);
    query += "' AND QInfo.completed IS NULL";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // get results of the query
    vector<string> ids;
    if (not results.extractFirstColumn(ids, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract query ID from query result");
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();

    // convert strings to numbers
    result.reserve(ids.size());
    std::transform(ids.begin(), ids.end(), std::back_inserter(result), boost::lexical_cast<QueryId, string>);

    return result;
}

// Check that all necessary tables exist or create them
void QMetaMysql::_checkDb() {
    // this is only called from constructor, no locking is needed here

    vector<string> tables;
    sql::SqlErrorObject errObj;
    if (not _conn->listTables(tables, errObj)) {
        // likely failed to connect to server or database is missing
        LOGS(_log, LOG_LVL_ERROR,
             "Failed to connect to query metadata database, check that "
             "server is running and database "
                     << _conn->getActiveDbName() << " exists");
        throw SqlError(ERR_LOC, errObj);
    }

    // check that all tables are there
    char const* requiredTables[] = {"QCzar", "QInfo", "QTable", "QWorker", "QMetadata", "QStatsTmp"};
    int const nTables = sizeof requiredTables / sizeof requiredTables[0];
    for (int i = 0; i != nTables; ++i) {
        char const* const table = requiredTables[i];
        if (std::find(tables.begin(), tables.end(), table) == tables.end()) {
            LOGS(_log, LOG_LVL_ERROR, "Query metadata table is missing: " << table);
            throw MissingTableError(ERR_LOC, table);
        }
    }

    // check schema version
    sql::SqlResults results;
    string query = "SELECT value FROM QMetadata WHERE metakey = 'version'";
    if (not _conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    // expect one record, will throw if different number of records in result
    string value;
    if (not results.extractFirstValue(value, errObj)) {
        throw ConsistencyError(ERR_LOC,
                               "QMetadata table may be missing 'version' record: " + errObj.errMsg());
    }

    // compare versions
    if (value != ::VERSION_STR) {
        throw ConsistencyError(ERR_LOC, "QMeta version mismatch, expecting version " + string(::VERSION_STR) +
                                                ", database schema version is " + value);
    }
}

void QMetaMysql::saveResultQuery(QueryId queryId, string const& query) {
    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // find and update query info
    string sqlQuery = "UPDATE QInfo SET resultQuery = \"" + _conn->escapeString(query);
    sqlQuery += "\" WHERE queryId = ";
    sqlQuery += to_string(queryId);

    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << sqlQuery);
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    if (not _conn->runQuery(sqlQuery, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL query failed: " << sqlQuery);
        throw SqlError(ERR_LOC, errObj);
    }

    // check number of rows updated, expect exactly one
    if (results.getAffectedRows() == 0) {
        throw QueryIdError(ERR_LOC, queryId);
    } else if (results.getAffectedRows() > 1) {
        throw ConsistencyError(ERR_LOC, "More than one row updated for query ID " + to_string(queryId) +
                                                ": " + to_string(results.getAffectedRows()));
    }

    trans->commit();
}

void QMetaMysql::addQueryMessages(QueryId queryId, shared_ptr<MessageStore> const& msgStore) {
    int msgCount = msgStore->messageCount();
    int cancelCount = 0;
    int completeCount = 0;
    int execFailCount = 0;
    map<string, ManyMsg> msgCountMap;
    for (int i = 0; i != msgCount; ++i) {
        qmeta::QueryMessage const& qMsg = msgStore->getMessage(i);
        try {
            _addQueryMessage(queryId, qMsg, cancelCount, completeCount, execFailCount, msgCountMap);
        } catch (qmeta::SqlError const& ex) {
            LOGS(_log, LOG_LVL_ERROR, "UserQuerySelect::_qMetaUpdateMessages failed " << ex.what());
        }
    }
    // Add the total number of cancel messages received.
    if (cancelCount > 0 || execFailCount > 0) {
        qmeta::QueryMessage qm(-1, "CANCELTOTAL", 0,
                               string("{\"CANCEL_count\":") + to_string(cancelCount) +
                                       ", \"EXECFAIL_count\":" + to_string(execFailCount) +
                                       ", \"COMPLETE_count\":" + to_string(completeCount) + "}",
                               qmeta::JobStatus::getNow(), MessageSeverity::MSG_INFO);
        _addQueryMessage(queryId, qm, cancelCount, completeCount, execFailCount, msgCountMap);
    }

    for (auto const& elem : msgCountMap) {
        if (elem.second.count > _maxMsgSourceStore) {
            string source = string("MANY_") + elem.first;
            string desc = string("{\"msgSource\":") + elem.first +
                          ", \"count\":" + to_string(elem.second.count) + "}";
            qmeta::QueryMessage qm(-1, source, 0, desc, qmeta::JobStatus::getNow(), elem.second.severity);
            _addQueryMessage(queryId, qm, cancelCount, completeCount, execFailCount, msgCountMap);
        }
    }
}

QMetaChunkMap QMetaMysql::getChunkMap(chrono::time_point<chrono::system_clock> const& prevUpdateTime) {
    lock_guard<mutex> lock(_dbMutex);

    QMetaChunkMap chunkMap;

    auto trans = QMetaTransaction::create(*_conn);

    // Check if the table needs to be read. Note that the default value of
    // the previous update timestamp always forces an attempt to read the map.
    auto const updateTime = _getChunkMapUpdateTime(lock);
    LOGS(_log, LOG_LVL_INFO,
         "QMetaMysql::getChunkMap updateTime=" << util::TimeUtils::timePointToDateTimeString(updateTime));
    bool const force =
            (prevUpdateTime == chrono::time_point<chrono::system_clock>()) || (prevUpdateTime < updateTime);
    if (!force) {
        trans->commit();
        return QMetaChunkMap();
    }

    // Read the map itself

    sql::SqlErrorObject errObj;
    sql::SqlResults results;

    string const tableName = "chunkMap";
    string const query = "SELECT `worker`,`database`,`table`,`chunk`,`size` FROM `" + tableName + "`";
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (!_conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }
    vector<vector<string>> const rows = results.extractFirstNColumns(5);
    trans->commit();

    if (rows.empty()) throw EmptyTableError(ERR_LOC, tableName);
    try {
        for (auto const& row : rows) {
            string const& worker = row[0];
            string const& database = row[1];
            string const& table = row[2];
            unsigned int chunk = lsst::qserv::stoui(row[3]);
            size_t const size = stoull(row[4]);
            chunkMap.workers[worker][database][table].push_back(QMetaChunkMap::ChunkInfo{chunk, size});
            LOGS(_log, LOG_LVL_TRACE,
                 "QMetaInsrt{worker=" << worker << " dbN=" << database << " tblN=" << table
                                      << " chunk=" << chunk << " sz=" << size);
        }
        chunkMap.updateTime = updateTime;
    } catch (exception const& ex) {
        string const msg = "Failed to parse result set of query " + query + ", ex: " + string(ex.what());
        throw ConsistencyError(ERR_LOC, msg);
    }
    return chunkMap;
}

chrono::time_point<chrono::system_clock> QMetaMysql::_getChunkMapUpdateTime(lock_guard<mutex> const& lock) {
    sql::SqlErrorObject errObj;
    sql::SqlResults results;
    string const tableName = "chunkMapStatus";
    string const query = "SELECT UNIX_TIMESTAMP(`update_time`) FROM `" + tableName +
                         "` ORDER BY `update_time` DESC LIMIT 1";

    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (!_conn->runQuery(query, results, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }
    vector<string> updateTime;
    if (!results.extractFirstColumn(updateTime, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to extract result set of query " + query);
        throw SqlError(ERR_LOC, errObj);
    }
    if (updateTime.empty()) {
        throw EmptyTableError(ERR_LOC, tableName);
    } else if (updateTime.size() > 1) {
        throw ConsistencyError(ERR_LOC, "Too many rows in result set of query " + query);
    }
    try {
        LOGS(_log, LOG_LVL_TRACE, "QMetaMysql::_getChunkMapUpdateTime " << updateTime[0]);
        return chrono::time_point<chrono::system_clock>() + chrono::seconds(stol(updateTime[0]));
    } catch (exception const& ex) {
        string const msg = "Failed to parse result set of query " + query + ", ex: " + string(ex.what());
        throw ConsistencyError(ERR_LOC, msg);
    }
}

void QMetaMysql::_addQueryMessage(QueryId queryId, qmeta::QueryMessage const& qMsg, int& cancelCount,
                                  int& completeCount, int& execFailCount, map<string, ManyMsg>& msgCountMap) {
    // Don't add duplicate messages.
    if (qMsg.msgSource == "DUPLICATE") return;
    // Don't add MULTIERROR as it's all duplicates.
    if (qMsg.msgSource == "MULTIERROR") return;
    // Don't add COMPLETE messages as no one is interested.
    if (qMsg.msgSource == "COMPLETE") {
        ++completeCount;
        return;
    }
    // Dont't add individual "CANCEL" messages.
    if (qMsg.msgSource == "CANCEL") {
        ++cancelCount;
        return;
    }
    // EXECFAIL are messages that the executive has killed since something else happened.
    if (qMsg.msgSource == "EXECFAIL") {
        ++execFailCount;
        return;
    }
    auto iter = msgCountMap.find(qMsg.msgSource);
    if (iter == msgCountMap.end()) {
        msgCountMap[qMsg.msgSource] = ManyMsg(1, qMsg.severity);
    } else {
        ++(iter->second.count);
        // If there's an error message, the error logic must be triggered,
        // so the value of severity latches to MSG_ERROR.
        bool severityChangedToError = false;
        if (qMsg.severity == MSG_ERROR) {
            if (iter->second.severity == MSG_INFO) {
                severityChangedToError = true;
            }
            iter->second.severity = MSG_ERROR;
        }
        if (iter->second.count > _maxMsgSourceStore) {
            // If severityChangedToError, then this message should be added to the table,
            // since the first ERROR message of this msgSource is more important than
            // the previous INFO messages.
            if (not severityChangedToError) {
                return;
            }
        }
    }

    lock_guard<mutex> sync(_dbMutex);

    auto trans = QMetaTransaction::create(*_conn);

    // build query
    std::string severity = (qMsg.severity == MSG_INFO ? "INFO" : "ERROR");

    string query =
            "INSERT INTO QMessages (queryId, msgSource, chunkId, code, severity, message, timestamp) VALUES "
            "(";
    query += to_string(queryId);
    query += ", \"" + _conn->escapeString(qMsg.msgSource) + "\"";
    query += ", " + to_string(qMsg.chunkId);
    query += ", " + to_string(qMsg.code);
    query += ", \"" + _conn->escapeString(severity) + "\"";
    query += ", \"" + _conn->escapeString(qMsg.description) + "\"";
    query += ", " + to_string(qmeta::JobStatus::timeToInt(qMsg.timestamp));
    query += ")";
    // run query
    sql::SqlErrorObject errObj;
    LOGS(_log, LOG_LVL_DEBUG, "Executing query: " << query);
    if (not _conn->runQuery(query, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "SQL addQueryMessage query failed: " << query);
        throw SqlError(ERR_LOC, errObj);
    }

    trans->commit();
}

}  // namespace lsst::qserv::qmeta
