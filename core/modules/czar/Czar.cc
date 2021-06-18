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
#include "czar/Czar.h"

// System headers
#include <sys/time.h>
#include <thread>

// Third-party headers
#include "boost/format.hpp"
#include "boost/lexical_cast.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/ConfigMap.h"
#include "ccontrol/UserQuerySelect.h"
#include "ccontrol/UserQueryType.h"
#include "czar/CzarErrors.h"
#include "czar/MessageTable.h"
#include "global/LogContext.h"
#include "qproc/DatabaseModels.h"
#include "rproc/InfileMerger.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "sql/SqlResults.h"
#include "util/common.h"
#include "util/IterableFormatter.h"
#include "XrdSsi/XrdSsiProvider.hh"
#include "../util/StringHelper.h"
#include "WorkerResourceLists.h" // &&& TODO:UJ


using namespace std;


extern XrdSsiProvider *XrdSsiProviderClient;


namespace {

string const createAsyncResultTmpl("CREATE TABLE IF NOT EXISTS %1% "
    "(jobId BIGINT, resultLocation VARCHAR(1024))"
    "ENGINE=MEMORY;"
    "INSERT INTO %1% (jobId, resultLocation) "
    "VALUES (%2%, '%3%')");


LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.Czar");

} // anonymous namespace

namespace lsst {
namespace qserv {
namespace czar {

Czar::Ptr Czar::_czar;

Czar::Ptr Czar::createCzar(string const& configPath, string const& czarName) {
    _czar.reset(new Czar(configPath, czarName));
    return _czar;
}

// Constructors
Czar::Czar(string const& configPath, string const& czarName)
    : _czarName(czarName), _czarConfig(configPath),
      _idCounter(), _uqFactory(), _clientToQuery(), _mutex() {

    // set id counter to milliseconds since the epoch, mod 1 year.
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const int year = 60*60*24*365;
    _idCounter = uint64_t(tv.tv_sec % year)*1000 + tv.tv_usec/1000;

    string logConfig = _czarConfig.getLogConfig();
    if (not logConfig.empty()) {
        LOG_CONFIG(logConfig);
    }

    auto databaseModels = qproc::DatabaseModels::create(_czarConfig.getCssConfigMap(),
                                                        _czarConfig.getMySqlResultConfig());

    // Need to be done first as it adds logging context for new threads
    _uqFactory.reset(new ccontrol::UserQueryFactory(_czarConfig, databaseModels, _czarName));

    int qPoolSize = _czarConfig.getQdispPoolSize();
    int maxPriority = std::max(0, _czarConfig.getQdispMaxPriority());
    string vectRunSizesStr = _czarConfig.getQdispVectRunSizes();
    vector<int> vectRunSizes = util::StringHelper::getIntVectFromStr(vectRunSizesStr, ":", 1);
    string vectMinRunningSizesStr = _czarConfig.getQdispVectMinRunningSizes();
    vector<int> vectMinRunningSizes = util::StringHelper::getIntVectFromStr(vectMinRunningSizesStr, ":", 0);
    LOGS(_log, LOG_LVL_INFO, "INFO qdisp config qPoolSize=" << qPoolSize << " maxPriority=" << maxPriority
            << " vectRunSizes=" << vectRunSizesStr << " -> " << util::prettyCharList(vectRunSizes)
            << " vectMinRunningSizes=" << vectMinRunningSizesStr << " -> " << util::prettyCharList(vectMinRunningSizes));
    _qdispPool = make_shared<qdisp::QdispPool>(qPoolSize, maxPriority, vectRunSizes, vectMinRunningSizes);
    int xrootdCBThreadsMax = _czarConfig.getXrootdCBThreadsMax();
    int xrootdCBThreadsInit = _czarConfig.getXrootdCBThreadsInit();
    LOGS(_log, LOG_LVL_INFO, "config xrootdCBThreadsMax=" << xrootdCBThreadsMax);
    LOGS(_log, LOG_LVL_INFO, "config xrootdCBThreadsInit=" << xrootdCBThreadsInit);
    XrdSsiProviderClient->SetCBThreads(xrootdCBThreadsMax, xrootdCBThreadsInit);
    int const xrootdSpread = _czarConfig.getXrootdSpread();
    LOGS(_log, LOG_LVL_INFO, "config xrootdSpread=" << xrootdSpread);
    XrdSsiProviderClient->SetSpread(xrootdSpread);

    LOGS(_log, LOG_LVL_INFO, "Creating czar instance with name " << czarName);
    LOGS(_log, LOG_LVL_INFO, "Czar config: " << _czarConfig);
}

SubmitResult
Czar::submitQuery(string const& query,
                  map<string, string> const& hints) {

    LOGS(_log, LOG_LVL_DEBUG, "New query: " << query
         << ", hints: " << util::printable(hints));

    // Most of the time, this should do nothing.
    removeOldResultTables();

    util::ConfigStore hintsConfigStore(hints);

    // Analyze query hints
    string clientId = hintsConfigStore.get("client_dst_name");

    // Not being able to get thread id is not fatal,
    // it just means query cannot be associate with particular
    // client/thread and will not be able to be killed later
    int threadId = hintsConfigStore.getInt("server_thread_id", -1);

    string defaultDb = hintsConfigStore.get("db");
    LOGS(_log, LOG_LVL_DEBUG, "Default database is \"" << defaultDb <<"\"");

    // make message table name
    string userQueryId = to_string(_idCounter++);
    LOGS(_log, LOG_LVL_DEBUG, "userQueryId: " << userQueryId);
    string resultDb = _czarConfig.getMySqlResultConfig().dbName;
    string const msgTableName = "message_" + userQueryId;
    string const lockName = resultDb + "." + msgTableName;

    // Add logging context with user query ID
    LOG_MDC_SCOPE("TID", userQueryId);

    SubmitResult result;

    // instantiate message table manager
    MessageTable msgTable(lockName, _czarConfig.getMySqlResultConfig());
    try {
        msgTable.lock();
    } catch (std::exception const& exc) {
        result.errorMessage = exc.what();
        return result;
    }

    // make new UserQuery
    // this is atomic
    ccontrol::UserQuery::Ptr uq;
    {
        lock_guard<mutex> lock(_mutex);
        uq = _uqFactory->newUserQuery(query, defaultDb, getQdispPool(), userQueryId, msgTableName, resultDb);
    }

    // Add logging context with query ID
    QSERV_LOGCONTEXT_QUERY(uq->getQueryId());
    // Generate a log message with the QueryId and the full user query so that problems in the log
    // can be traced back to the source query without accessing the database.
    LOGS(_log, LOG_LVL_WARN, "New query:" << query << ", hints:" << util::printable(hints)
                          << " defaultDb:" << defaultDb << " message_table:" << msgTableName);

    // check for errors
    auto error = uq->getError();
    if (not error.empty()) {
        result.errorMessage = uq->getQueryIdString() + " Failed to instantiate query: " + error;
        return result;
    }

    auto resultQuery = uq->getResultQuery();

    // spawn background thread to wait until query finishes to unlock,
    // note that lambda stores copies of uq and msgTable.
    auto finalizer = [uq, msgTable]() mutable {
        // Add logging context with query ID
        QSERV_LOGCONTEXT_QUERY(uq->getQueryId());
        LOGS(_log, LOG_LVL_DEBUG, "submitting new query");
        uq->submit();
        uq->join();
        try {
            msgTable.unlock(uq);
            if (uq) uq->discard();
        } catch (std::exception const& exc) {
            // TODO? if this fails there is no way to notify client, and client
            // will likely hang because table may still be locked.
            LOGS(_log, LOG_LVL_ERROR, "Query finalization failed (client likely hangs): " << exc.what());
        }
    };
    LOGS(_log, LOG_LVL_DEBUG, "starting finalizer thread for query");
    thread finalThread(finalizer);
    finalThread.detach();

    // update/cleanup query map
    _updateQueryHistory(clientId, threadId, uq);

    // return all info to caller
    if (uq->isAsync()) {
        // make separate message and result tables to return info about ASYNC query,
        // we do not need to lock message because result is ready before we return
        string const resultTableName = resultDb + ".result_async_" + userQueryId;
        string const asyncLockName = resultDb + ".message_async_" + userQueryId;
        MessageTable msgTable(asyncLockName, _czarConfig.getMySqlResultConfig());
        try {
            _makeAsyncResult(resultTableName, uq->getQueryId(), uq->getResultLocation());
            msgTable.create();
        } catch (std::exception const& exc) {
            result.errorMessage = exc.what();
            return result;
        }

        result.resultTable = resultTableName;
        result.messageTable = asyncLockName;
        if (not resultTableName.empty()) {
            // respond with info about the results table.
            result.resultQuery = "SELECT * FROM " + resultTableName;
        }
    } else {
        result.messageTable = lockName;
        if (not resultQuery.empty()) {
            result.resultTable = resultDb + "." + uq->getResultTableName();
            result.resultQuery = resultQuery;
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, "returning result to proxy: resultTable="
         << result.resultTable << " messageTable=" << result.messageTable
         << " resultQuery=" << result.resultQuery);

    return result;
}

void
Czar::killQuery(string const& query, string const& clientId) {

    LOGS(_log, LOG_LVL_INFO, "KILL query: " << query << ", clientId: " << clientId);

    // the query can be one of:
    //   "KILL QUERY NNN" - kills currently running query in thread NNN
    //   "KILL CONNECTION NNN" - kills connection associated with thread NNN
    //                           and all queries in that connection
    //   "KILL NNN" - same as "KILL CONNECTION NNN"
    //   "CANCEL NNN" - kill query with ID=NNN


    // Clean query maps from expired entries
    _cleanupQueryHistory();

    ccontrol::UserQuery::Ptr uq;
    int threadId;
    QueryId queryId;
    if (ccontrol::UserQueryType::isKill(query, threadId)) {
        LOGS(_log, LOG_LVL_DEBUG, "thread ID: " << threadId);
        lock_guard<mutex> lock(_mutex);

        // find it in the client map based in client/thread id
        ClientThreadId ctId(clientId, threadId);
        auto iter = _clientToQuery.find(ctId);
        if (iter == _clientToQuery.end()) {
            LOGS(_log, LOG_LVL_INFO, "Cannot find client thread id: " << threadId);
            throw std::runtime_error("Unknown thread ID: " + query);
        }
        uq = iter->second.lock();
    } else if (ccontrol::UserQueryType::isCancel(query, queryId)) {
        LOGS(_log, LOG_LVL_DEBUG, "query ID: " << queryId);
        lock_guard<mutex> lock(_mutex);

        // find it in the client map based in client/thread id
        auto iter = _idToQuery.find(queryId);
        if (iter == _idToQuery.end()) {
            LOGS(_log, LOG_LVL_INFO, "Cannot find query id: " << queryId);
            throw std::runtime_error("Unknown or finished query ID: " + query);
        }
        uq = iter->second.lock();
    } else {
        throw std::runtime_error("Failed to parse query: " + query);
    }

    // assume this cannot fail or throw
    if (uq) {
        LOGS(_log, LOG_LVL_DEBUG, "Killing query: " << uq->getQueryId());
        // query killing can potentially take very long and we do now want to block
        // proxy from serving other requests so run it in a detached thread
        thread killThread([uq]() {
            uq->kill();
            LOGS(_log, LOG_LVL_DEBUG, "Finished killing query: " << uq->getQueryId());
        });
        killThread.detach();
    } else {
        LOGS(_log, LOG_LVL_DEBUG, "Query has expired/finished: " << query);
        throw std::runtime_error("Query has already finished: " + query);
    }
}

void
Czar::_cleanupQueryHistoryLocked() {
    // _mutex must be locked

    // first cleanup client query maps from completed queries
    for (auto iter = _clientToQuery.begin(); iter != _clientToQuery.end(); ) {
        if (iter->second.expired()) {
            iter = _clientToQuery.erase(iter);
        } else {
            ++ iter;
        }
    }
    for (auto iter = _idToQuery.begin(); iter != _idToQuery.end(); ) {
        if (iter->second.expired()) {
            iter = _idToQuery.erase(iter);
        } else {
            ++ iter;
        }
    }
}

void
Czar::_cleanupQueryHistory() {
    lock_guard<mutex> lock(_mutex);
    _cleanupQueryHistoryLocked();
}

void
Czar::_updateQueryHistory(string const& clientId,
                          int threadId,
                          ccontrol::UserQuery::Ptr const& uq) {

    lock_guard<mutex> lock(_mutex);

    // first cleanup client query maps from completed queries
    _cleanupQueryHistoryLocked();

    // remember query (weak pointer) in case we want to kill query
    if (uq->getQueryId() != QueryId(0)) {
        _idToQuery.insert(make_pair(uq->getQueryId(), uq));
        LOGS(_log, LOG_LVL_DEBUG, "Remembering query ID: "
             << uq->getQueryId() << " (new map size: "
             << _idToQuery.size() << ")");
    }
    if (not clientId.empty() and threadId >= 0) {
        ClientThreadId ctId(clientId, threadId);
        _clientToQuery.insert(make_pair(ctId, uq));
        LOGS(_log, LOG_LVL_DEBUG, "Remembering query: ("
             << clientId << ", " << threadId << ") (new map size: "
             << _clientToQuery.size() << ")");
    }
}

void
Czar::_makeAsyncResult(string const& asyncResultTable,
                       QueryId queryId,
                       string const& resultLoc) {

    auto sqlConn = sql::SqlConnectionFactory::make(_czarConfig.getMySqlResultConfig());
    LOGS(_log, LOG_LVL_DEBUG, "creating async result table " << asyncResultTable);

    sql::SqlErrorObject sqlErr;
    string resultLocEscaped;
    if (not sqlConn->escapeString(resultLoc, resultLocEscaped,sqlErr)) {
        SqlError exc(ERR_LOC, "Failure in escapString", sqlErr);
        LOGS(_log, LOG_LVL_ERROR, exc.message());
        throw exc;
    }

    string query = (boost::format(::createAsyncResultTmpl)
                    % asyncResultTable % queryId % resultLocEscaped).str();

    if (not sqlConn->runQuery(query, sqlErr)) {
        SqlError exc(ERR_LOC, "Failure creating async result table", sqlErr);
        LOGS(_log, LOG_LVL_ERROR, exc.message());
        throw exc;
    }
}


void Czar::removeOldResultTables() {
    // This only needs to run occasionally.
    lock_guard<mutex> lockOldTblDel(_lastRemovedMtx);
    _lastRemovedTimer.stop();
    double oneDaySec = 60.0 * 60.0 * 24.0; // seconds in one hour
    if (_lastRemovedTimer.getElapsed() < oneDaySec || _removingOldTables) {
        return;
    }
    _lastRemovedTimer.start();
    _removingOldTables = true;
    // Run in a separate thread in the off chance this takes a while.
    thread t([this](){
        LOGS(_log, LOG_LVL_INFO, "Removing old result database tables.");
        auto sqlConn = sql::SqlConnectionFactory::make(_czarConfig.getMySqlResultConfig());
        string dbName = _czarConfig.getMySqlResultConfig().dbName;
        string dStr = to_string(_czarConfig.getOldestResultKeptDays());

        // Find result related tables that haven't been updated in a long time.
        string sql = "SELECT table_name FROM information_schema.tables "
                "WHERE table_schema = '" + dbName + "' AND engine IS NOT NULL  "
                "AND ((update_time < (now() - INTERVAL " + dStr + " DAY)) "
                "OR (update_time IS NULL "
                     "AND create_time < (now() - INTERVAL "+ dStr +" DAY)))";
        sql::SqlResults results;
        sql::SqlErrorObject err;
        if (!sqlConn->runQuery(sql, results, err)) {
            LOGS(_log, LOG_LVL_ERROR, "Query to find old result tables failed err=" << err.printErrMsg()
                    << " sql=" << sql);
        }
        vector<string> oldTables;
        results.extractFirstColumn(oldTables, err);
        for (auto iter=oldTables.begin(), end=oldTables.end(); iter != end;) { // iter increment in loop
            // Delete in blocks of 30 to save time.
            string dropTbl = "";
            int count = 0;
            while (iter != end && count < 30) {
                string tbl = *iter;
                ++iter;
                dropTbl += "DROP TABLE " + dbName + "." + tbl + ";";
                ++count;
            }
            if (count > 0) {
                LOGS(_log, LOG_LVL_DEBUG, "trying:" << dropTbl);
                if (!sqlConn->runQuery(dropTbl, err)) {
                    LOGS(_log, LOG_LVL_ERROR, "Could not delete old tables err=" << err.printErrMsg()
                            << " sql=" << dropTbl);
                }
            }
        }
        _removingOldTables = false;
    });
    t.detach();
    _oldTableRemovalThread = std::move(t);
}


bool Czar::setupWorkerResources(std::string const& fName) {
    _workerResourceLists = std::make_shared<WorkerResourceLists>();
    return _workerResourceLists->readIn(fName);
}


}}} // namespace lsst::qserv::czar
