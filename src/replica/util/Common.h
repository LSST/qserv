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
#ifndef LSST_QSERV_REPLICA_COMMON_H
#define LSST_QSERV_REPLICA_COMMON_H

/**
 * This declares various small utilities, such data types, functions and
 * classes which are shared by the code in the rest of this package.
 * It would be not practical to put each of these utilities in a separate
 * header.
 */

// System headers
#include <cstdint>
#include <list>
#include <ostream>
#include <string>
#include <tuple>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "replica/util/Mutex.h"

// This header declarations
namespace lsst::qserv::replica {

/// The number of the 'overflow' chunks
unsigned int const overflowChunkNumber = 1234567890;

// Standard priorities for requests and jobs. Activities that can be run in background
// should be started with the lowest priority. Urgent operations may need to assume
// one of the high priority levels. The default priority level should be set
// to PRIORITY_NORMAL.

int const PRIORITY_VERY_LOW = 1;
int const PRIORITY_LOW = 2;
int const PRIORITY_NORMAL = 3;
int const PRIORITY_HIGH = 4;
int const PRIORITY_VERY_HIGH = 5;

/// @return The string representation of the extended status.
std::string status2string(ProtocolStatusExt status);

/// The chunk overlap selector is used where the tri-state is required.
enum class ChunkOverlapSelector : int { CHUNK = 1, OVERLAP = 2, CHUNK_AND_OVERLAP = 3 };

/// @param selector The selector to be translated.
/// @return The string representation of the selector.
/// @throw std::invalid_argument If the selector is not valid.
std::string overlapSelector2str(ChunkOverlapSelector selector);

std::ostream& operator<<(std::ostream& os, ChunkOverlapSelector selector);

/// @param str The input string to be parsed.
/// @return ChunkOverlapSelector The selector's value.
/// @throw std::invalid_argument If the string doesn't match any value.
ChunkOverlapSelector str2overlapSelector(std::string const& str);

/**
 * Class Generators is the utility class for generating a set of unique
 * identifiers, etc. Each call to the class's method 'next()' will produce
 * a new identifier.
 */
class Generators {
public:
    /// @return next unique identifier
    static std::string uniqueId();

private:
    /// For thread safety where it's required
    static replica::Mutex _mtx;
};

/**
 * This class is an abstraction for column definitions. A column has
 * a name and a type.
 */
class SqlColDef {
public:
    SqlColDef() = default;
    SqlColDef(std::string const name_, std::string const type_) : name(name_), type(type_) {}
    SqlColDef(SqlColDef const&) = default;
    SqlColDef& operator=(SqlColDef const&) = default;
    ~SqlColDef() = default;

    std::string name;
    std::string type;
};

inline bool operator==(SqlColDef const& lhs, SqlColDef const& rhs) {
    return (lhs.name == rhs.name) && (lhs.type == rhs.type);
}

inline bool operator!=(SqlColDef const& lhs, SqlColDef const& rhs) { return !operator==(lhs, rhs); }

/**
 * @param columnsJsonArray The JSON array containing the column definitions.
 * @return The list of column definitions.
 * @throw std::invalid_argument If the input JSON array is not valid.
 */
std::list<SqlColDef> parseSqlColumns(nlohmann::json const& columnsJsonArray);

/**
 * This class is an abstraction for columns within table index
 * specifications.
 */
class SqlIndexColumn {
public:
    SqlIndexColumn() = default;
    SqlIndexColumn(std::string const name_, size_t length_, bool ascending_)
            : name(name_), length(length_), ascending(ascending_) {}
    SqlIndexColumn(SqlIndexColumn const&) = default;
    SqlIndexColumn& operator=(SqlIndexColumn const&) = default;
    ~SqlIndexColumn() = default;

    std::string name;
    size_t length = 0;
    bool ascending = true;
};

/**
 * This class is an abstraction for the index definitions.
 */
class SqlIndexDef {
public:
    SqlIndexDef() = default;

    /**
     * Parse the definition from then input JSON object.
     * @param indexSpecJson The JSON object containing the index definitions.
     * @throw std::invalid_argument If the input JSON object is not valid.
     */
    SqlIndexDef(nlohmann::json const& indexSpecJson);

    SqlIndexDef(SqlIndexDef const&) = default;
    SqlIndexDef& operator=(SqlIndexDef const&) = default;
    ~SqlIndexDef() = default;

    std::string spec;
    std::string name;
    std::string comment;
    std::list<std::tuple<std::string, unsigned int, bool>> keys;
};

/**
 * Class ReplicationRequestParams encapsulates parameters of the replica
 * creation requests.
 */
class ReplicationRequestParams {
public:
    std::string database;
    unsigned int chunk = 0;
    std::string sourceWorker;
    std::string sourceWorkerHost;
    uint16_t sourceWorkerPort;
    std::string sourceWorkerDataDir;

    ReplicationRequestParams() = default;

    explicit ReplicationRequestParams(ProtocolRequestReplicate const& request);
};

/**
 * Class DeleteRequestParams represents parameters of the replica
 * deletion requests.
 */
class DeleteRequestParams {
public:
    std::string database;
    unsigned int chunk = 0;
    std::string sourceWorker;

    DeleteRequestParams() = default;

    explicit DeleteRequestParams(ProtocolRequestDelete const& request);
};

/**
 * Class FindRequestParams represents parameters of a single replica
 * lookup (finding) requests.
 */
class FindRequestParams {
public:
    std::string database;
    unsigned int chunk = 0;

    FindRequestParams() = default;

    explicit FindRequestParams(ProtocolRequestFind const& request);
};

/**
 * Class FindAllRequestParams represents parameters of the replica
 * group (depends on a scope of the corresponding request) lookup (finding)
 * requests.
 */
class FindAllRequestParams {
public:
    std::string database;

    FindAllRequestParams() = default;

    explicit FindAllRequestParams(ProtocolRequestFindAll const& request);
};

/**
 * Class EchoRequestParams represents parameters of the echo requests.
 */
class EchoRequestParams {
public:
    std::string data;
    uint64_t delay = 0;

    EchoRequestParams() = default;

    explicit EchoRequestParams(ProtocolRequestEcho const& request);
};

/// The type for the super-transaction identifiers
typedef uint32_t TransactionId;

/// The type for event identifiers of the the super-transactions.
typedef uint32_t TransactionEventId;

/**
 * Class SqlRequestParams represents parameters of the SQL requests.
 */
class SqlRequestParams {
public:
    enum Type {
        QUERY,
        CREATE_DATABASE,
        DROP_DATABASE,
        ENABLE_DATABASE,
        DISABLE_DATABASE,
        GRANT_ACCESS,
        CREATE_TABLE,
        DROP_TABLE,
        REMOVE_TABLE_PARTITIONING,
        DROP_TABLE_PARTITION,
        GET_TABLE_INDEX,
        CREATE_TABLE_INDEX,
        DROP_TABLE_INDEX,
        ALTER_TABLE,
        TABLE_ROW_STATS
    };
    Type type = QUERY;

    uint64_t maxRows = 0;

    std::string query;
    std::string user;
    std::string password;
    std::string database;
    std::string table;
    std::string engine;
    std::string partitionByColumn;

    TransactionId transactionId = 0;

    std::list<SqlColDef> columns;

    std::vector<std::string> tables;

    bool batchMode = false;

    /**
     * Class IndexSpec is an abstraction for the index type specification.
     *
     * It's been designed to allow constructing specifications from a string
     * or a Protobuf representations. The class contract also allows a reverse
     * translation into either of those representations.
     */
    class IndexSpec {
    public:
        /**
         * Construct from the Protobuf representation.
         * @throws std::invalid_argument If the input specification is not supported
         *   by the class.
         */
        IndexSpec(ProtocolRequestSql::IndexSpec spec);

        /**
         * Construct by translate the input string into the internal specification.
         * @throws std::invalid_argument If the input specification is not supported
         *   by the class.
         */
        IndexSpec(std::string const& str);

        IndexSpec() = default;
        IndexSpec(IndexSpec const&) = default;
        IndexSpec& operator=(IndexSpec const&) = default;

        /// @return The string representation.
        std::string str() const;

        /// @return The Protobuf representation.
        ProtocolRequestSql::IndexSpec protocol() const;

    private:
        /// The internal representation
        enum Spec { DEFAULT, UNIQUE, FULLTEXT, SPATIAL };
        Spec _spec = Spec::DEFAULT;
    };
    IndexSpec indexSpec;

    std::string indexName;
    std::string indexComment;

    std::vector<SqlIndexColumn> indexColumns;

    // The constructors

    SqlRequestParams() = default;

    explicit SqlRequestParams(ProtocolRequestSql const& request);

    std::string type2str() const;
};

std::ostream& operator<<(std::ostream& os, SqlRequestParams const& params);

/**
 * Class DirectorIndexRequestParams represents parameters of requests extracting data
 * to be loaded into the "director" index.
 */
class DirectorIndexRequestParams {
public:
    std::string database;
    unsigned int chunk = 0;
    bool hasTransactions = false;
    TransactionId transactionId = 0;

    DirectorIndexRequestParams() = default;

    explicit DirectorIndexRequestParams(ProtocolRequestDirectorIndex const& request);
};

/**
 * An utility function translating a boolean value into a string representation.
 * @param v The input value.
 * @return The result ("0" for "false" and "1" for "true").
 */
inline std::string bool2str(bool v) { return v ? "1" : "0"; }

/**
 * Class Query stores a query and the optional transient synchronization context
 * for the query.
 */
class Query {
public:
    Query() = default;
    Query(Query const&) = default;
    Query& operator=(Query const&) = default;
    /// @param query_ A query.
    /// @param mutexName_ The optional name of a mutex to be held before
    ///   executing the query.
    explicit Query(std::string const& query_, std::string const& mutexName_ = std::string())
            : query(query_), mutexName(mutexName_) {}
    ~Query() = default;

    std::string query;
    std::string mutexName;
};

/**
 * @brief Generate the name of a metadata table at czar for the specified data table.
 * @param databaseName The name of a database where the data table is residing.
 * @param tableName The name of the data table.
 * @param suffix The optional suffix for the metadata table.
 * @return std::string The name of the metadata table at czar.
 * @throws std::invalid_argument If the length of the resulting name exceeds the MySQL limit.
 */
std::string tableNameBuilder(std::string const& databaseName, std::string const& tableName,
                             std::string const& suffix = std::string());

/// @return The name of the "director" index table
inline std::string directorIndexTableName(std::string const& databaseName,
                                          std::string const& directorTableName) {
    return tableNameBuilder(databaseName, directorTableName);
}

/// @return The name of a table at czar that stores table row counters of the specified data table.
inline std::string rowCountersTable(std::string const& databaseName, std::string const& tableName) {
    return tableNameBuilder(databaseName, tableName, "__rows");
}

template <typename T>
inline void throwIf(bool condition, std::string const& message) {
    if (condition) throw T(message);
}

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_COMMON_H
