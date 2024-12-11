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
#include "replica/worker/WorkerSqlHttpRequest.h"

// System headers
#include <stdexcept>
#include <utility>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/mysql/DatabaseMySQLUtils.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/Performance.h"
#include "replica/util/Mutex.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using json = nlohmann::json;

#define CONTEXT context("WorkerSqlHttpRequest", __func__)

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerSqlHttpRequest");

}  // namespace

namespace lsst::qserv::replica {

using namespace database::mysql;

shared_ptr<WorkerSqlHttpRequest> WorkerSqlHttpRequest::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired) {
    auto ptr = shared_ptr<WorkerSqlHttpRequest>(
            new WorkerSqlHttpRequest(serviceProvider, worker, hdr, req, onExpired));
    ptr->init();
    return ptr;
}

WorkerSqlHttpRequest::WorkerSqlHttpRequest(shared_ptr<ServiceProvider> const& serviceProvider,
                                           string const& worker, protocol::QueuedRequestHdr const& hdr,
                                           json const& req, ExpirationCallbackType const& onExpired)
        : WorkerHttpRequest(serviceProvider, worker,
                            "SQL:" + protocol::toString(protocol::SqlRequestType(req.at("type"))), hdr, req,
                            onExpired),
          _sqlRequestType(req.at("type")),
          _user(req.at("user")),
          _password(req.at("password")),
          _databaseInfo(serviceProvider->config()->databaseInfo(req.at("database"))),
          _maxRows(req.at("max_rows")),
          _batchMode(req.at("batch_mode")),
          _resultSets(json::array()) {
    // Parse the request-specific parameters.
    switch (_sqlRequestType) {
        case protocol::SqlRequestType::QUERY:
            _query = req.at("query");
            break;
        case protocol::SqlRequestType::CREATE_TABLE:
            if (!_batchMode) _table = req.at("table");
            _engine = req.at("engine");
            _columns = replica::parseSqlColumns(req.at("columns"));
            _partitionByColumn = req.at("partition_by_column");
            break;
        case protocol::SqlRequestType::CREATE_TABLE_INDEX:
            if (!_batchMode) _table = req.at("table");
            _index = SqlIndexDef(req.at("index"));
            break;
        case protocol::SqlRequestType::DROP_TABLE_PARTITION:
            if (!_batchMode) _table = req.at("table");
            _transactionId = req.at("transaction_id");
            break;
        case protocol::SqlRequestType::DROP_TABLE_INDEX:
            if (!_batchMode) _table = req.at("table");
            _indexName = req.at("index_name");
            break;
        case protocol::SqlRequestType::ALTER_TABLE:
            if (!_batchMode) _table = req.at("table");
            _alterTableSpec = req.at("alter_spec");
            break;
        default:
            break;
    }
}

void WorkerSqlHttpRequest::getResult(json& result) const {
    // No locking is needed here since the method is called only after
    // the request is completed.
    result["result_sets"] = _resultSets;
}

bool WorkerSqlHttpRequest::execute() {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT);

    replica::Lock lock(_mtx, CONTEXT);
    checkIfCancelling(lock, __func__);

    try {
        // Pre-create the default result-set message before any operations with
        // the database service. This is needed to report errors in method _reportFailure.
        json& resultSet = _currentResultSet(lock, true);

        // Open the connection once and then manage transactions via
        // the connection handlers down below to ensure no lingering transactions
        // are left after the completion of the request's execution (whether it's
        // successful or not).
        auto const connection = _connector();

        // Check if this is the "batch" request which involves executing
        // a series of queries. This kind of requests needs to be processed
        // slightly differently since we need to intercept and properly handle
        // a few known (and somewhat expected) MySQL errors w/o aborting
        // the whole request.
        if (_batchMode) {
            // Count the number of failures for proper error reporting on
            // the current request.
            size_t numFailures = 0;
            bool first = true;
            for (string const& table : _tables) {
                // If this is the very first iteration of the loop then use
                // the default result set created earlier. Otherwise create
                // a new one.
                if (exchange(first, false) == false) {
                    resultSet = _currentResultSet(lock, true);
                }
                resultSet["scope"] = table;
                try {
                    ConnectionHandler const h(connection);
                    h.conn->execute([&](decltype(h.conn) const& conn_) {
                        conn_->begin();
                        auto const query = _generateQuery(conn_, table);
                        if (query.mutexName.empty()) {
                            conn_->execute(query.query);
                        } else {
                            replica::Lock const lock(serviceProvider()->getNamedMutex(query.mutexName),
                                                     CONTEXT);
                            conn_->execute(query.query);
                        }
                        _extractResultSet(lock, conn_);
                        conn_->commit();
                    });
                } catch (database::mysql::ER_NO_SUCH_TABLE_ const& ex) {
                    ++numFailures;
                    resultSet["status_ext"] = protocol::StatusExt::NO_SUCH_TABLE;
                    resultSet["status_ext_str"] = protocol::toString(protocol::StatusExt::NO_SUCH_TABLE);
                    resultSet["error"] = string(ex.what());
                } catch (database::mysql::ER_PARTITION_MGMT_ON_NONPARTITIONED_ const& ex) {
                    ++numFailures;
                    resultSet["status_ext"] = protocol::StatusExt::NOT_PARTITIONED_TABLE;
                    resultSet["status_ext_str"] =
                            protocol::toString(protocol::StatusExt::NOT_PARTITIONED_TABLE);
                    resultSet["error"] = string(ex.what());
                } catch (database::mysql::ER_DUP_KEYNAME_ const& ex) {
                    ++numFailures;
                    resultSet["status_ext"] = protocol::StatusExt::DUPLICATE_KEY;
                    resultSet["status_ext_str"] = protocol::toString(protocol::StatusExt::DUPLICATE_KEY);
                    resultSet["error"] = string(ex.what());
                } catch (database::mysql::ER_CANT_DROP_FIELD_OR_KEY_ const& ex) {
                    ++numFailures;
                    resultSet["status_ext"] = protocol::StatusExt::CANT_DROP_KEY;
                    resultSet["status_ext_str"] = protocol::toString(protocol::StatusExt::CANT_DROP_KEY);
                    resultSet["error"] = string(ex.what());
                }
            }
            if (numFailures > 0) {
                setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::MULTIPLE);
            } else {
                setStatus(lock, protocol::Status::SUCCESS);
            }
        } else {
            // TODO: the algorithm will only report a result set of the last query
            // from the multi-query collections. The implementations of the corresponding
            // requests should take this into account.
            ConnectionHandler const h(connection);
            h.conn->execute([&](decltype(h.conn) const& conn_) {
                conn_->begin();
                for (auto const& query : _queries(conn_)) {
                    if (query.mutexName.empty()) {
                        conn_->execute(query.query);
                    } else {
                        replica::Lock const lock(serviceProvider()->getNamedMutex(query.mutexName), CONTEXT);
                        conn_->execute(query.query);
                    }
                    _extractResultSet(lock, conn_);
                }
                conn_->commit();
            });
            setStatus(lock, protocol::Status::SUCCESS);
        }
    } catch (database::mysql::ER_NO_SUCH_TABLE_ const& ex) {
        _reportFailure(lock, protocol::StatusExt::NO_SUCH_TABLE, ex.what());
    } catch (database::mysql::ER_PARTITION_MGMT_ON_NONPARTITIONED_ const& ex) {
        _reportFailure(lock, protocol::StatusExt::NOT_PARTITIONED_TABLE, ex.what());
    } catch (database::mysql::ER_DUP_KEYNAME_ const& ex) {
        _reportFailure(lock, protocol::StatusExt::DUPLICATE_KEY, ex.what());
    } catch (database::mysql::ER_CANT_DROP_FIELD_OR_KEY_ const& ex) {
        _reportFailure(lock, protocol::StatusExt::CANT_DROP_KEY, ex.what());
    } catch (database::mysql::Error const& ex) {
        _reportFailure(lock, protocol::StatusExt::MYSQL_ERROR, ex.what());
    } catch (invalid_argument const& ex) {
        _reportFailure(lock, protocol::StatusExt::INVALID_PARAM, ex.what());
    } catch (out_of_range const& ex) {
        _reportFailure(lock, protocol::StatusExt::LARGE_RESULT, ex.what());
    } catch (exception const& ex) {
        _reportFailure(lock, protocol::StatusExt::OTHER_EXCEPTION, ex.what());
    }
    return true;
}

Connection::Ptr WorkerSqlHttpRequest::_connector() const {
    // A choice of credential for connecting to the database service depends
    // on a type of the request. For the sake of greater security, arbitrary
    // queries require a client to explicitly provide the credentials.
    // Otherwise, using credentials from the worker's configuration.
    bool const clientCredentials = _sqlRequestType == protocol::SqlRequestType::QUERY;
    auto connectionParams = Configuration::qservWorkerDbParams();
    if (clientCredentials) {
        connectionParams.user = _user;
        connectionParams.password = _password;
    }
    return Connection::open(connectionParams);
}

vector<Query> WorkerSqlHttpRequest::_queries(Connection::Ptr const& conn) const {
    QueryGenerator const g(conn);
    vector<Query> queries;
    switch (_sqlRequestType) {
        case protocol::SqlRequestType::QUERY:
            queries.emplace_back(Query(_query));
            break;
        case protocol::SqlRequestType::CREATE_DATABASE: {
            bool const ifNotExists = true;
            string const query = g.createDb(_databaseInfo.name, ifNotExists);
            queries.emplace_back(Query(query));
            break;
        }
        case protocol::SqlRequestType::DROP_DATABASE: {
            bool const ifExists = true;
            string const query = g.dropDb(_databaseInfo.name, ifExists);
            queries.emplace_back(Query(query));
            break;
        }
        case protocol::SqlRequestType::ENABLE_DATABASE: {
            // Using REPLACE instead of INSERT to avoid hitting the DUPLICATE KEY error
            // if such entry already exists in the table.
            string const query = g.replace("qservw_worker", "Dbs", _databaseInfo.name);
            queries.emplace_back(Query(query));
            break;
        }
        case protocol::SqlRequestType::DISABLE_DATABASE: {
            string const where = g.where(g.eq("db", _databaseInfo.name));
            queries.emplace_back(Query(g.delete_(g.id("qservw_worker", "Chunks")) + where));
            queries.emplace_back(Query(g.delete_(g.id("qservw_worker", "Dbs")) + where));
            break;
        }
        case protocol::SqlRequestType::GRANT_ACCESS: {
            string const query = g.grant("ALL", _databaseInfo.name, _user, "localhost");
            queries.emplace_back(Query(query));
            break;
        }
        default:
            // The remaining remaining types of requests require the name of a table
            // affected by the operation.
            queries.emplace_back(_generateQuery(conn, _table));
            break;
    }
    return queries;
}

Query WorkerSqlHttpRequest::_generateQuery(Connection::Ptr const& conn, string const& table) const {
    QueryGenerator const g(conn);
    SqlId const databaseTable = g.id(_databaseInfo.name, table);
    switch (_sqlRequestType) {
        case protocol::SqlRequestType::CREATE_TABLE: {
            list<string> const keys;
            bool const ifNotExists = true;
            string query = g.createTable(databaseTable, ifNotExists, _columns, keys, _engine);

            // If MySQL partitioning was requested for the table then configure partitioning
            // parameters and add the initial partition corresponding to the default
            // transaction identifier. The table will be partitioned based on values of
            // the transaction identifiers in the specified column.
            string const partitionByColumn = _partitionByColumn;
            if (!partitionByColumn.empty()) {
                TransactionId const defaultTransactionId = 0;
                query += g.partitionByList(partitionByColumn) + g.partition(defaultTransactionId);
            }
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::DROP_TABLE: {
            bool const ifExists = true;
            string const query = g.dropTable(databaseTable, ifExists);
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::DROP_TABLE_PARTITION: {
            bool const ifExists = true;
            string const query = g.alterTable(databaseTable) + g.dropPartition(_transactionId, ifExists);
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::REMOVE_TABLE_PARTITIONING: {
            string const query = g.alterTable(databaseTable) + g.removePartitioning();
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::CREATE_TABLE_INDEX: {
            string const query =
                    g.createIndex(databaseTable, _index.name, _index.spec, _index.keys, _index.comment);
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::DROP_TABLE_INDEX: {
            string const query = g.dropIndex(databaseTable, _indexName);
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::GET_TABLE_INDEX: {
            return Query(g.showIndexes(databaseTable));
        }
        case protocol::SqlRequestType::ALTER_TABLE: {
            string const query = g.alterTable(databaseTable, _alterTableSpec);
            return Query(query, databaseTable.str);
        }
        case protocol::SqlRequestType::TABLE_ROW_STATS: {
            // The transaction identifier column is not required to be present in
            // the legacy catalogs (ingested w/o super-transactions), or in (the narrow) tables
            // in which the column was removed to save disk space. The query generator
            // implemented below accounts for this scenario by consulting MySQL's
            // information schema. If the column isn't present then the default transaction
            // identifier 0 will be injected into the result set.
            string query = g.select(Sql::COUNT_STAR) +
                           g.from(DoNotProcess(g.id("information_schema", "COLUMNS"))) +
                           g.where(g.eq("TABLE_SCHEMA", _databaseInfo.name), g.eq("TABLE_NAME", table),
                                   g.eq("COLUMN_NAME", "qserv_trans_id"));
            int count = 0;
            selectSingleValue(conn, query, count);
            if (count == 0) {
                string const query =
                        g.select(g.as(g.val(0), "qserv_trans_id"), g.as(Sql::COUNT_STAR, "num_rows")) +
                        g.from(DoNotProcess(databaseTable));
                return Query(query);
            }
            query = g.select("qserv_trans_id", g.as(Sql::COUNT_STAR, "num_rows")) +
                    g.from(DoNotProcess(databaseTable)) + g.groupBy("qserv_trans_id");
            return Query(query);
        }
        default:
            throw invalid_argument(
                    CONTEXT + " not the table-scope request type: " + protocol::toString(_sqlRequestType));
    }
}

void WorkerSqlHttpRequest::_extractResultSet(replica::Lock const& lock, Connection::Ptr const& conn) {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT);

    json& resultSet = _currentResultSet(lock);

    // This will explicitly reset the default failure mode as it was
    // initialized by the constructor of the result set class.
    resultSet["status_ext"] = protocol::StatusExt::NONE;
    resultSet["status_ext_str"] = protocol::toString(protocol::StatusExt::NONE);

    // Now carry over the actual rest set (if any)
    resultSet["char_set_name"] = conn->charSetName();
    resultSet["has_result"] = conn->hasResult() ? 1 : 0;
    if (conn->hasResult()) {
        resultSet["fields"] = conn->fieldsToJson();
        resultSet["rows"] = json::array();
        json& rowsJson = resultSet["rows"];
        size_t numRowsProcessed = 0;
        Row row;
        while (conn->next(row)) {
            if (_maxRows != 0) {
                if (numRowsProcessed >= _maxRows) {
                    throw out_of_range(CONTEXT + " max_rows=" + to_string(_maxRows) + " limit exceeded");
                }
                ++numRowsProcessed;
            }
            rowsJson.push_back(row.toJson());
        }
    }
}

void WorkerSqlHttpRequest::_reportFailure(replica::Lock const& lock, protocol::StatusExt statusExt,
                                          string const& error) {
    LOGS(_log, LOG_LVL_ERROR, CONTEXT << " exception: " << error);

    // Note that the actual reason for a query to fail is recorded in its
    // result set, while the final state of the whole request may vary
    // depending on a kind of the request - if it's a simple or the "batch"
    // request.
    json& resultSet = _currentResultSet(lock);
    resultSet["status_ext"] = statusExt;
    resultSet["status_ext_str"] = protocol::toString(statusExt);
    resultSet["error"] = error;
    setStatus(lock, protocol::Status::FAILED, _batchMode ? statusExt : protocol::StatusExt::MULTIPLE);
}

json& WorkerSqlHttpRequest::_currentResultSet(replica::Lock const& lock, bool create) {
    if (create) _resultSets.push_back(json::object());
    if (_resultSets.size() != 0) return _resultSets.back();
    throw logic_error(CONTEXT + " the operation is not allowed in this state");
}

}  // namespace lsst::qserv::replica
