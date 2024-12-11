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
#ifndef LSST_QSERV_REPLICA_PROTOCOL_H
#define LSST_QSERV_REPLICA_PROTOCOL_H

// System headers
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// This header declarations
namespace lsst::qserv::replica::protocol {

/// Subtypes of the SQL requests.
enum class SqlRequestType : int {

    QUERY = 0,
    CREATE_DATABASE = 1,
    DROP_DATABASE = 2,
    ENABLE_DATABASE = 3,   ///< in Qserv
    DISABLE_DATABASE = 4,  ///< in Qserv
    GRANT_ACCESS = 5,
    CREATE_TABLE = 6,
    DROP_TABLE = 7,
    REMOVE_TABLE_PARTITIONING = 8,
    DROP_TABLE_PARTITION = 9,
    GET_TABLE_INDEX = 10,
    CREATE_TABLE_INDEX = 11,
    DROP_TABLE_INDEX = 12,
    ALTER_TABLE = 13,
    TABLE_ROW_STATS = 14
};

/// @return the string representation of the SQL request type
std::string toString(SqlRequestType status);

/// Types of the table indexes specified in the index management requests requests.
enum class SqlIndexSpec : int { DEFAULT = 1, UNIQUE = 2, FULLTEXT = 3, SPATIAL = 4 };

/// Status values returned by all request related to operations with
/// replicas. Request management operations always return messages whose types
/// match the return types of the corresponding (original) replica-related requests.
/// Service management requests have their own set of status values.
///
enum class Status : int {
    CREATED = 0,
    SUCCESS = 1,
    QUEUED = 2,
    IN_PROGRESS = 3,
    IS_CANCELLING = 4,
    BAD = 5,
    FAILED = 6,
    CANCELLED = 7
};

enum class StatusExt : int {
    NONE = 0,                    ///< Unspecified problem.
    INVALID_PARAM = 1,           ///< Invalid parameter(s) of a request.
    INVALID_ID = 2,              ///< An invalid request identifier.
    FOLDER_STAT = 4,             ///< Failed to obtain fstat() for a folder.
    FOLDER_CREATE = 5,           ///< Failed to create a folder.
    FILE_STAT = 6,               ///< Failed to obtain fstat() for a file.
    FILE_SIZE = 7,               ///< Failed to obtain a size of a file.
    FOLDER_READ = 8,             ///< Failed to read the contents of a folder.
    FILE_READ = 9,               ///< Failed to read the contents of a file.
    FILE_ROPEN = 10,             ///< Failed to open a remote file.
    FILE_CREATE = 11,            ///< Failed to create a file.
    FILE_OPEN = 12,              ///< Failed to open a file.
    FILE_RESIZE = 13,            ///< Failed to resize a file.
    FILE_WRITE = 14,             ///< Failed to write into a file.
    FILE_COPY = 15,              ///< Failed to copy a file.
    FILE_DELETE = 16,            ///< Failed to delete a file.
    FILE_RENAME = 17,            ///< Failed to rename a file.
    FILE_EXISTS = 18,            ///< File already exists.
    SPACE_REQ = 19,              ///< Space availability check failed.
    NO_FOLDER = 20,              ///< Folder doesn't exist.
    NO_FILE = 21,                ///< File doesn't exist.
    NO_ACCESS = 22,              ///< No access to a file or a folder.
    NO_SPACE = 23,               ///< No space left on a device as required by an operation.
    FILE_MTIME = 24,             ///< Get/set 'mtime' operation failed.
    MYSQL_ERROR = 25,            ///< General MySQL error (other than any specific ones listed here).
    LARGE_RESULT = 26,           ///< Result exceeds a limit set in a request.
    NO_SUCH_TABLE = 27,          ///< No table found while performing a MySQL operation.
    NOT_PARTITIONED_TABLE = 28,  ///< The table is not MySQL partitioned as it was expected.
    NO_SUCH_PARTITION = 29,      ///< No MySQL partition found in a table as it was expected.
    MULTIPLE = 30,               ///< Multiple unspecified errors encountered when processing a request.
    OTHER_EXCEPTION = 31,        ///< Other exception not listed here.
    FOREIGN_INSTANCE = 32,       ///< Detected a request from a Controller serving an unrelated Qserv.
    DUPLICATE_KEY = 33,          ///< Duplicate key found when creating an index or altering a table schema.
    CANT_DROP_KEY = 34           ///< Can't drop a field or a key which doesn't exist.
};

/// @return the string representation of the status
std::string toString(Status status);

/// @return the string representation of the extended status
std::string toString(StatusExt extendedStatus);

/// @return the string representation of the full status
std::string toString(Status status, StatusExt extendedStatus);

/// Status of a service.
enum class ServiceState : int { SUSPEND_IN_PROGRESS = 0, SUSPENDED = 1, RUNNING = 2 };

/// @return the string representation of the service state
std::string toString(ServiceState state);

/// The header to be sent with the requests processed through the worker's queueing system.
struct QueuedRequestHdr {
    std::string id;
    int priority;
    unsigned int timeout;
    QueuedRequestHdr(std::string const& id_, int priority_, unsigned int timeout_)
            : id(id_), priority(priority_), timeout(timeout_) {}
    nlohmann::json toJson() const { return {{"id", id}, {"priority", priority}, {"timeout", timeout}}; };
};

}  // namespace lsst::qserv::replica::protocol

#endif  // LSST_QSERV_REPLICA_PROTOCOL_H
