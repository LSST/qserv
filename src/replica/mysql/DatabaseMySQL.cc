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
#include "replica/mysql/DatabaseMySQL.h"

// System headers
#include <sstream>
#include <stdexcept>

// Third party headers
#include <mysql/mysqld_error.h>
#include <mysql/errmsg.h>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/mysql/DatabaseMySQLGenerator.h"
#include "replica/mysql/DatabaseMySQLUtils.h"
#include "replica/proto/protocol.pb.h"
#include "replica/util/Performance.h"
#include "util/BlockPost.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DatabaseMySQL");

}  // namespace

namespace lsst::qserv::replica::database::mysql {

atomic<size_t> Connection::_nextId{0};

unsigned long Connection::max_allowed_packet() {
    // Reasons behind setting this parameter to 4 MB can be found here:
    // https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_max_allowed_packet

    return 4 * 1024 * 1024;
}

Connection::Ptr Connection::open(ConnectionParams const& connectionParams) {
    return open2(connectionParams, Configuration::databaseAllowReconnect(),
                 Configuration::databaseConnectTimeoutSec());
}

Connection::Ptr Connection::open2(ConnectionParams const& connectionParams, bool allowReconnects,
                                  unsigned int connectTimeoutSec) {
    unsigned int const effectiveConnectTimeoutSec =
            0 == connectTimeoutSec ? Configuration::databaseConnectTimeoutSec() : connectTimeoutSec;
    Connection::Ptr ptr(new Connection(connectionParams, allowReconnects ? effectiveConnectTimeoutSec : 0));
    ptr->_connect();
    return ptr;
}

Connection::Connection(ConnectionParams const& connectionParams, unsigned int connectTimeoutSec)
        : _id(++_nextId),
          _connectionParams(connectionParams),
          _connectTimeoutSec(connectTimeoutSec),
          _inTransaction(false),
          _mysql(nullptr),
          _mysqlThreadId(0),
          _connectionAttempt(0),
          _res(nullptr),
          _fields(nullptr),
          _numFields(0) {
    LOGS(_log, LOG_LVL_TRACE, "Connection[" + to_string(_id) + "]  constructed");
}

Connection::~Connection() {
    if (nullptr != _res) mysql_free_result(_res);
    if (nullptr != _mysql) {
        // Resetting the connection would release all table locks, roll back any
        // outstanding transactions, etc. See details at:
        // https://dev.mysql.com/doc/c-api/8.0/en/mysql-reset-connection.html
        // Ignore the status code returned by the method.
        mysql_reset_connection(_mysql);
        mysql_close(_mysql);
    }
    LOGS(_log, LOG_LVL_TRACE, "Connection[" + to_string(_id) + "]  destructed");
}

string Connection::escape(string const& inStr) const {
    if (nullptr == _mysql) {
        throw Error("Connection[" + to_string(_id) + "]::" + string(__func__) +
                    "  not connected to the MySQL service");
    }
    size_t const inLen = inStr.length();

    // Allocate at least that number of bytes to cover the worst case scenario
    // of each input character to be escaped plus the end of string terminator.
    // See: https://dev.mysql.com/doc/refman/5.7/en/mysql-real-escape-string.html

    size_t const outLenMax = 2 * inLen + 1;

    // The temporary storage will get automatically deconstructed in the end
    // of this block.

    unique_ptr<char[]> outStr(new char[outLenMax]);
    size_t const outLen = mysql_real_escape_string(_mysql, outStr.get(), inStr.c_str(), inLen);

    return string(outStr.get(), outLen);
}

string Connection::charSetName() const {
    if (nullptr == _mysql) {
        throw Error("Connection[" + to_string(_id) + "]::" + string(__func__) +
                    "  not connected to the MySQL service");
    }
    return _charSetName;
}

bool Connection::tableExists(string const& table, string const& proposedDatabase) {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + "  ";
    if (table.empty()) {
        throw invalid_argument(context + "the table name can't be empty.");
    }
    auto const self = shared_from_this();
    QueryGenerator const g(self);
    string database = proposedDatabase;
    if (database.empty()) {
        string const query = g.select(Sql::DATABASE);
        if (!selectSingleValue(self, query, database)) {
            throw Error(context + "the name of a database is not set on this connection.");
        }
    }
    size_t count = 0;
    string const query = g.select(Sql::COUNT_STAR) + g.from(g.id("information_schema", "TABLES")) +
                         g.where(g.eq("TABLE_SCHEMA", database), g.eq("TABLE_NAME", table));
    return selectSingleValue(self, query, count) && count != 0;
}

Connection::Ptr Connection::begin() {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    _assertTransaction(false);
    execute("BEGIN");
    _inTransaction = true;
    return shared_from_this();
}

Connection::Ptr Connection::commit() {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    _assertTransaction(true);
    execute("COMMIT");
    _inTransaction = false;
    return shared_from_this();
}

Connection::Ptr Connection::rollback() {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    _assertTransaction(true);
    execute("ROLLBACK");
    _inTransaction = false;
    return shared_from_this();
}

void Connection::_processLastError(string const& context, bool instantAutoReconnect) {
    string const msg = context + ", error: " + string(mysql_error(_mysql)) +
                       ", errno: " + to_string(mysql_errno(_mysql));

    // Note, that according to the MariaDB documentation:
    //
    // "...Error codes from 1900 and up are specific to MariaDB, while error codes
    // from 1000 to 1800 are shared by MySQL and MariaDB..."
    //
    // See: https://mariadb.com/kb/en/library/mariadb-error-codes/

    switch (mysql_errno(_mysql)) {
        case 0:
            throw logic_error(string(__func__) + "  inappropriate use of this method from context: " + msg);

        case ER_DUP_KEYNAME:
            throw ER_DUP_KEYNAME_(msg);

        case ER_DUP_ENTRY:
            throw ER_DUP_ENTRY_(msg);

        case ER_CANT_DROP_FIELD_OR_KEY:
            throw ER_CANT_DROP_FIELD_OR_KEY_(msg);

        case ER_BAD_DB_ERROR:
            throw ER_BAD_DB_ERROR_(msg);

        case ER_DB_CREATE_EXISTS:
            throw ER_DB_CREATE_EXISTS_(msg);

        case ER_DB_DROP_EXISTS:
            throw ER_DB_DROP_EXISTS_(msg);

        case ER_DBACCESS_DENIED_ERROR:
            throw ER_DBACCESS_DENIED_ERROR_(msg);

        case ER_ACCESS_DENIED_ERROR:
            throw ER_ACCESS_DENIED_ERROR_(msg);

        case ER_TABLE_EXISTS_ERROR:
            throw ER_TABLE_EXISTS_ERROR_(msg);

        case ER_BAD_TABLE_ERROR:
            throw ER_BAD_TABLE_ERROR_(msg);

        case ER_NO_SUCH_TABLE:
            throw ER_NO_SUCH_TABLE_(msg);

        case ER_PARTITION_MGMT_ON_NONPARTITIONED:
            throw ER_PARTITION_MGMT_ON_NONPARTITIONED_(msg);

        case ER_UNKNOWN_PARTITION:
            throw ER_UNKNOWN_PARTITION_(msg);

        case ER_DROP_PARTITION_NON_EXISTENT:
            throw ER_DROP_PARTITION_NON_EXISTENT_(msg);

        case ER_LOCK_DEADLOCK:
            throw ER_LOCK_DEADLOCK_(msg);

        case ER_ABORTING_CONNECTION:
        case ER_NEW_ABORTING_CONNECTION:
        case ER_CONNECTION_ALREADY_EXISTS:  // MariaDB specific internal error
        case ER_CONNECTION_KILLED:          // MariaDB specific internal error
        case ER_FORCING_CLOSE:
        case ER_NORMAL_SHUTDOWN:
        case ER_SHUTDOWN_COMPLETE:
        case ER_SERVER_SHUTDOWN:
        case ER_NET_READ_ERROR:
        case ER_NET_READ_INTERRUPTED:
        case ER_NET_ERROR_ON_WRITE:
        case ER_NET_WRITE_INTERRUPTED:
        case CR_CONNECTION_ERROR:
        case CR_CONN_HOST_ERROR:
        case CR_LOCALHOST_CONNECTION:
        case CR_MALFORMED_PACKET:
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_HANDSHAKE_ERR:
        case CR_SERVER_LOST:
        case CR_SERVER_LOST_EXTENDED:
        case CR_TCP_CONNECTION:
        case CR_UNKNOWN_HOST:
            if (instantAutoReconnect) {
                // Attempt to reconnect before notifying a client if the re-connection
                // timeout is enabled during the connector's construction.
                if (_connectTimeoutSec > 0) {
                    _connect();
                    throw Reconnected(msg);
                }
            }
            throw ConnectError(msg);
        default:
            // For other error conditions encapsulate error message into a general
            // database exception.
            throw Error(msg);
    }
}

Connection::Ptr Connection::execute(string const& query) {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context << query);

    if (query.empty()) {
        throw invalid_argument(context + "empty query string passed into the object");
    }

    // Reset/initialize the query context before attempting to execute
    // the new  query.

    _lastQuery = query;

    if (_res) mysql_free_result(_res);
    _res = nullptr;
    _fields = nullptr;
    _numFields = 0;

    _columnNames.clear();
    _name2index.clear();

    if (0 != mysql_real_query(_mysql, _lastQuery.c_str(), _lastQuery.size())) {
        _processLastError(context + "mysql_real_query failed, query: '" + _lastQuery + "'");
    }

    // Fetch result set for queries which return the one

    if (0 != mysql_field_count(_mysql)) {
        // Unbuffered read

        if (nullptr == (_res = mysql_use_result(_mysql))) {
            _processLastError(context + "mysql_use_result failed");
        }
        _numFields = mysql_num_fields(_res);
        _fields = mysql_fetch_fields(_res);

        for (size_t i = 0; i < _numFields; i++) {
            _columnNames.push_back(string(_fields[i].name));
            _name2index[_fields[i].name] = i;
        }
    }
    return shared_from_this();
}

Connection::Ptr Connection::execute(function<void(Connection::Ptr)> const& script, unsigned int maxReconnects,
                                    unsigned int timeoutSec) {
    unsigned int const effectiveMaxReconnects =
            0 != maxReconnects ? maxReconnects : Configuration::databaseMaxReconnects();

    unsigned int const effectiveTimeoutSec =
            0 != timeoutSec ? timeoutSec : Configuration::databaseConnectTimeoutSec();

    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) +
                           ",effectiveMaxReconnects=" + to_string(effectiveMaxReconnects) +
                           ",effectiveTimeoutSec=" + to_string(effectiveTimeoutSec) + ")  ";

    auto conn = shared_from_this();

    unsigned int numReconnects = 0;
    size_t const beginTimeMillisec = util::TimeUtils::now();
    do {
        try {
            LOGS(_log, LOG_LVL_DEBUG, context << "running user script, numReconnects: " << numReconnects);

            script(conn);
            return conn;

        } catch (Reconnected const& ex) {
            LOGS(_log, LOG_LVL_DEBUG, context << "user script failed due to a reconnect");

            // Check for the maximum allowed reconnect limit

            ++numReconnects;
            if (numReconnects > effectiveMaxReconnects) {
                string const msg = context + "aborting script, exceeded effectiveMaxReconnects: " +
                                   to_string(effectiveMaxReconnects);

                LOGS(_log, LOG_LVL_DEBUG, msg);
                throw MaxReconnectsExceeded(msg, effectiveMaxReconnects);
            }
        }

        // Check for timer expiration

        size_t const elapsedTimeMillisec = util::TimeUtils::now() - beginTimeMillisec;
        if (elapsedTimeMillisec / 1000 > effectiveTimeoutSec) {
            string const msg = context + "aborting script, expired effectiveTimeoutSec: " +
                               to_string(effectiveTimeoutSec) +
                               ", elapsedTimeSec: " + to_string(elapsedTimeMillisec / 1000);

            LOGS(_log, LOG_LVL_DEBUG, msg);
            throw ConnectTimeoutError(msg, effectiveTimeoutSec);
        }

    } while (true);
}

Connection::Ptr Connection::executeInOwnTransaction(function<void(Ptr)> const& script,
                                                    unsigned int maxReconnects, unsigned int timeoutSec,
                                                    unsigned int maxRetriesOnDeadLock) {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) +
                           ",maxRetriesOnDeadLock=" + to_string(maxRetriesOnDeadLock) + ")  ";

    try {
        unsigned int numRetriesOnDeadLock = 0;
        do {
            LOGS(_log, LOG_LVL_DEBUG,
                 context << "running user script, numRetriesOnDeadLock: " << numRetriesOnDeadLock);
            try {
                return execute(
                        [&script](Connection::Ptr const& conn) {
                            conn->begin();
                            script(conn);
                            conn->commit();
                        },
                        maxReconnects, timeoutSec);
            } catch (ER_LOCK_DEADLOCK_ const& ex) {
                if (inTransaction()) rollback();
                if (numRetriesOnDeadLock < maxRetriesOnDeadLock) {
                    LOGS(_log, LOG_LVL_DEBUG, context << "exception: " << ex.what());
                    ++numRetriesOnDeadLock;
                } else {
                    LOGS(_log, LOG_LVL_DEBUG,
                         context << "maximum number of retries " << maxRetriesOnDeadLock
                                 << " for avoiding deadlocks on a table has been reached."
                                 << " Aborting the script.");
                    throw;
                }
            }
        } while (true);
    } catch (...) {
        if (inTransaction()) rollback();
        throw;
    }
    return shared_from_this();
}

Connection::Ptr Connection::executeInsertOrUpdate(function<void(Ptr)> const& insertScript,
                                                  function<void(Ptr)> const& updateScript,
                                                  unsigned int maxReconnects, unsigned int timeoutSec,
                                                  unsigned int maxRetriesOnDeadLock) {
    try {
        return executeInOwnTransaction(insertScript, maxReconnects, timeoutSec, maxRetriesOnDeadLock);
    } catch (database::mysql::ER_DUP_ENTRY_ const&) {
        return executeInOwnTransaction(updateScript, maxReconnects, timeoutSec, maxRetriesOnDeadLock);
    }
}

unsigned int Connection::warningCount() { return mysql_warning_count(_mysql); }

list<Warning> Connection::warnings(unsigned int maxNumWarnings, unsigned int offset) {
    list<Warning> result;
    if (mysql_warning_count(_mysql) == 0) return result;
    QueryGenerator const g(shared_from_this());
    execute(g.warnings() + g.limit(maxNumWarnings, offset));
    Row row;
    while (next(row)) {
        Warning w;
        row.get("Level", w.level);
        row.get("Code", w.code);
        row.get("Message", w.message);
        result.push_back(move(w));
    }
    return result;
}

uint64_t Connection::affectedRows() { return mysql_affected_rows(_mysql); }

bool Connection::hasResult() const { return _mysql and _res; }

vector<string> const& Connection::columnNames() const {
    _assertQueryContext();
    return _columnNames;
}

size_t Connection::numFields() const {
    _assertQueryContext();
    return _numFields;
}

void Connection::exportField(ProtocolResponseSqlField* ptr, size_t idx) const {
    _assertQueryContext();

    string const context = "Connection::" + string(__func__) + "  idx: " + to_string(idx) + " range: [0," +
                           to_string(_numFields) + "]  ";

    if (idx >= _numFields) {
        throw out_of_range(context + " error: index is out of range");
    }
    auto&& field = _fields[idx];
    ptr->set_name(field.name);
    ptr->set_org_name(field.org_name);
    ptr->set_table(field.table);
    ptr->set_org_table(field.org_table);
    ptr->set_db(field.db);
    ptr->set_catalog(field.catalog);
    ptr->set_def(field.def);
    ptr->set_length(field.length);
    ptr->set_max_length(field.max_length);
    ptr->set_flags(field.flags);
    ptr->set_decimals(field.decimals);
    ptr->set_type(field.type);
}

json Connection::fieldsToJson() const {
    _assertQueryContext();

    json result;
    for (size_t i = 0; i < _numFields; ++i) {
        auto&& field = _fields[i];
        json f;
        f["name"] = field.name;
        f["org_name"] = field.org_name;
        f["table"] = field.table;
        f["org_table"] = field.org_table;
        f["db"] = field.db;
        f["catalog"] = field.catalog;
        f["def"] = field.def;
        f["length"] = field.length;
        f["max_length"] = field.max_length;
        f["flags"] = field.flags;
        f["decimals"] = field.decimals;
        f["type"] = field.type;
        result.push_back(move(f));
    }
    return result;
}

bool Connection::next(Row& row) {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + ")  ";

    _assertQueryContext();

    _row = mysql_fetch_row(_res);
    if (not _row) {
        // Just no more rows is no specific error reported
        if (0 == mysql_errno(_mysql)) return false;

        _processLastError(context + "mysql_fetch_row failed, query: '" + _lastQuery + "'");
    }
    size_t const* lengths = mysql_fetch_lengths(_res);

    // Transfer the data pointers for each field and their lengths into
    // the provided Row object.

    row._name2indexPtr = &_name2index;
    row._index2cell.clear();

    row._index2cell.reserve(_numFields);
    for (size_t i = 0; i < _numFields; ++i) {
        row._index2cell.emplace_back(Row::Cell{_row[i], lengths[i]});
    }
    return true;
}

void Connection::_connect() {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) +
                           ",_connectTimeoutSec=" + to_string(_connectTimeoutSec) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context << __func__ << "  started");

    // Allow just one shot if no reconnects are allewed by setting the timeout
    // to a value greater than 0.

    if (0 == _connectTimeoutSec) {
        _connectOnce();
    } else {
        // Otherwise keep trying before succeeded or the connection timeout
        // expired.

        long timeLapsedMilliseconds = 0;
        util::BlockPost delayBetweenReconnects(1000, 1001);  // ~1 second

        while (true) {
            try {
                _connectOnce();
                break;
            } catch (ConnectError const& ex) {
                LOGS(_log, LOG_LVL_DEBUG, context << "connection attempt failed: " << ex.what());

                // Delay another connection attempt and check if the timer has expired

                timeLapsedMilliseconds += delayBetweenReconnects.wait();
                if (timeLapsedMilliseconds > 1000 * _connectTimeoutSec) {
                    string const msg = context + "connection timeout has expired";
                    LOGS(_log, LOG_LVL_DEBUG, msg);
                    throw ConnectTimeoutError(msg, _connectTimeoutSec);
                }
            }
        }
    }
    LOGS(_log, LOG_LVL_DEBUG, context << __func__ << "  connected");
}

void Connection::_connectOnce() {
    ++_connectionAttempt;

    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) +
                           ",_connectionAttempt=" + to_string(_connectionAttempt) + ")  ";

    LOGS(_log, LOG_LVL_DEBUG, context);

    // Clean up a context of the previous connection (if any)

    _inTransaction = false;
    _columnNames.clear();
    _name2index.clear();

    if (nullptr != _mysql) {
        if (nullptr != _res) mysql_free_result(_res);

        _res = nullptr;
        _fields = nullptr;
        _numFields = 0;

        mysql_close(_mysql);
        _mysql = nullptr;
    }

    // Prepare the connection object

    if (not(_mysql = mysql_init(_mysql))) {
        throw Error(context + "mysql_init failed");
    }

    // Only allow TCP/IP, no UNIX sockets for now
    enum mysql_protocol_type const prot_type = MYSQL_PROTOCOL_TCP;
    mysql_options(_mysql, MYSQL_OPT_PROTOCOL, &prot_type);

    // This is required by 'LOAD DATA LOCAL INFILE ...' to allow ingesting files which
    // are not directly seen by the MySQL server. The 'LOCAL' option would make a file
    // local to a client opening this connection to be transferred to some temporary
    // directory owned by the server. After that the file will get ingested into
    // the destination table.
    //
    // NOTES:
    // - The server must be configured with global variable 'local_infile=1'
    // - The temporary folder managed by the server is required to have enough space
    //   to accommodate files received from the client.
    unsigned int const enableLocalInfile = 1;
    mysql_options(_mysql, MYSQL_OPT_LOCAL_INFILE, &enableLocalInfile);
    mysql_set_local_infile_default(_mysql);

    // Make a connection attempt

    if (nullptr ==
        mysql_real_connect(_mysql, _connectionParams.host.empty() ? nullptr : _connectionParams.host.c_str(),
                           _connectionParams.user.empty() ? nullptr : _connectionParams.user.c_str(),
                           _connectionParams.password.empty() ? nullptr : _connectionParams.password.c_str(),
                           _connectionParams.database.empty() ? nullptr : _connectionParams.database.c_str(),
                           _connectionParams.port, 0, /* no default UNIX socket */
                           0) /* no default client flag */) {
        bool const instantAutoReconnect = false;
        _processLastError(context + "mysql_real_connect() failed", instantAutoReconnect);
    }

    // Update the current connection identifier, and if reconnecting then also
    // tell MySQL to kill the previous thread to ensure any on-going transaction
    // is aborted and no tables are still locked.
    //
    // NOTE: ignore result of the "KILL <thread-id>" query because we're making
    //       our best attempt to clear the previous context. And chances are that
    //       the server has already disposed that thread.

    unsigned long const id = _mysqlThreadId;
    _mysqlThreadId = mysql_thread_id(_mysql);

    if ((0 != id) and (id != _mysqlThreadId)) {
        string const query = "KILL " + to_string(id);
        mysql_query(_mysql, query.c_str());
    }

    // Get the default character set name

    _charSetName = mysql_character_set_name(_mysql);

    // Set session attributes

    vector<string> queries;

    // The change is related to the following bug in MariaDB before 10.2.8:
    //    https://jira.mariadb.org/browse/MDEV-16792
    //
    // This bug causes problems with partitioned tables if partitions are created
    // with SQL_MODE='ANSI'. This mode sets 'ANSI_QUOTES` (double quotes for identifiers).
    // Details are in:
    //   https://dev.mysql.com/doc/refman/5.7/en/sql-mode.html#sqlmode_ansi_quotes
    //
    // The problem is seen in clients which aren't setting this mode. Hence a workaround
    // is to either keep this mode here or to set that mode in all clients (Qserv).
    // The current solution is to disable this:
    //
    //   queries.push_back("SET SESSION SQL_MODE='ANSI'");

    queries.push_back("SET SESSION AUTOCOMMIT=0");
    //
    // TODO: Reconsider this because it won't work in the modern versions
    //       of MySQL/MariaDB. Perhaps an opposite operation of pulling
    //       the parameter's value from the server would make more sense here.
    //
    // queries.push_back("SET SESSION max_allowed_packet=" + to_string(max_allowed_packet()));

    for (auto&& query : queries) {
        if (0 != mysql_query(_mysql, query.c_str())) {
            throw Error(context + "mysql_query() failed in query:" + query +
                        ", error: " + string(mysql_error(_mysql)));
        }
    }

    // Note that this counters is meant to count unsuccessful connection attempts
    // before a good connection is established.
    _connectionAttempt = 0;
}

void Connection::_assertQueryContext() const {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) + ")  ";

    LOGS(_log, LOG_LVL_TRACE, context);

    if (_mysql == nullptr) throw Error(context + "not connected to the MySQL service");
    if (_res == nullptr) throw Error(context + "no prior query made");
}

void Connection::_assertTransaction(bool inTransaction) const {
    string const context = "Connection[" + to_string(_id) + "]::" + string(__func__) +
                           "(_inTransaction=" + to_string(_inTransaction ? 1 : 0) +
                           ",inTransaction=" + to_string(inTransaction ? 1 : 0) + ")  ";

    LOGS(_log, LOG_LVL_TRACE, context);

    if (inTransaction != _inTransaction) {
        throw logic_error(context + "the transaction is" + string(_inTransaction ? " " : " not") + " active");
    }
}

ConnectionPool::Ptr ConnectionPool::create(ConnectionParams const& params, size_t maxConnections) {
    return Ptr(new ConnectionPool(params, maxConnections));
}

ConnectionPool::ConnectionPool(ConnectionParams const& params, size_t maxConnections)
        : _params(params), _maxConnections(maxConnections) {}

Connection::Ptr ConnectionPool::allocate() {
    string const context = "ConnectionPool::" + string(__func__) + "  ";

    LOGS(_log, LOG_LVL_TRACE, context);

    unique_lock<mutex> lock(_mtx);

    if (_availableConnections.empty()) {
        // Open a new connection and return it right away if the limit hasn't
        // been reached yet.
        //
        // TODO: the factory method called below may put a calling thread
        // in the blocking state while the (database) service becomes available.
        // This will prevent operations with the pool by other threads. Investigate
        // a non-blocking algorithm.

        if (_availableConnections.size() + _usedConnections.size() < _maxConnections) {
            auto conn = Connection::open(_params);
            _usedConnections.push_back(conn);
            return conn;
        }

        // Otherwise grab an existing one (which may require to wait before
        // it would become available).

        _available.wait(lock, [&]() { return not _availableConnections.empty(); });
    }
    Connection::Ptr conn = _availableConnections.front();
    _availableConnections.pop_front();
    _usedConnections.push_back(conn);

    return conn;
}

void ConnectionPool::release(Connection::Ptr const& conn) {
    string const context = "ConnectionPool::" + string(__func__) + "  ";

    LOGS(_log, LOG_LVL_TRACE, context);

    unique_lock<mutex> lock(_mtx);

    // Move it between queues.

    size_t numRemoved = 0;
    _usedConnections.remove_if([&numRemoved, &conn](Connection::Ptr const& ptr) {
        if (ptr == conn) {
            numRemoved++;
            return true;
        }
        return false;
    });
    if (1 != numRemoved) {
        throw logic_error(context + "inappropriate use of the method");
    }
    _availableConnections.push_back(conn);

    // Notify one client (if any) waiting for a service

    lock.unlock();
    _available.notify_one();
}

ConnectionHandler::ConnectionHandler(Connection::Ptr const& conn_) : conn(conn_) {}

ConnectionHandler::ConnectionHandler(ConnectionPool::Ptr const& pool) : conn(pool->allocate()), _pool(pool) {}

ConnectionHandler::~ConnectionHandler() {
    string const context = "ConnectionHandler::" + string(__func__) + "  ";
    try {
        if ((nullptr != conn) and conn->inTransaction()) {
            conn->rollback();
        }
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_TRACE, context << "ex: " << ex.what());
    }
    if (nullptr != _pool) _pool->release(conn);
}

}  // namespace lsst::qserv::replica::database::mysql
