/*
 * LSST Data Management System
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

// Class header
#include "replica/HttpQservMonitorModule.h"

// System headers
#include <sstream>
#include <stdexcept>
#include <vector>

// Third party headers
#include "boost/lexical_cast.hpp"

// Qserv headers
#include "css/CssAccess.h"
#include "css/CssError.h"
#include "replica/DatabaseMySQL.h"
#include "replica/DatabaseMySQLTypes.h"
#include "replica/DatabaseMySQLUtils.h"
#include "global/intTypes.h"
#include "replica/Common.h"
#include "replica/Configuration.h"
#include "replica/ConfigDatabase.h"
#include "replica/DatabaseServices.h"
#include "replica/HttpExceptions.h"
#include "replica/QservMgtServices.h"
#include "replica/QservStatusJob.h"
#include "replica/ServiceProvider.h"
#include "wbase/TaskState.h"

// LSST headers
#include "partition/Chunker.h"
#include "partition/Geometry.h"
#include "lsst/sphgeom/Chunker.h"

using namespace std;
using json = nlohmann::json;
using namespace lsst::qserv;
using namespace lsst::qserv::replica;
using namespace lsst::qserv::replica::database::mysql;

namespace {

/**
 * Extract a value of field from a result set and store it
 *
 * @param context the context for error reporting
 * @param row     the current row from the result set
 * @param column  the name of a column
 * @param obj     JSON object where to store the values
 *
 * @throws invalid_argument  if the column is not present in a result set
 * or the value of the field is 'NULL'.
 */
template <typename T>
void parseFieldIntoJson(string const& context, Row const& row, string const& column, json& obj) {
    T val;
    if (!row.get(column, val)) {
        throw invalid_argument(context + " no column '" + column + "' found in the result set");
    }
    obj[column] = val;
}

/**
 * The complementary version of the above defined function which replaces
 * 'NULL' found in a field with the specified default value.
 */
template <typename T>
void parseFieldIntoJson(string const& context, Row const& row, string const& column, json& obj,
                        T const& defaultValue) {
    if (row.isNull(column)) {
        obj[column] = defaultValue;
        return;
    }
    parseFieldIntoJson<T>(context, row, column, obj);
}

/**
 * Extract rows selected from table qservMeta.QInfo into a JSON object.
 */
void extractQInfo(Connection::Ptr const& conn, json& result) {
    if (!conn->hasResult()) return;

    Row row;
    while (conn->next(row)) {
        QueryId queryId;
        if (!row.get("queryId", queryId)) continue;

        string query, status, submitted, completed;
        row.get("query", query);
        row.get("status", status);
        row.get("submitted", submitted);
        row.get("completed", completed);

        string const queryIdStr = to_string(queryId);
        result[queryIdStr]["query"] = query;
        result[queryIdStr]["status"] = status;
        result[queryIdStr]["submitted"] = submitted;
        result[queryIdStr]["completed"] = completed;
    }
}

}  // namespace

namespace lsst::qserv::replica {

void HttpQservMonitorModule::process(Controller::Ptr const& controller, string const& taskName,
                                     HttpProcessorConfig const& processorConfig,
                                     qhttp::Request::Ptr const& req, qhttp::Response::Ptr const& resp,
                                     string const& subModuleName, HttpAuthType const authType) {
    HttpQservMonitorModule module(controller, taskName, processorConfig, req, resp);
    module.execute(subModuleName, authType);
}

HttpQservMonitorModule::HttpQservMonitorModule(Controller::Ptr const& controller, string const& taskName,
                                               HttpProcessorConfig const& processorConfig,
                                               qhttp::Request::Ptr const& req,
                                               qhttp::Response::Ptr const& resp)
        : HttpModule(controller, taskName, processorConfig, req, resp) {}

json HttpQservMonitorModule::executeImpl(string const& subModuleName) {
    if (subModuleName == "WORKERS")
        return _workers();
    else if (subModuleName == "WORKER")
        return _worker();
    else if (subModuleName == "WORKER-DB")
        return _workerDb();
    else if (subModuleName == "CZAR")
        return _czar();
    else if (subModuleName == "CZAR-DB")
        return _czarDb();
    else if (subModuleName == "QUERIES-ACTIVE")
        return _activeQueries();
    else if (subModuleName == "QUERIES-ACTIVE-PROGRESS")
        return _activeQueriesProgress();
    else if (subModuleName == "QUERIES-PAST")
        return _pastQueries();
    else if (subModuleName == "QUERY")
        return _userQuery();
    else if (subModuleName == "CSS")
        return _css();
    throw invalid_argument(context() + "::" + string(__func__) + "  unsupported sub-module: '" +
                           subModuleName + "'");
}

json HttpQservMonitorModule::_workers() {
    debug(__func__);
    checkApiVersion(__func__, 19);

    unsigned int const timeoutSec = query().optionalUInt("timeout_sec", workerResponseTimeoutSec());
    bool const keepResources = query().optionalUInt("keep_resources", 0) != 0;
    wbase::TaskSelector const taskSelector = _translateTaskSelector(__func__);

    debug(__func__, "timeout_sec=" + to_string(timeoutSec));

    bool const allWorkers = true;
    string const noParentJobId;
    auto const job =
            QservStatusJob::create(timeoutSec, allWorkers, controller(), noParentJobId, taskSelector);
    job->start();
    job->wait();

    json result = json::object();
    result["status"] = json::object();

    map<string, set<int>> schedulers2chunks;
    set<int> chunks;

    QservStatus const& status = job->qservStatus();
    for (auto itr : status.workers) {
        string const& worker = itr.first;
        bool const success = itr.second;
        json const& info = success ? status.info.at(worker) : json();
        _processWorkerInfo(worker, keepResources, info, result["status"], schedulers2chunks, chunks);
    }
    result["schedulers_to_chunks"] = _schedulers2chunks2json(schedulers2chunks);
    result["chunks"] = _chunkInfo(chunks);
    return result;
}

json HttpQservMonitorModule::_worker() {
    debug(__func__);
    checkApiVersion(__func__, 19);

    auto const worker = params().at("worker");
    unsigned int const timeoutSec = query().optionalUInt("timeout_sec", workerResponseTimeoutSec());
    bool const keepResources = query().optionalUInt("keep_resources", 0) != 0;
    wbase::TaskSelector const taskSelector = _translateTaskSelector(__func__);

    debug(__func__, "worker=" + worker);
    debug(__func__, "timeout_sec=" + to_string(timeoutSec));

    string const noParentJobId;
    GetStatusQservMgtRequest::CallbackType const onFinish = nullptr;

    auto const request = controller()->serviceProvider()->qservMgtServices()->status(
            worker, noParentJobId, taskSelector, onFinish, timeoutSec);
    request->wait();

    json result = json::object();
    result["status"] = json::object();

    map<string, set<int>> schedulers2chunks;
    set<int> chunks;

    bool const success = request->extendedState() == QservMgtRequest::ExtendedState::SUCCESS;
    json const& info = success ? request->info() : json();
    _processWorkerInfo(worker, keepResources, info, result["status"], schedulers2chunks, chunks);
    result["schedulers_to_chunks"] = _schedulers2chunks2json(schedulers2chunks);
    result["chunks"] = _chunkInfo(chunks);
    return result;
}

json HttpQservMonitorModule::_workerDb() {
    debug(__func__);
    checkApiVersion(__func__, 24);

    auto const worker = params().at("worker");
    unsigned int const timeoutSec = query().optionalUInt("timeout_sec", workerResponseTimeoutSec());

    debug(__func__, "worker=" + worker);
    debug(__func__, "timeout_sec=" + to_string(timeoutSec));

    string const noParentJobId;
    GetDbStatusQservMgtRequest::CallbackType const onFinish = nullptr;

    auto const request = controller()->serviceProvider()->qservMgtServices()->databaseStatus(
            worker, noParentJobId, onFinish, timeoutSec);
    request->wait();

    if (request->extendedState() != QservMgtRequest::ExtendedState::SUCCESS) {
        string const msg = "database operation failed, error: " +
                           QservMgtRequest::state2string(request->extendedState());
        throw HttpError(__func__, msg);
    }
    json result = json::object();
    result["status"] = request->info();
    return result;
}

json HttpQservMonitorModule::_czar() {
    debug(__func__);
    checkApiVersion(__func__, 25);

    // Connect to the Czar's MySQL proxy service.
    // Execute w/o any transactions since the transcation management isn't supported
    // by Qserv Czar.
    auto const conn = Connection::open(Configuration::qservCzarProxyParams());
    QueryGenerator const g(conn);
    string const command = "query_proc_stats";
    string const query = g.call(g.QSERV_MANAGER(command));
    string response;
    conn->execute([&query, &response](auto conn) { selectSingleValue<string>(conn, query, response); });
    string err;
    if (response.empty() || (response == command)) {
        err = "no response received from Czar";
    } else {
        if (auto const status = json::parse(response); status.is_object()) {
            return json::object({{"status", status}});
        }
        err = "response received from Czar is not a valid JSON object";
    }
    throw HttpError(__func__, err + ", query: " + query);
}

json HttpQservMonitorModule::_czarDb() {
    debug(__func__);
    checkApiVersion(__func__, 24);

    // Connect to the master database. Manage the new connection via the RAII-style
    // handler to ensure the transaction is automatically rolled-back in case of exceptions.
    ConnectionHandler const h(Connection::open(Configuration::qservCzarDbParams("qservMeta")));
    bool const full = true;
    return json::object({{"status", database::mysql::processList(h.conn, full)}});
}

wbase::TaskSelector HttpQservMonitorModule::_translateTaskSelector(string const& func) const {
    wbase::TaskSelector selector;
    selector.includeTasks = query().optionalUInt("include_tasks", 0) != 0;
    selector.queryIds = query().optionalVectorUInt64("query_ids");
    string const taskStatesParam = "task_states";
    for (auto&& str : query().optionalVectorStr(taskStatesParam)) {
        try {
            auto const state = wbase::str2taskState(str);
            selector.taskStates.push_back(state);
            debug(func, "str='" + str + "', task state=" + wbase::taskState2str(state));
        } catch (exception const& ex) {
            string const msg =
                    "failed to parse query parameter '" + taskStatesParam + "', ex: " + string(ex.what());
            error(func, msg);
            throw invalid_argument(msg);
        }
    }
    selector.maxTasks = query().optionalUInt("max_tasks", 0);
    debug(func, "include_tasks=" + replica::bool2str(selector.includeTasks));
    debug(func, "queryIds.size()=" + to_string(selector.queryIds.size()));
    debug(func, "taskStates.size()=" + to_string(selector.taskStates.size()));
    debug(func, "max_tasks=" + to_string(selector.maxTasks));
    return selector;
}

void HttpQservMonitorModule::_processWorkerInfo(string const& worker, bool keepResources,
                                                json const& inWorkerInfo, json& statusRef,
                                                map<string, set<int>>& schedulers2chunks,
                                                set<int>& chunks) const {
    statusRef[worker] = json::object();
    json& workerRef = statusRef[worker];
    workerRef["success"] = inWorkerInfo.is_null() ? 0 : 1;

    if (!inWorkerInfo.is_null()) {
        workerRef["info"] = inWorkerInfo;
        json& info = workerRef["info"];
        if (!keepResources) {
            info["resources"] = json::array();
        }
        workerRef["queries"] = _getQueries(info);
        for (json const& scheduler :
             info.at("processor").at("queries").at("blend_scheduler").at("schedulers")) {
            string const scheduerName = scheduler.at("name");
            for (json const& chunk2tasks : scheduler.at("chunk_to_num_tasks")) {
                int const chunk = chunk2tasks[0];
                schedulers2chunks[scheduerName].insert(chunk);
                chunks.insert(chunk);
            }
        }
    }
}

json HttpQservMonitorModule::_schedulers2chunks2json(map<string, set<int>> const& schedulers2chunks) const {
    json result;
    for (auto itr : schedulers2chunks) {
        string const& scheduerName = itr.first;
        for (int const chunk : itr.second) {
            result[scheduerName].push_back(chunk);
        }
    }
    return result;
}

json HttpQservMonitorModule::_activeQueries() {
    debug(__func__);
    checkApiVersion(__func__, 25);

    unsigned int const timeoutSec = query().optionalUInt("timeout_sec", workerResponseTimeoutSec());
    debug(__func__, "timeout_sec=" + to_string(timeoutSec));

    // Check which queries and in which schedulers are being executed
    // by Qserv workers.
    bool const allWorkers = true;
    auto const job = QservStatusJob::create(timeoutSec, allWorkers, controller());
    job->start();
    job->wait();

    map<QueryId, string> queryId2scheduler;
    auto&& status = job->qservStatus();
    for (auto&& entry : status.workers) {
        string const& worker = entry.first;
        bool success = entry.second;
        if (success) {
            auto info = status.info.at(worker);
            auto&& schedulers = info["processor"]["queries"]["blend_scheduler"]["schedulers"];
            for (auto&& scheduler : schedulers) {
                string const scheduerName = scheduler["name"];
                for (auto&& queryId2count : scheduler["query_id_to_count"]) {
                    QueryId const queryId = queryId2count[0];
                    queryId2scheduler[queryId] = scheduerName;
                }
            }
        }
    }

    // Connect to the master database. Manage the new connection via the RAII-style
    // handler to ensure the transaction is automatically rolled-back in case of exceptions.
    ConnectionHandler const h(Connection::open(Configuration::qservCzarDbParams("qservMeta")));
    QueryGenerator const g(h.conn);

    // Get info on the ongoing queries
    json result;
    h.conn->executeInOwnTransaction(
            [&](auto conn) { result["queries"] = _currentUserQueries(conn, queryId2scheduler); });
    return result;
}

json HttpQservMonitorModule::_activeQueriesProgress() {
    debug(__func__);
    checkApiVersion(__func__, 25);

    QueryId const selectQueryId = query().optionalUInt64("query_id", 0);
    unsigned int const selectLastSeconds = query().optionalUInt("last_seconds", 0);

    debug(__func__, "query_id=" + to_string(selectQueryId));
    debug(__func__, "last_seconds=" + to_string(selectLastSeconds));

    // Connect to the Czar's MySQL proxy service.
    auto const conn = Connection::open(Configuration::qservCzarProxyParams());
    QueryGenerator const g(conn);
    string const command = "query_info " + to_string(selectQueryId) + " " + to_string(selectLastSeconds);
    string const query = g.call(g.QSERV_MANAGER(command));

    debug(__func__, "query=" + query);

    // Result set processor populates the JSON object and returns the completion
    // status of the operation as a string. The empty string indicates success.
    json queries = json::object();
    auto const extractResultSet = [&queries, this](auto conn) -> string {
        debug(__func__, "");
        // Clear the result in case if the previous retry failed mid-flight.
        queries = json::object();
        vector<string> const requiredColumnNames = {"queryId", "timestamp_ms", "num_jobs"};
        if (conn->columnNames() != requiredColumnNames) {
            return "unexpected schema of the result set";
        }
        string prevQueryIdStr;
        Row row;
        while (conn->next(row)) {
            // Default values indicate NULLs
            string const queryIdStr = row.getAs<string>(0, string());
            uint64_t const timestampMs = row.getAs<uint64_t>(1, 0);
            int const numJobs = row.getAs<int>(2, -1);
            if (queryIdStr.empty() || (timestampMs == 0) || (numJobs < 0)) {
                return "NULL values in the result set";
            }
            // Group query-specific results into dedicated arrays
            if (prevQueryIdStr.empty() || (prevQueryIdStr != queryIdStr)) {
                prevQueryIdStr = queryIdStr;
                queries[queryIdStr] = json::array();
            }
            queries[queryIdStr].push_back(json::array({timestampMs, numJobs}));
            debug(__func__, "(queryIdStr,timestampMs,numJobs)=(" + queryIdStr + "," + to_string(timestampMs) +
                                    "," + to_string(numJobs) + ")");
        }
        return string();
    };

    // Execute w/o any transactions since the transcation management isn't supported
    // by Qserv Czar. Execute the query via the automatic query retry wrapper
    string error;
    conn->execute([&query, &error, &extractResultSet](auto conn) {
        conn->execute(query);
        // if (conn->hasResult()) error = extractResultSet(conn);
        error = extractResultSet(conn);
    });
    if (error.empty()) return json::object({{"queries", queries}});
    throw HttpError(__func__, error + ", query: " + query);
}

json HttpQservMonitorModule::_pastQueries() {
    debug(__func__);
    checkApiVersion(__func__, 25);

    auto const config = controller()->serviceProvider()->config();
    string const queryStatus = query().optionalString("query_status", string());
    string const queryType = query().optionalString("query_type", string());
    unsigned int const queryAgeSec = query().optionalUInt("query_age", 0);
    unsigned int const minElapsedSec = query().optionalUInt("min_elapsed_sec", 0);
    unsigned int const limit4past = query().optionalUInt("limit4past", 1);
    string const searchPattern = query().optionalString("search_pattern", string());
    bool const searchRegexpMode = query().optionalUInt("search_regexp_mode", 0) != 0;
    bool const includeMessages = query().optionalUInt("include_messages", 0) != 0;

    debug(__func__, "query_status=" + queryStatus);
    debug(__func__, "query_type=" + queryType);
    debug(__func__, "query_age=" + to_string(queryAgeSec));
    debug(__func__, "min_elapsed_sec=" + to_string(minElapsedSec));
    debug(__func__, "limit4past=" + to_string(limit4past));
    debug(__func__, "search_pattern=" + searchPattern);
    debug(__func__, "search_regexp_mode=" + bool2str(searchRegexpMode));
    debug(__func__, "include_messages=" + bool2str(includeMessages));

    // Connect to the master database. Manage the new connection via the RAII-style
    // handler to ensure the transaction is automatically rolled-back in case of exceptions.
    ConnectionHandler const h(Connection::open(Configuration::qservCzarDbParams("qservMeta")));
    QueryGenerator const g(h.conn);

    // Get info on the past queries matching the specified criteria.
    string constraints;
    if (queryStatus.empty()) {
        g.packCond(constraints, g.neq("status", "EXECUTING"));
    } else {
        g.packCond(constraints, g.eq("status", queryStatus));
    }
    if (!queryType.empty()) {
        g.packCond(constraints, g.eq("qType", queryType));
    }
    if (queryAgeSec > 0) {
        string const cond = g.gt(g.TIMESTAMPDIFF("SECOND", "submitted", Sql::NOW), queryAgeSec);
        g.packCond(constraints, cond);
    }
    if (minElapsedSec > 0) {
        string const cond = g.gt(g.TIMESTAMPDIFF("SECOND", "submitted", "completed"), minElapsedSec);
        g.packCond(constraints, cond);
    }
    if (!searchPattern.empty()) {
        if (searchRegexpMode) {
            g.packCond(constraints, g.regexp("query", searchPattern));
        } else {
            g.packCond(constraints, g.like("query", "%" + searchPattern + "%"));
        }
    }
    h.conn->executeInOwnTransaction([&](auto conn) {
        result["queries_past"] = _pastUserQueries(conn, constraints, limit4past, includeMessages);
    });
    return result;
}

json HttpQservMonitorModule::_userQuery() {
    debug(__func__);
    checkApiVersion(__func__, 12);

    auto const queryId = stoull(params().at("id"));
    bool const includeMessages = query().optionalUInt("include_messages", 0) != 0;

    debug(__func__, "id=" + to_string(queryId));
    debug(__func__, "include_messages=" + bool2str(includeMessages));

    json result;

    // Connect to the master database
    // Manage the new connection via the RAII-style handler to ensure the transaction
    // is automatically rolled-back in case of exceptions.

    ConnectionHandler const h(Connection::open(Configuration::qservCzarDbParams("qservMeta")));
    QueryGenerator const g(h.conn);

    h.conn->executeInOwnTransaction([&](auto conn) {
        unsigned int const limit4past = 0;
        result["queries_past"] =
                _pastUserQueries(conn, g.eq("queryId", queryId), limit4past, includeMessages);
    });
    return result;
}

json HttpQservMonitorModule::_currentUserQueries(Connection::Ptr& conn,
                                                 map<QueryId, string> const& queryId2scheduler) {
    QueryGenerator const g(conn);
    string const query =
            g.select(g.id("QStatsTmp", Sql::STAR), g.as(g.UNIX_TIMESTAMP("queryBegin"), "queryBegin_sec"),
                     g.as(g.UNIX_TIMESTAMP("lastUpdate"), "lastUpdate_sec"), g.as(Sql::NOW, "samplingTime"),
                     g.as(g.UNIX_TIMESTAMP(Sql::NOW), "samplingTime_sec"), g.id("QInfo", "query")) +
            g.from("QStatsTmp", "QInfo") +
            g.where(g.eq(g.id("QStatsTmp", "queryId"), g.id("QInfo", "queryId"))) +
            g.orderBy(make_pair(g.id("QStatsTmp", "queryBegin"), "DESC"));
    conn->execute(query);

    json result = json::array();
    if (conn->hasResult()) {
        Row row;
        while (conn->next(row)) {
            json resultRow;
            ::parseFieldIntoJson<QueryId>(__func__, row, "queryId", resultRow);
            ::parseFieldIntoJson<int>(__func__, row, "totalChunks", resultRow);
            ::parseFieldIntoJson<int>(__func__, row, "completedChunks", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "queryBegin", resultRow);
            ::parseFieldIntoJson<long>(__func__, row, "queryBegin_sec", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "lastUpdate", resultRow);
            ::parseFieldIntoJson<long>(__func__, row, "lastUpdate_sec", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "samplingTime", resultRow);
            ::parseFieldIntoJson<long>(__func__, row, "samplingTime_sec", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "query", resultRow);

            // Optionally, add the name of corresponding worker scheduler
            // if the one was already known for the query.

            QueryId const queryId = resultRow["queryId"];
            auto itr = queryId2scheduler.find(queryId);
            if (itr != queryId2scheduler.end()) {
                resultRow["scheduler"] = itr->second;
            }
            result.push_back(resultRow);
        }
    }
    return result;
}

json HttpQservMonitorModule::_pastUserQueries(Connection::Ptr& conn, string const& constraint,
                                              unsigned int limit4past, bool includeMessages) {
    json result = json::array();
    QueryGenerator const g(conn);
    string const query = g.select(Sql::STAR, g.as(g.UNIX_TIMESTAMP("submitted"), "submitted_sec"),
                                  g.as(g.UNIX_TIMESTAMP("completed"), "completed_sec"),
                                  g.as(g.UNIX_TIMESTAMP("returned"), "returned_sec")) +
                         g.from("QInfo") + g.where(constraint) + g.orderBy(make_pair("submitted", "DESC")) +
                         g.limit(limit4past);

    conn->execute(query);
    if (conn->hasResult()) {
        Row row;
        while (conn->next(row)) {
            json resultRow;
            ::parseFieldIntoJson<QueryId>(__func__, row, "queryId", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "qType", resultRow);
            ::parseFieldIntoJson<int>(__func__, row, "czarId", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "user", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "query", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "qTemplate", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "qMerge", resultRow, "");
            ::parseFieldIntoJson<string>(__func__, row, "status", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "submitted", resultRow);
            ::parseFieldIntoJson<long>(__func__, row, "submitted_sec", resultRow);
            ::parseFieldIntoJson<string>(__func__, row, "completed", resultRow, "");
            ::parseFieldIntoJson<long>(__func__, row, "completed_sec", resultRow, 0);
            ::parseFieldIntoJson<string>(__func__, row, "returned", resultRow, "");
            ::parseFieldIntoJson<long>(__func__, row, "returned_sec", resultRow, 0);
            ::parseFieldIntoJson<string>(__func__, row, "messageTable", resultRow, "");
            ::parseFieldIntoJson<string>(__func__, row, "resultLocation", resultRow, "");
            ::parseFieldIntoJson<string>(__func__, row, "resultQuery", resultRow, "");
            ::parseFieldIntoJson<long>(__func__, row, "chunkCount", resultRow, 0);
            ::parseFieldIntoJson<uint64_t>(__func__, row, "collectedBytes", resultRow, 0);
            ::parseFieldIntoJson<uint64_t>(__func__, row, "collectedRows", resultRow, 0);
            ::parseFieldIntoJson<uint64_t>(__func__, row, "finalRows", resultRow, 0);
            resultRow["messages"] = json::array();
            result.push_back(resultRow);
        }
        if (includeMessages) {
            for (auto&& queryInfo : result) {
                string const query = g.select(Sql::STAR) + g.from("QMessages") +
                                     g.where(g.eq("queryId", queryInfo["queryId"].get<QueryId>())) +
                                     g.orderBy(make_pair("timestamp", "ASC"));
                conn->execute(query);
                if (conn->hasResult()) {
                    while (conn->next(row)) {
                        json resultRow;
                        ::parseFieldIntoJson<QueryId>(__func__, row, "queryId", resultRow);
                        ::parseFieldIntoJson<string>(__func__, row, "msgSource", resultRow);
                        ::parseFieldIntoJson<int>(__func__, row, "chunkId", resultRow);
                        ::parseFieldIntoJson<int>(__func__, row, "code", resultRow);
                        ::parseFieldIntoJson<string>(__func__, row, "message", resultRow);
                        ::parseFieldIntoJson<string>(__func__, row, "severity", resultRow);
                        ::parseFieldIntoJson<uint64_t>(__func__, row, "timestamp", resultRow);
                        queryInfo["messages"].push_back(resultRow);
                    }
                }
            }
        }
    }
    return result;
}

json HttpQservMonitorModule::_getQueries(json const& workerInfo) const {
    // Find identifiers of all queries in the wait queues of all schedulers
    set<QueryId> qids;
    for (json const& scheduler :
         workerInfo.at("processor").at("queries").at("blend_scheduler").at("schedulers")) {
        for (json const& entry : scheduler.at("query_id_to_count")) {
            qids.insert(entry[0].get<QueryId>());
        }
    }

    // Connect to the master database
    // Manage the new connection via the RAII-style handler to ensure the transaction
    // is automatically rolled-back in case of exceptions.

    auto const config = controller()->serviceProvider()->config();
    ConnectionHandler const h(Connection::open(Configuration::qservCzarDbParams("qservMeta")));
    QueryGenerator const g(h.conn);

    // Extract descriptions of those queries from qservMeta
    json result;
    if (!qids.empty()) {
        string const query = g.select(Sql::STAR) + g.from("QInfo") + g.where(g.in("queryId", qids));
        h.conn->executeInOwnTransaction([&](decltype(h.conn) conn) {
            conn->execute(query);
            ::extractQInfo(conn, result);
        });
    }
    return result;
}

json HttpQservMonitorModule::_css() {
    debug(__func__);
    checkApiVersion(__func__, 12);

    // Results are packed into the dictionary: family->database->table-sharedScan
    json resultSharedScan;
    auto const config = controller()->serviceProvider()->config();
    auto const cssAccess = qservCssAccess();
    for (string const& familyName : config->databaseFamilies()) {
        bool const allDatabases = true;
        for (string const& databaseName : config->databases(familyName, allDatabases)) {
            auto const database = config->databaseInfo(databaseName);
            // Do not include special tables into the report.
            vector<string> sharedScanTables;
            for (string const& tableName : database.tables()) {
                auto const table = database.findTable(tableName);
                if (table.isPartitioned && !table.isRefMatch) {
                    sharedScanTables.emplace_back(table.name);
                    // Set the empty object as the default result for each table.
                    resultSharedScan[familyName][database.name][table.name] = json::object();
                }
            }
            // Override the default values for tables for which the shared scan
            // parameters were explicitly set.
            if (cssAccess->containsDb(database.name)) {
                for (string const& tableName : sharedScanTables) {
                    if (cssAccess->containsTable(database.name, tableName)) {
                        try {
                            css::ScanTableParams const params =
                                    cssAccess->getScanTableParams(database.name, tableName);
                            resultSharedScan[familyName][database.name][tableName] =
                                    json::object({{"lockInMem", params.lockInMem ? 1 : 0},
                                                  {"scanRating", params.scanRating}});
                        } catch (css::NoSuchTable const&) {
                            // CSS key for the shared scans may not exist yet even if the table
                            // is known to CSS.
                            ;
                        }
                    }
                }
            }
        }
    }
    json result = json::object();
    result["css"]["shared_scan"] = resultSharedScan;
    return result;
}

json HttpQservMonitorModule::_chunkInfo(set<int> const& chunks) const {
    json result;
    auto const config = controller()->serviceProvider()->config();
    for (string const& familyName : config->databaseFamilies()) {
        auto&& familyInfo = config->databaseFamilyInfo(familyName);
        /*
         * TODO: both versions of the 'Chunker' class need to be used due to non-overlapping
         * functionality and the interface.  The one from the spherical geometry packages
         * provides a simple interface for validating chunk numbers, meanwhile the other
         * one allows to extract spatial parameters of chunks. This duality will be
         * addressed later after migrating package 'partition' to use geometry utilities
         * of package 'sphgeom'.
         */
        lsst::sphgeom::Chunker const sphgeomChunker(familyInfo.numStripes, familyInfo.numSubStripes);
        lsst::partition::Chunker const partitionChunker(familyInfo.overlap, familyInfo.numStripes,
                                                        familyInfo.numSubStripes);
        for (int const chunk : chunks) {
            if (sphgeomChunker.valid(chunk)) {
                json chunkGeometry;
                auto box = partitionChunker.getChunkBounds(chunk);
                chunkGeometry["lat_min"] = box.getLatMin();
                chunkGeometry["lat_max"] = box.getLatMax();
                chunkGeometry["lon_min"] = box.getLonMin();
                chunkGeometry["lon_max"] = box.getLonMax();
                result[to_string(chunk)][familyInfo.name] = chunkGeometry;
            }
        }
    }
    return result;
}

}  // namespace lsst::qserv::replica
