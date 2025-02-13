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
#include "replica/worker/WorkerDirectorIndexHttpRequest.h"

// System headers
#include <algorithm>
#include <cerrno>
#include <iostream>
#include <stdexcept>

// Third party headers
#include "boost/filesystem.hpp"

// Qserv headers
#include "global/constants.h"
#include "replica/config/Configuration.h"
#include "replica/mysql/DatabaseMySQL.h"
#include "replica/proto/Protocol.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/Performance.h"
#include "util/String.h"

// LSST headers
#include "lsst/log/Log.h"

#define CONTEXT context("WorkerDirectorIndexHttpRequest", __func__)

using namespace std;
namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerDirectorIndexHttpRequest");

}  // namespace

namespace lsst::qserv::replica {

using namespace database::mysql;

shared_ptr<WorkerDirectorIndexHttpRequest> WorkerDirectorIndexHttpRequest::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired,
        shared_ptr<database::mysql::ConnectionPool> const& connectionPool) {
    auto ptr = shared_ptr<WorkerDirectorIndexHttpRequest>(
            new WorkerDirectorIndexHttpRequest(serviceProvider, worker, hdr, req, onExpired, connectionPool));
    ptr->init();
    return ptr;
}

WorkerDirectorIndexHttpRequest::WorkerDirectorIndexHttpRequest(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired,
        shared_ptr<database::mysql::ConnectionPool> const& connectionPool)
        : WorkerHttpRequest(serviceProvider, worker, "INDEX", hdr, req, onExpired),
          _databaseInfo(serviceProvider->config()->databaseInfo(req.at("database"))),
          _tableInfo(_databaseInfo.findTable(req.at("director_table"))),
          _hasTransactions(req.at("has_transaction")),
          _transactionId(req.at("transaction_id")),
          _chunk(req.at("chunk")),
          _offset(req.at("offset")),
          _connectionPool(connectionPool),
          _tmpDirName(serviceProvider->config()->get<string>("worker", "loader-tmp-dir") + "/" +
                      _databaseInfo.name),
          _fileName(_tmpDirName + "/" + _tableInfo.name + "-" + to_string(_chunk) +
                    (_hasTransactions ? "-p" + to_string(_transactionId) : "") + "-" + hdr.id) {}

void WorkerDirectorIndexHttpRequest::getResult(json& result) const {
    // No locking is needed here since the method is called only after
    // the request is completed.
    result["error"] = _error;
    result["data"] = util::String::toHex(_data.data(), _data.size());
    result["total_bytes"] = _fileSizeBytes;
}

bool WorkerDirectorIndexHttpRequest::execute() {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT);

    replica::Lock lock(_mtx, CONTEXT);
    checkIfCancelling(lock, CONTEXT);

    try {
        // The table will be scanned only when the offset is set to 0.
        if (_offset == 0) {
            // Create a folder (if it still doesn't exist) where the temporary files will be placed
            // NOTE: this folder is supposed to be seen by the worker's MySQL/MariaDB server, and it
            // must be write-enabled for an account under which the service is run.
            boost::system::error_code ec;
            fs::create_directory(fs::path(_tmpDirName), ec);
            if (ec.value() != 0) {
                _error = "failed to create folder '" + _tmpDirName;
                LOGS(_log, LOG_LVL_ERROR, CONTEXT << " " << _error);
                setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::FOLDER_CREATE);
            }

            // Make sure no file exists from any previous attempt to harvest the index data
            // in a scope of the request. Otherwise MySQL query will fail.
            _removeFile();

            // Connect to the worker database
            // Manage the new connection via the RAII-style handler to ensure the transaction
            // is automatically rolled-back in case of exceptions.
            ConnectionHandler const h(_connectionPool);

            // A scope of the query depends on parameters of the request
            h.conn->executeInOwnTransaction([self = shared_from_base<WorkerDirectorIndexHttpRequest>()](
                                                    auto conn) { conn->execute(self->_query(conn)); });
        }
        if (auto const status = _readFile(_offset); status != protocol::StatusExt::NONE) {
            setStatus(lock, protocol::Status::FAILED, status);
        } else {
            setStatus(lock, protocol::Status::SUCCESS);
        }
    } catch (ER_NO_SUCH_TABLE_ const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " MySQL error: " << ex.what());
        _error = ex.what();
        setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::NO_SUCH_TABLE);
    } catch (database::mysql::ER_PARTITION_MGMT_ON_NONPARTITIONED_ const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " MySQL error: " << ex.what());
        _error = ex.what();
        setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::NOT_PARTITIONED_TABLE);
    } catch (database::mysql::ER_UNKNOWN_PARTITION_ const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " MySQL error: " << ex.what());
        _error = ex.what();
        setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::NO_SUCH_PARTITION);
    } catch (database::mysql::Error const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " MySQL error: " << ex.what());
        _error = ex.what();
        setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::MYSQL_ERROR);
    } catch (invalid_argument const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " exception: " << ex.what());
        _error = ex.what();
        setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::INVALID_PARAM);
    } catch (out_of_range const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " exception: " << ex.what());
        _error = ex.what();
        setStatus(lock, protocol::Status::FAILED, protocol::StatusExt::LARGE_RESULT);
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " exception: " << ex.what());
        _error = "Exception: " + string(ex.what());
        setStatus(lock, protocol::Status::FAILED);
    }
    return true;
}

string WorkerDirectorIndexHttpRequest::_query(shared_ptr<database::mysql::Connection> const& conn) const {
    if (!_tableInfo.isDirector()) {
        throw invalid_argument("table '" + _tableInfo.name +
                               "' is not been configured as director in database '" + _databaseInfo.name +
                               "'");
    }
    if (_tableInfo.directorTable.primaryKeyColumn().empty()) {
        throw invalid_argument("director table '" + _tableInfo.name +
                               "' has not been properly configured in database '" + _databaseInfo.name + "'");
    }
    if (_tableInfo.columns.empty()) {
        throw invalid_argument("no schema found for director table '" + _tableInfo.name + "' of database '" +
                               _databaseInfo.name + "'");
    }

    // Find types required by the "director" index table's columns

    string const qservTransId = _hasTransactions ? "qserv_trans_id" : string();
    string qservTransIdType;
    string primaryKeyColumnType;
    string subChunkIdColNameType;

    for (auto&& column : _tableInfo.columns) {
        if (!qservTransId.empty() && column.name == qservTransId)
            qservTransIdType = column.type;
        else if (column.name == _tableInfo.directorTable.primaryKeyColumn())
            primaryKeyColumnType = column.type;
        else if (column.name == lsst::qserv::SUB_CHUNK_COLUMN)
            subChunkIdColNameType = column.type;
    }
    if ((!qservTransId.empty() && qservTransIdType.empty()) || primaryKeyColumnType.empty() or
        subChunkIdColNameType.empty()) {
        throw invalid_argument(
                "column definitions for the Object identifier or sub-chunk identifier"
                " columns are missing in the director table schema for table '" +
                _tableInfo.name + "' of database '" + _databaseInfo.name + "'");
    }

    // NOTE: injecting the chunk number into each row of the result set because
    // the chunk-id column is optional.
    QueryGenerator const g(conn);
    DoNotProcess const chunk = g.val(_chunk);
    SqlId const sqlTableId = g.id(_databaseInfo.name, _tableInfo.name + "_" + to_string(_chunk));
    string query;
    if (qservTransId.empty()) {
        query = g.select(_tableInfo.directorTable.primaryKeyColumn(), chunk, lsst::qserv::SUB_CHUNK_COLUMN) +
                g.from(sqlTableId) + g.orderBy(make_pair(_tableInfo.directorTable.primaryKeyColumn(), ""));
    } else {
        query = g.select(qservTransId, _tableInfo.directorTable.primaryKeyColumn(), chunk,
                         lsst::qserv::SUB_CHUNK_COLUMN) +
                g.from(sqlTableId) + g.inPartition(g.partId(_transactionId)) +
                g.orderBy(make_pair(qservTransId, ""),
                          make_pair(_tableInfo.directorTable.primaryKeyColumn(), ""));
    }
    return query + g.intoOutfile(_fileName);
}

protocol::StatusExt WorkerDirectorIndexHttpRequest::_readFile(size_t offset) {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT);

    // Open the the file.
    ifstream f(_fileName, ios::binary);
    if (!f.good()) {
        _error = "failed to open file '" + _fileName + "'";
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " " << _error);
        return protocol::StatusExt::FILE_ROPEN;
    }

    // Get the file size.
    boost::system::error_code ec;
    _fileSizeBytes = fs::file_size(_fileName, ec);
    if (ec.value() != 0) {
        _error = "failed to get file size '" + _fileName + "'";
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " " << _error);
        return protocol::StatusExt::FILE_SIZE;
    }

    // Validate a value of the offset and position indicator as requested.
    if (offset == _fileSizeBytes) {
        _removeFile();
        return protocol::StatusExt::NONE;
    } else if (offset > _fileSizeBytes) {
        _error = "attempted to read the file '" + _fileName + "' at the offset " + to_string(offset) +
                 " that is beyond the file size of " + to_string(_fileSizeBytes) + " bytes.";
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " " << _error);
        return protocol::StatusExt::INVALID_PARAM;
    } else if (offset != 0) {
        f.seekg(offset, ios::beg);
    }

    // Resize the memory buffer for the efficiency of the following read.
    size_t const recordSize =
            std::min(_fileSizeBytes - offset,
                     serviceProvider()->config()->get<size_t>("worker", "director-index-record-size"));
    _data.resize(recordSize, ' ');

    // Read the specified number of bytes into the buffer.
    protocol::StatusExt result = protocol::StatusExt::NONE;
    f.read(&_data[0], recordSize);
    if (f.bad()) {
        _error = "failed to read " + to_string(recordSize) + " bytes from the file '" + _fileName +
                 "' at the offset " + to_string(offset) + ".";
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " " << _error);
        result = protocol::StatusExt::FILE_READ;
    }
    f.close();

    // If this was the last record read from the file then delete the file.
    if (offset + recordSize >= _fileSizeBytes) {
        _removeFile();
    }
    return result;
}

void WorkerDirectorIndexHttpRequest::_removeFile() const {
    // Make the best attempt to get rid of the temporary file. Ignore any errors
    // for now. Just report them. Note that 'remove_all' won't complain if the file
    // didn't exist.
    boost::system::error_code ec;
    fs::remove_all(fs::path(_fileName), ec);
    if (ec.value() != 0) {
        LOGS(_log, LOG_LVL_WARN, CONTEXT << " failed to remove the temporary file '" << _fileName);
    }
}

}  // namespace lsst::qserv::replica
