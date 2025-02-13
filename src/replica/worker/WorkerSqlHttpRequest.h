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
#ifndef LSST_QSERV_REPLICA_WORKERSQLHTTPREQUEST_H
#define LSST_QSERV_REPLICA_WORKERSQLHTTPREQUEST_H

// System headers
#include <list>
#include <string>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/config/ConfigDatabase.h"
#include "replica/mysql/DatabaseMySQL.h"
#include "replica/proto/Protocol.h"
#include "replica/util/Common.h"
#include "replica/worker/WorkerHttpRequest.h"

// Forward declarations
namespace lsst::qserv::replica {
class ServiceProvider;
}  // namespace lsst::qserv::replica

namespace lsst::qserv::replica::database::mysql {
class Connection;
}  // namespace lsst::qserv::replica::database::mysql

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerSqlHttpRequest executes queries against the worker database
 * and return results sets (if any) back to a caller.
 *
 * @note Queries passed into this operation are supposed to be well formed.
 * If a MySQL error would occur during an attempt to execute an incorrectly
 * formed query then the corresponding MySQL error will be recorded
 * and reported to a caller in the response structure which is set
 * by method WorkerSqlHttpRequest::setInfo().
 */
class WorkerSqlHttpRequest : public WorkerHttpRequest {
public:
    /**
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * @param serviceProvider provider is needed to access the Configuration
     *   of a setup and for validating the input parameters
     * @param worker the name of a worker. The name must match the worker which
     *   is going to execute the request.
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @param onExpired request expiration callback function
     * @return pointer to the created object
     */
    static std::shared_ptr<WorkerSqlHttpRequest> create(
            std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker,
            protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req,
            ExpirationCallbackType const& onExpired);

    WorkerSqlHttpRequest() = delete;
    WorkerSqlHttpRequest(WorkerSqlHttpRequest const&) = delete;
    WorkerSqlHttpRequest& operator=(WorkerSqlHttpRequest const&) = delete;

    ~WorkerSqlHttpRequest() override = default;

    bool execute() override;

protected:
    void getResult(nlohmann::json& result) const override;

private:
    WorkerSqlHttpRequest(std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker,
                         protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req,
                         ExpirationCallbackType const& onExpired);

    /// @return A connector as per the input request
    std::shared_ptr<database::mysql::Connection> _connector() const;

    /**
     * The query generator for simple requests uses parameters of a request
     * to compose a collection of desired queries.
     * @note this method is capable of generating a single or multiple queries
     *   as needed by the corresponding non-batch requests.
     * @param conn A reference to the database connector is needed to process
     *   arguments to meet requirements of the database query processing engine.
     * @return A collection of queries to be executed as per the input request.
     * @throw std::invalid_argument For unsupported requests types supported.
     */
    std::vector<Query> _queries(std::shared_ptr<database::mysql::Connection> const& conn) const;

    /**
     * The query generator for queries which have a target table.
     * @param conn A reference to the database connector is needed to process
     *   arguments to meet requirements of the database query processing engine.
     * @param table The name of table affected by the query.
     * @return A query as per the input request and the name of a table.
     * @throw std::invalid_argument For unsupported requests types.
     */
    Query _generateQuery(std::shared_ptr<database::mysql::Connection> const& conn,
                         std::string const& table) const;

    /**
     * Extract a result set (if any) via the database connector into
     * the Protobuf response object.
     * @param lock The lock must be held before calling the method since it's
     *   going to access a protected state of the object.
     * @param conn  a valid database connector for extracting a result set
     */
    void _extractResultSet(replica::Lock const& lock,
                           std::shared_ptr<database::mysql::Connection> const& conn);

    /**
     * Report & record a failure
     *
     * @param lock The lock must be held before calling the method since it's
     *   going to modify a protected state of the object.
     * @param statusExt An extended status to be reported to Controllers and
     *   set in the current (most recently processed query if any) result set.
     * @param error A message to be logged and returned to Controllers.
     * @throw std::logic_error Is thrown when the method is called before
     *   creating a result set.
     */
    void _reportFailure(replica::Lock const& lock, protocol::StatusExt statusExt, std::string const& error);

    /// @param lock The lock must be held before calling the method since it's
    ///   going to modify a protected state of the object.
    /// @param create A flag to indicate if a new result set should be created
    /// @return A mutable pointer to the current result set
    nlohmann::json& _currentResultSet(replica::Lock const& lock, bool create = false);

    // Input parameters (mandatory)

    protocol::SqlRequestType const _sqlRequestType;  ///< The type of the SQL request
    std::string const _user;                         ///< The name of the MySQL user (queries or grants)
    std::string const _password;       ///< The MySQL password for the user account (queries only)
    DatabaseInfo const _databaseInfo;  ///< Database descriptor obtained from the Configuration
    std::size_t const _maxRows;        ///< The maximum number of rows to be returned in a result set

    // Input parameters (of batch nmode requested)
    bool const _batchMode;             ///< A flag to indicate if the request is targeting many tables
    std::vector<std::string> _tables;  ///< A list of tables to be affected by the request

    // Input parameters (request-specific, see the constructor for further details)

    std::string _query;              ///< The query to be executed
    std::string _table;              ///< The name of the table to be affected by the request
    std::list<SqlColDef> _columns;   ///< The list of columns for a table to be created
    std::string _partitionByColumn;  ///< The name of the column to be used for partitioning
    SqlIndexDef _index;              ///< The index definition
    std::string _engine;             ///< The name of the table engine to be used
    TransactionId _transactionId;    ///< The transaction identifier
    std::string _indexName;          ///< The name of the index to be dropped
    std::string _alterTableSpec;     ///< The specification for the ALTER TABLE request

    /// Cached result to be sent to a client upon a request
    nlohmann::json _resultSets;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERSQLHTTPREQUEST_H
