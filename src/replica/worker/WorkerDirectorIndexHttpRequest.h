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
#ifndef LSST_QSERV_REPLICA_WORKERDIRECTORINDEXHTTPREQUEST_H
#define LSST_QSERV_REPLICA_WORKERDIRECTORINDEXHTTPREQUEST_H

// System headers
#include <memory>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/config/ConfigDatabase.h"
#include "replica/proto/Protocol.h"
#include "replica/util/Common.h"
#include "replica/worker/WorkerHttpRequest.h"

// Forward declarations
namespace lsst::qserv::replica {
class ServiceProvider;
}  // namespace lsst::qserv::replica

namespace lsst::qserv::replica::database::mysql {
class Connection;
class ConnectionPool;
}  // namespace lsst::qserv::replica::database::mysql

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerDirectorIndexHttpRequest queries a director table (the whole or just one MySQL
 * partition, depending on parameters of the request) of a database
 * to extracts data to be loaded into the "director" index.
 */
class WorkerDirectorIndexHttpRequest : public WorkerHttpRequest {
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
     * @param connectionPool a pool of connections to the MySQL/MariaDB server
     * @return pointer to the created object
     */
    static std::shared_ptr<WorkerDirectorIndexHttpRequest> create(
            std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker,
            protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req,
            ExpirationCallbackType const& onExpired,
            std::shared_ptr<database::mysql::ConnectionPool> const& connectionPool);

    WorkerDirectorIndexHttpRequest() = delete;
    WorkerDirectorIndexHttpRequest(WorkerDirectorIndexHttpRequest const&) = delete;
    WorkerDirectorIndexHttpRequest& operator=(WorkerDirectorIndexHttpRequest const&) = delete;

    ~WorkerDirectorIndexHttpRequest() override = default;

    bool execute() override;

protected:
    void getResult(nlohmann::json& result) const override;

private:
    WorkerDirectorIndexHttpRequest(std::shared_ptr<ServiceProvider> const& serviceProvider,
                                   std::string const& worker, protocol::QueuedRequestHdr const& hdr,
                                   nlohmann::json const& req, ExpirationCallbackType const& onExpired,
                                   std::shared_ptr<database::mysql::ConnectionPool> const& connectionPool);

    /**
     * The query generator uses parameters of a request to compose
     * a desired query.
     *
     * @param conn a reference to the database connector is needed to process
     *   arguments to meet requirements of the database query processing engine.
     * @return a query as per the input request
     * @throws std::invalid_argument if the input parameters are not supported
     */
    std::string _query(std::shared_ptr<database::mysql::Connection> const& conn) const;

    /**
     * Read the content of the file into memory starting from the given offset.
     * @note The maximum number of bytes to read is set in the Configuration
     *   parameter (worker, director-index-record-size).
     * @param offset A position of the first byte in the file to read.
     * @return The completion status to be returned to the Controller.
     */
    protocol::StatusExt _readFile(size_t offset);

    /// Get rid of the temporary file if it's still tehre.
    void _removeFile() const;

    // Input parameters
    DatabaseInfo const _databaseInfo;  ///< Database descriptor obtained from the Configuration
    TableInfo const _tableInfo;        ///< Director table descriptor obtained from the Configuration
    bool const _hasTransactions;
    TransactionId const _transactionId;
    unsigned int const _chunk;
    std::size_t const _offset;
    std::shared_ptr<database::mysql::ConnectionPool> const _connectionPool;

    /// The path name of a temporary folder where the file will be stored.
    /// The folder gets created before extracting data from the MySQL table
    /// into the file.
    std::string const _tmpDirName;

    /// The full path name of a temporary file into which the TSV/CSV dump will be made.
    /// This file will get deleted when its whole content is sent to the Controller.
    std::string const _fileName;

    /// The size of the file is determined each time before reading it.
    size_t _fileSizeBytes = 0;

    /// Cached error to be sent to a client
    std::string _error;

    /// In-memory storage for the content of the file upon a successful completion
    /// of the data extraction query.
    std::string _data;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERDIRECTORINDEXHTTPREQUEST_H
