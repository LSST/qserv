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
#ifndef LSST_QSERV_REPLICA_CONTROLLERAPP_H
#define LSST_QSERV_REPLICA_CONTROLLERAPP_H

// System headers
#include <cstdint>
#include <limits>
#include <string>

// Qserv headers
#include "replica/apps/Application.h"
#include "replica/contr/Controller.h"
#include "replica/requests/Request.h"
#include "replica/util/Common.h"

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class ControllerApp implements a tool for testing all known types of
 * the Controller requests.
 */
class ControllerApp : public Application {
public:
    typedef std::shared_ptr<ControllerApp> Ptr;

    /**
     * The factory method is the only way of creating objects of this class
     * because of the very base class's inheritance from 'enable_shared_from_this'.
     *
     * @param argc the number of command-line arguments
     * @param argv the vector of command-line arguments
     */
    static Ptr create(int argc, char* argv[]);

    ControllerApp() = delete;
    ControllerApp(ControllerApp const&) = delete;
    ControllerApp& operator=(ControllerApp const&) = delete;

    ~ControllerApp() override = default;

protected:
    int runImpl() final;

private:
    ControllerApp(int argc, char* argv[]);

    void _configureParser();
    void _configureParserCommandREPLICATE();
    void _configureParserCommandDELETE();
    void _configureParserCommandFIND();
    void _configureParserCommandFINDALL();
    void _configureParserCommandECHO();
    void _configureParserCommandSQL();
    void _configureParserCommandINDEX();
    void _configureParserCommandSTATUS();
    void _configureParserCommandSTOP();
    void _configureParserCommandDISPOSE();
    void _configureParserCommandSERVICE();

    Request::Ptr _launchStatusRequest(Controller::Ptr const& controller) const;
    Request::Ptr _launchStopRequest(Controller::Ptr const& controller) const;

    /// The type of a request
    std::string _requestType;

    /// The type of a request affected by the STATUS and STOP requests
    std::string _affectedRequest;

    /// The name of a worker which will execute a request
    std::string _workerName;

    /// The name of a source worker for the replication operation
    std::string _sourceWorkerName;

    /// The name of database
    std::string _databaseName;

    /// An identifier of a request for operations over known requests
    std::string _affectedRequestId;

    /// The number of a chunk
    unsigned int _chunkNumber = 0;

    /// The flag that defines a type of a table.
    bool _isOverlap = false;

    /// The data string to be sent to a worker in the ECHO request
    std::string _echoData;

    /// The optional delay (milliseconds) to be made by a worker before replying
    /// to the ECHO requests
    uint64_t _echoDelayMilliseconds = 0;

    /// An SQL query to be executed by a worker
    std::string _sqlQuery;

    /// A database user for establishing a connection with the worker's database
    std::string _sqlUser;

    /// A database password for establishing a connection with the worker's database
    std::string _sqlPassword;

    /// The name of a database
    std::string _sqlDatabase;

    /// The name of a table
    std::string _sqlTable;

    /// The name of a MySQL engine for tables to be created
    std::string _sqlEngine;

    /// The name of a file where to read table schema from
    std::string _sqlSchemaFile;

    /// The name of the PRIMARY KEY for the MySQL partitioned tables
    std::string _sqlPartitionByColumn;

    /// The name of an index to be created
    std::string _sqlIndexName;

    /// The type specification of an index.
    std::string _sqlIndexSpecStr;

    /// The optional comment explaining an index to be created
    std::string _sqlIndexComment;

    /// The name of a file where to read definitions of the index's columns
    std::string _sqlIndexColumnsFile;

    /// A specification of what's to change following the SQL's 'ALTER TABLE <table> '
    std::string _sqlAlterSpec;

    /// An identifier of a super-transaction
    TransactionId _transactionId = std::numeric_limits<TransactionId>::max();

    /// The optional limit for the total number of rows to be pulled from a result
    /// set when executing queries against the worker's database. The default value
    /// of 0 won't enforce any such limit.
    uint64_t _sqlMaxRows = 0;

    /// The number of rows in the table of a query result set (0 means no pages)
    size_t _sqlPageSize = 20;

    /// The optional (milliseconds) to wait before cancelling (if the number of not 0)
    /// the earlier made request.
    uint64_t _cancelDelayMilliseconds = 0;

    /// The priority level of a request
    int _priority = 0;

    /// Do not track requests waiting before they finish
    bool _doNotTrackRequest = false;

    /// Do not save the replica info in the database if set to 'true'
    bool _doNotSaveReplicaInfo = false;

    /// Automatically compute and store in the database check/control sums of
    /// the replica's files.
    bool _computeCheckSum = false;

    /// The flag that (if set to 'true') will result in printing the index data
    /// onto the standard output stream.  Otherwise opnly the number of bytes will
    /// be printed.
    bool _printDirectorIndexData = false;
};

}  // namespace lsst::qserv::replica

#endif /* LSST_QSERV_REPLICA_CONTROLLERAPP_H */