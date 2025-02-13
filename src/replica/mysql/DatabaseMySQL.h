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
#ifndef LSST_QSERV_REPLICA_DATABASEMYSQL_H
#define LSST_QSERV_REPLICA_DATABASEMYSQL_H

/**
 * This header represents a C++ wrapper for the MySQL C language library.
 * The primary class of this API is Connection.
 *
 * @see class Connection
 *
 * Other public classes of the API, such as class Row, specific exception
 * classes, as well as some others, are defined in separate headers
 * included from this one:
 *
 * DatabaseMySQLExceptions.h
 * DatabaseMySQLGenerator.h
 * DatabaseMySQLRow.h
 * DatabaseMySQLTypes.h
 */

// System headers
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Third party headers
#include <mysql/mysql.h>
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/mysql/DatabaseMySQLExceptions.h"
#include "replica/mysql/DatabaseMySQLRow.h"
#include "replica/mysql/DatabaseMySQLTypes.h"
#include "replica/util/Common.h"
#include "replica/util/Mutex.h"

// Forward declarations
namespace lsst::qserv::replica {
class ProtocolResponseSqlField;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica::database::mysql {

/**
 * Class Connection provides the main API to the database.
 */
class Connection : public std::enable_shared_from_this<Connection> {
public:
    typedef std::shared_ptr<Connection> Ptr;

    /// @return value of the corresponding MySQL variable set for a session
    static unsigned long max_allowed_packet();

    /**
     * Connect to the MySQL service with the specified parameters and if successfully
     * connected return a pointer to the Connection object. Otherwise an exception will
     * be thrown.
     *
     * A behavior of a connector created by the method depends on default values
     * of Configuration parameters returned by Configuration::databaseAllowReconnect()
     * and Configuration::databaseConnectTimeoutSec(). If the automatic reconnect is
     * allowed then multiple connection attempts to a database service can be made
     * before the connection timeout expires or until some problem which can't be
     * resolved with the allowed connection retries happens.
     *
     * @note MySQL auto-commits are disabled
     * @note MySQL automatic re-connects are not allowed because this Connector class
     *   implements its own protocol for reconnects (when allowed)
     * @note connections are always open with option MYSQL_PROTOCOL_TCP.
     * @note MySQL option MYSQL_OPT_LOCAL_INFILE is always enabled to allow
     *   queries like 'LOAD DATA LOCAL INFILE ...'. This protocol is used in the bulk
     *   data ingest scenarios in which input files can't be directly placed at a location
     *   accessible by the MySQL server. Be aware that this way of loading data requires
     *   the server to be configured with global variable 'local_infile=1'. Another related
     *   requirement is that MySQL client library was compiled with option '-DENABLED_LOCAL_INFILE=true'.
     *
     * Here is an example of using this method to establish a connection:
     * @code
     *    BlockPost delayBetweenReconnects(1000,2000);
     *    ConnectionParams params = ...;
     *    Connection::Ptr conn;
     *    do {
     *        try {
     *            conn = Connection::open(params);
     *
     *        } catch (ConnectError const& ex) {
     *            cerr << "connection attempt failed: " << ex.what << endl;
     *            delayBetweenReconnects.wait();
     *            cerr << "reconnecting..." << endl;
     *
     *        } catch (ConnectTimeoutError const& ex) {
     *            cerr << "connection attempt expired after: " << ex.timeoutSec() << " seconds "
     *                 << "due to: " << ex.what << endl;
     *            throw;
     *
     *        } catch (Error const& ex) {
     *            cerr << "connection attempt failed: " << ex.what << endl;
     *            throw;
     *        }
     *   } while (nullptr != conn);
     * @endcode
     *
     * @param connectionParams parameters of a connection
     *
     * @return a valid object if the connection attempt succeeded (no nullptr
     *   to be returned under any circumstances)
     *
     * @throw ConnectTimeoutError is thrown only if the automatic reconnects
     *   are allowed to indicate that connection attempts to a server
     *   failed to be established within the specified timeout
     *
     * @throw ConnectError the exception is thrown if automatic reconnets are not allowed
     *   to indicate that the only connection attempt to a server failed
     *
     * @throw Error - for any other database errors
     *
     * @see Configuration::databaseAllowReconnect()
     * @see Configuration::databaseConnectTimeoutSec()
     * @see Connection::open2()
     */
    static Ptr open(ConnectionParams const& connectionParams);

    /**
     * The factory method allows to override default values of the corresponding
     * connection management options of the Configuration.
     *
     * @note If the timeout is set to 0 (the default value) and if reconnects are
     *   allowed then the method will assume a global value defined by
     *   the Configuration parameter: Configuration::databaseConnectTimeoutSec()
     * @note The same value of the timeout would be also assumed if the connection
     *   is lost when executing queries or pulling the result sets.
     *
     * @param connectionParams
     * @param allowReconnects  if 'true' then multiple reconnection attempts will be allowed
     * @param connectTimeoutSec  maximum number of seconds to wait before a connection with
     *   a database server is established.
     *
     * @return a valid object if the connection attempt succeeded (no nullptr
     *  to be returned under any circumstances)
     *
     * @see Configuration::databaseConnectTimeoutSec()
     * @see Connection::open()
     */
    static Ptr open2(ConnectionParams const& connectionParams, bool allowReconnects = false,
                     unsigned int connectTimeoutSec = 0);

    Connection() = delete;
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;

    virtual ~Connection();

    std::string escape(std::string const& str) const;

    ConnectionParams const& connectionParams() const { return _connectionParams; }

    /// @return maximum amount of time to wait while making reconnection attempts
    unsigned int connectTimeoutSec() const { return _connectTimeoutSec; }

    std::string charSetName() const;

    // --------------------------------------
    // Operations with the information schema
    // --------------------------------------

    /**
     * @brief Check if the table exists in a scope of a database.
     *
     * The name of the database can be specified in the optional parameter
     * \param proposedDatabase. If the value of the parameter will be found
     * empty the current database (the one specified in the ConnectionParams
     * object or set by the subsequent query "USE <database>") will be assumed.
     *
     * @param table The name of a table to check.
     * @param proposedDatabase The optional name of a database.
     * @return true if the table exists.
     * @throws std::invalid_argument If the name of the table is empty.
     * @throws Error If no valid database could be deduced from any source.
     */
    bool tableExists(std::string const& table, std::string const& proposedDatabase = std::string());

    ///  @return the status of the transaction
    bool inTransaction() const { return _inTransaction; }

    /**
     * Start a transaction
     *
     * @return a smart pointer to self to allow chained calls
     * @throw std::logic_error if the transaction was already been started
     * @throw Error for any other MySQL specific errors
     */
    Connection::Ptr begin();

    /**
     * Commit the current transaction
     *
     * @return a smart pointer to self to allow chained calls
     * @throw std::logic_error if the transaction was not started
     * @throw Error for any other MySQL specific errors
     */
    Connection::Ptr commit();

    /**
     * Rollback the current transaction
     *
     * @return smart pointer to self to allow chained calls
     * @throw std::logic_error if the transaction was not started
     * @throw Error for any other MySQL specific errors
     */
    Connection::Ptr rollback();

    /**
     * Execute the specified query and initialize object context to allow
     * a result set extraction.
     *
     * @param query query to be executed
     * @return a smart pointer to self to allow chained calls
     *
     * @throw std::invalid_argument for empty query strings
     * @throw ER_DUP_ENTRY_ for attempts to insert rows with duplicate keys
     * @throw Error for any other MySQL specific errors
     */
    Connection::Ptr execute(std::string const& query);

    /**
     * Execute a user-supplied algorithm which could be retried the specified
     * number of times (or until a given timeout expires) if a connection to
     * a server is lost and re-established before the completion of the algorithm.
     * The number of allowed auto-reconnects and the timeout are controlled by
     * the corresponding parameters of the method.
     *
     * @note In case of reconnects and retries the failed transaction will be aborted.
     * @note It's up to a user script to begin and commit a transaction as needed.
     * @note It's up to a user script to take care of side effects if the script will run
     *   more than once.
     *
     * Example:
     * @code
     *     Configuration::setDatabaseConnectTimeoutSec(60);
     *     ConnectionParams params = ... ;
     *     Connection::Ptr conn = Connection::open(params);
     *     try {
     *         conn->execute(
     *             [](Connection::Ptr conn) {
     *                 conn->begin();
     *                 conn->execute("SELECT ...");
     *                 conn->execute("INSERT ...");
     *                 conn->commit();
     *             },
     *             10                        // 10 extra attempts before to fail
     *             2 * connectionTimeoutSec  // no longer that 120 seconds
     *         );
     *     } catch (ConnectError const& ex) {
     *         cerr << "you only made one failed attempt because "
     *              << "no automatic reconnects were allowed. Open your connection "
     *              << "with factory method Connection::openWait()" << endl;
     *     } catch (ConnectTimeoutError const& ex) {
     *         cerr << "you have exausted the maximum allowed number of retries "
     *              << "within the specified (or implicitly assumed) "
     *                 "timeout: " << ex.timeoutSec() << endl;
     *     } catch (Error const& ex) {
     *         cerr << "failed due to an unrecoverable error: " << ex.what()
     *     }
     * @endcode
     *
     * @param script a user-provided function (the callable) to execute
     * @param maxReconnects (optional) maximum number of reconnects allowed
     *   If 0 is passed as a value pf the parameter then the default
     *   value corresponding configuration parameter will be
     *   assumed: Configuration::databaseMaxReconnects().
     * @param timeoutSec (optional) the maximum duration of time allowed for
     *   the procedure to wait before a connection will be established.
     *   If 0 is passed as a value pf the parameter then the default
     *   value corresponding configuration parameter will be
     *   assumed: Configuration::databaseConnectTimeoutSec().
     *
     * @throw std::invalid_argument if 'nullptr' is passed in place of 'script'
     * @throw ConnectError failed to establish a connection if connection was
     *   open with factory method Connection::open().
     * @throw ConnectTimeoutError failed to establish a connection within a timeout
     *   if a connection was open with factory method Connection::openWait().
     *
     * @throw MaxReconnectsExceeded for multiple failed attempts (due to connection losses
     *   and subsequent reconnects) to execute the user function. And the number of the attempts
     *   exceeded a limit set by parameter 'maxReconnects'.
     *
     * @throw Error the general exception for any MySQL specific errors. A client code may also
     *   catch specific subclasses (other than ConnectError or ConnectTimeoutError) of that class
     *   if needed.
     *
     * @return a pointer to the same connector against which the method was invoked
     *   in case of successful completion of the requested operaton.
     *
     * @see Configuration::databaseMaxReconnects()
     * @see Configuration::setDatabaseMaxReconnects()
     * @see Configuration::databaseConnectTimeoutSec()
     * @see Configuration::setDatabaseConnectTimeoutSec()
     * @see Connection::open()
     */
    Connection::Ptr execute(std::function<void(Ptr)> const& script, unsigned int maxReconnects = 0,
                            unsigned int timeoutSec = 0);

    /**
     * Execute a script within its own transaction to be automatically started and commited.
     *
     * @note upon a completion of the method, depending of an outcome, the transaction
     *   is guaranteed to be commited or aborted.
     * @param script user-provided function (the callable) to execute
     * @param maxReconnects
     *   (optional) maximum number of reconnects allowed
     *   If 0 is passed as a value pf the parameter then the default
     *   value corresponding configuration parameter will be
     *   assumed: Configuration::databaseMaxReconnects().
     * @param timeoutSec
     *   (optional) the maximum duration of time allowed for
     *   the procedure to wait before a connection will be established.
     *   If 0 is passed as a value pf the parameter then the default
     *   value corresponding configuration parameter will be
     *   assumed: Configuration::databaseConnectTimeoutSec().
     * @param maxRetriesOnDeadLock (optional) the number of retries on exception
     *   LockDeadlock that may happen while two or many threads/processes were
     *   trying to update the same table.
     * @see Connection::execute()
     */
    Connection::Ptr executeInOwnTransaction(std::function<void(Ptr)> const& script,
                                            unsigned int maxReconnects = 0, unsigned int timeoutSec = 0,
                                            unsigned int maxRetriesOnDeadLock = 0);

    /**
     * This is just a convenience method for a typical use case.
     * @note both scripts are executed in own transactions. No transaction cleanup
     *   will be needed upon completion of the method.
     * @param insertScript user-provided function (the callable) to be executed first
     *   to insert new data into a database.
     * @param updateScript user-provided function (the callable) to be executed as
     *   as the failover solution to update existing data in the database if the first
     *   script fails due to the ER_DUP_ENTRY_ exception.
     * @param maxReconnects
     *   (optional) maximum number of reconnects allowed
     *   If 0 is passed as a value pf the parameter then the default
     *   value corresponding configuration parameter will be
     *   assumed: Configuration::databaseMaxReconnects().
     * @param timeoutSec
     *   (optional) the maximum duration of time allowed for
     *   the procedure to wait before a connection will be established.
     *   If 0 is passed as a value pf the parameter then the default
     *   value corresponding configuration parameter will be
     *   assumed: Configuration::databaseConnectTimeoutSec().
     * @param maxRetriesOnDeadLock (optional) the number of retries on exception
     *   LockDeadlock that may happen while two or many threads/processes were
     *   trying to update the same table.
     */
    Connection::Ptr executeInsertOrUpdate(std::function<void(Ptr)> const& insertScript,
                                          std::function<void(Ptr)> const& updateScript,
                                          unsigned int maxReconnects = 0, unsigned int timeoutSec = 0,
                                          unsigned int maxRetriesOnDeadLock = 0);

    /**
     * @return 'true' if the last successful query returned a result set
     *   (even though it may be empty).
     */
    bool hasResult() const;

    /**
     * @note Columns are returned exactly in the same order they were
     *   requested in the corresponding query.
     * @return names of the columns from the current result set
     * @throw std::logic_error if no SQL statement has ever been executed, or
     *   if the last query failed.
     */
    std::vector<std::string> const& columnNames() const;

    /**
     * @return the number of columns in the current result set
     * @throw std::logic_error if no SQL statement has ever been executed, or
     *   if the last query failed.
     */
    size_t numFields() const;

    /**
     * Fill a Protobuf object representing a field
     *
     * @note The method can be called only upon a successful completion of a query
     *   which has a result set. Otherwise it will throw an exception.
     *
     * @see mysql_fetch_field()
     * @see database::mysql::Connection::hasResult
     *
     * @param ptr a pointer to the Protobuf object to be populated
     * @param idx a relative (0 based) index of the field in a result set
     *
     * @throw std::logic_error if no SQL statement has ever been executed, or
     *   if the last query failed.
     * @throw std::out_of_range if the specified index exceed the maximum index of a result set.
     */
    void exportField(ProtocolResponseSqlField* ptr, size_t idx) const;

    /**
     * Convert the current result set into a JSON object.
     *
     * @note The method can be called only upon a successful completion of a query
     *   which has a result set. Otherwise it will throw an exception.
     *
     * @see Connection::hasResult
     *
     * @return a JSON object representing the current result set
     * @throw std::logic_error if no SQL statement has ever been executed, or
     *   if the last query failed.
     */
    nlohmann::json fieldsToJson() const;

    /**
     * Move the iterator to the next (first) row of the current result set
     * and if the iterator is not beyond the last row then initialize an object
     * passed as a parameter.
     *
     * @note Objects initialized upon the successful completion
     *   of the method are valid until the next call to the method or before
     *   the next query. Hence the safe practice for using this method to iterate
     *   over a result set would be:
     * @code
     *     Connection::Ptr conn = Connection::connect(...);
     *     conn->execute ("SELECT ...");
     *
     *     Row row;
     *     while (conn->next(row)) {
     *         // Extract data from 'row' within this block
     *         // before proceeding to the next row, etc.
     *     }
     * @endcode
     *
     * @return 'true' if the row was initialized or 'false' if past the last row
     *   in the result set.
     *
     * @throw std::logic_error if no SQL statement has ever been executed, or
     *    if the last query failed.
     */
    bool next(Row& row);

    /**
     * Retrieve the number of warnings generated by the last query in the current session.
     *
     * @note Calling this method won't reset warnings, which can be retrieved later
     *   by calling method warnings(). Calling this method many times in a row will return
     *   the same result.
     * @see Connection::warnings
     *
     * @return A value of the counter (0 or higher).
     */
    unsigned int warningCount();

    /**
     * Retrieve warning, errors or notes generated after executing the last statement.
     * This operation is implemented using the corresponding MySQL query.
     * @code
     *   SHOW WARNINGS [LIMIT <max-num-warnings> [OFFSET <offset>]]
     * @endcode
     *
     * @note The operation will not reset the query statistics.  Calling this method
     *   many times in a row will return the same result.
     * @see Connection::warningCount
     * @param maxNumWarnings The maximum number of entries to be reported. In case
     *   if a value of the parameter is set to 0 then the number of rows reported
     *   by the method won't exceed a limit set in the MySQL/MariaDB system
     *   variable (GLOBAL or SESSION) 'max_error_count'. More details on this subject
     *   can be found in at https://mariadb.com/docs/reference/mdb/system-variables/max_error_count/.
     *   Note that the limit needs to be set before executing the corresponding query for
     *   which the warnings are collected.
     * @param offset The number of entries to skip. The number is ignored if
     *   \param maxNumWarnings is set to 0.
     * @return A collection of entries.
     */
    std::list<Warning> warnings(unsigned int maxNumWarnings = 0, unsigned int offset = 0);

    /**
     * Retreive the number of rows affected by the last query.
     * @see https://mariadb.com/kb/en/mysql_affected_rows/
     * @return unsigned long long - the number of rows reported by MySQL.
     */
    uint64_t affectedRows();

private:
    /**
     * @see Connection::open()
     * @see Connection::open2()
     */
    Connection(ConnectionParams const& connectionParams, unsigned int connectTimeoutSec);

    /**
     * Keep trying to connect to a server until either a timeout expires, or
     * some unrecoverable failure happens while trying to establish a connection.
     *
     * @throw ConnectTimeoutError if failed to establish a connection within a timeout
     * @throw Error for other problems when preparing or establishing a connection
     */
    void _connect();

    /**
     * Make exactly one attempt to establish a connection
     *
     * @throw ConnectError connection to a server failed
     * @throw Error other problem when preparing or establishing a connection
     */
    void _connectOnce();

    /**
     * The method is called to process the last error, reconnect if needed (and allowed), etc.
     *
     * @param context a context from which this method was called
     * @param instantAutoReconnect an optional flag allowing to make instant reconnects for
     *   qualified error conditions
     *
     * @throw std::logic_error if the method is called after no actual error happened
     * @throw Reconnected after a successful reconnection has happened
     * @throw ConnectError connection to a server failed
     * @throw ER_DUP_ENTRY_ after the last statement attempted to violate
     *   the corresponding key constraint
     * @throw Error for some other error not listed above
     */
    void _processLastError(std::string const& context, bool instantAutoReconnect = true);

    /**
     * The method is to ensure that the transaction is in the desired state.
     * @param inTransaction the desired state of the transaction
     */
    void _assertTransaction(bool inTransaction) const;

    /**
     * The method is to ensure that a proper query context is set and
     * its result set can be explored.
     * @throw Error if the connection is not established or no prior query was made
     */
    void _assertQueryContext() const;

    static std::atomic<size_t>
            _nextId;   // The next available connector identifier in the generator's sequence
    size_t const _id;  // Unique identifier of this connector
    ConnectionParams const _connectionParams;  // Parameters of the connection
    unsigned int _connectTimeoutSec;           // Time to wait between reconnection attempts
    std::string _lastQuery;                    // The last SQL statement
    bool _inTransaction;                       // Transaction status
    MYSQL* _mysql;                             // MySQL connection
    unsigned long _mysqlThreadId;              // Thread ID of the current connection
    unsigned long _connectionAttempt;          // The counter of attempts between successful reconnects

    // A result set of the last successful query

    MYSQL_RES* _res;
    MYSQL_FIELD* _fields;
    size_t _numFields;
    std::vector<std::string> _columnNames;
    std::map<std::string, size_t> _name2index;

    std::string _charSetName;  // of the current connection

    // The row object gets updated after fetching each row of the result set.
    // It's required to be cached here to ensure at least the same lifespan as
    // the one of thw current class while a client is processing the last result set.
    MYSQL_ROW _row;
};

/**
 * Class ConnectionPool manages a pool of the similarly configured persistent
 * database connection. The number of connections is determined by the corresponding
 * Configuration parameter. Connections will be added to the pool (up to that limit)
 * on demand. This ensures that the class constructor is not blocking in case if
 * (or while) the corresponding MySQL/MariaDB service is not responding.
 *
 * @note This class is meant to be used indirectly by passing its instances
 *   to the constructor of class ConnectionHandler.
 * @see class ConnectionHandler
 */
class ConnectionPool {
public:
    typedef std::shared_ptr<ConnectionPool> Ptr;

    /**
     * The static factory object creates a pool and sets the maximum number
     * of connections.
     *
     * @note this is a non-blocking method. In particular, No connection attempts
     *   will be made by this method.
     *
     * @param params  parameters of the connections
     * @param maxConnections  the maximum number of connections managed by the pool
     * @return smart pointer to the newly created object
     */
    static Ptr create(ConnectionParams const& params, size_t maxConnections);

    ConnectionPool() = delete;
    ConnectionPool(ConnectionPool const&) = delete;
    ConnectionPool& operator=(ConnectionPool const&) = delete;

    /**
     * Allocate (and open a new if required/possible) connection
     *
     * @note the requester must return the service back after it's no longer needed.
     * @return pointer to the allocated connector
     * @see ConnectionPool::release()
     */
    Connection::Ptr allocate();

    /**
     * Return a connection object back into the pool of the available ones.
     *
     * @param conn  connection object to be returned back
     * @throw std::logic_error if the object was not previously allocated
     * @see ConnectionPool::allocate()
     */
    void release(Connection::Ptr const& conn);

private:
    /// @see ConnectionPool::create
    ConnectionPool(ConnectionParams const& params, size_t maxConnections);

    // Input parameters

    ConnectionParams const _params;
    size_t const _maxConnections;

    /// Connection objects which are available
    std::list<Connection::Ptr> _availableConnections;

    /// Connection objects which are in use
    std::list<Connection::Ptr> _usedConnections;

    /// The mutex for enforcing thread safety of the class's public API and
    /// internal operations. The mutex is locked by methods ConnectionPool::allocate()
    /// and ConnectionPool::release() when moving connection objects between
    /// the lists (defined above).
    mutable std::mutex _mtx;

    /// The condition variable for notifying client threads waiting for the next
    /// available connection.
    std::condition_variable _available;
};

/**
 * Class ConnectionHandler implements the RAII method of handling database
 * connection.
 */
class ConnectionHandler {
public:
    /**
     * The default constructor will initialize the connection pointer
     * with 'nullptr'
     */
    ConnectionHandler() = default;

    /**
     * Construct with a connection
     *
     * @param conn_  connection to be watched and managed
     */
    explicit ConnectionHandler(Connection::Ptr const& conn_);

    /**
     * Construct with a pointer to a connection pool for allocating
     * a connection. The connection will get released by the destructor.
     *
     * @param pool  connection pool to acquire persistent connections
     */
    explicit ConnectionHandler(ConnectionPool::Ptr const& pool);

    ConnectionHandler(ConnectionHandler const&) = delete;
    ConnectionHandler& operator=(ConnectionHandler const&) = delete;

    /**
     * The destructor will rollback a transaction if any was started at
     * a presence of a connection.
     */
    ~ConnectionHandler();

    /// The smart reference to the connector object (if any))
    Connection::Ptr conn;

private:
    /// The smart reference to the connector pool object (if any))
    ConnectionPool::Ptr _pool;
};

}  // namespace lsst::qserv::replica::database::mysql

#endif  // LSST_QSERV_REPLICA_DATABASEMYSQL_H
