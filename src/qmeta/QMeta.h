/*
 * LSST Data Management System
 * Copyright 2015 LSST Corporation.
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
#ifndef LSST_QSERV_QMETA_QMETA_H
#define LSST_QSERV_QMETA_QMETA_H

// System headers
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Qserv headers
#include "global/clock_defs.h"
#include "global/intTypes.h"
#include "qmeta/QInfo.h"
#include "qmeta/QStats.h"
#include "qmeta/types.h"

namespace lsst::qserv::qdisp {
class QueryMessage;
}  // namespace lsst::qserv::qdisp

namespace lsst::qserv::qmeta {

class MessageStore;

/// @addtogroup qmeta

/**
 * The structure ChunkMap encapsulates a disposition of chunks at Qserv workers
 * along with a time when the map was updated.
 *
 * Here is an example on how to using the map for getting info on all chunks in
 * the given context:
 * @code
 *   std::string const worker   = "worker-001";
 *   std::string const database = "LSST-DR01";
 *   std::string const table    = "Object";
 *
 *   ChunkMap const& chunkMap = ...;
 *   for (auto const& [chunk, size] : chunkMap[worker][database][table]) {
 *       ...
 *   }
 * @endcode
 */
struct QMetaChunkMap {
    /// @return 'true' if the map is empty (or constructed using the default constructor)
    bool empty() const {
        return workers.empty() || (std::chrono::time_point<std::chrono::system_clock>() == updateTime);
    }

    // NOTE: Separate types were added here for the sake of clarity to avoid
    // a definition of the unreadable nested map.

    struct ChunkInfo {
        unsigned int chunk = 0;  ///< The chunk number
        size_t size = 0;         ///< The file size (in bytes) of the chunk table
    };
    typedef std::vector<ChunkInfo> Chunks;             ///< Collection of chunks
    typedef std::map<std::string, Chunks> Tables;      ///< tables-to-chunks
    typedef std::map<std::string, Tables> Databases;   ///< Databases-to-tables
    typedef std::map<std::string, Databases> Workers;  ///< Workers-to-databases

    /// The chunk disposition map for all workers.
    Workers workers;

    /// The last time the map was updated (since UNIX Epoch).
    TIMEPOINT updateTime;
};

/**
 *  @ingroup qmeta
 *  @brief Interface for query metadata.
 */

class QMeta {
public:
    /**
     *  Type for representing the list of tables, first item in pair is
     *  database name, second is table name.
     */
    typedef std::vector<std::pair<std::string, std::string> > TableNames;

    /**
     *  Create QMeta instance from configuration dictionary.
     *
     *  Accepts dictionary containing all needed parameters, there is one
     *  required key "technology" in the dictionary, all other keys depend
     *  on the value of "technology" key. Here are possible values:
     *   'mysql': other keys (all optional):
     *       'hostname': string with mysql server host name or IP address
     *       'port': port number of mysql server (encoded as string)
     *       'socket': unix socket name
     *       'username': mysql user name
     *       'password': user password
     *       'database': database name
     *
     *  @param config:  configuration map
     *
     * @throws ConfigError: if config map is invalid
     * @throws CssError: for all CSS errors
     */
    static std::shared_ptr<QMeta> createFromConfig(std::map<std::string, std::string> const& config);

    // Instances cannot be copied
    QMeta(QMeta const&) = delete;
    QMeta& operator=(QMeta const&) = delete;

    // Destructor
    virtual ~QMeta() {}

    /**
     *  @brief Return czar ID given czar "name"
     *
     *  Negative number is returned if czar does not exist.
     *
     *  @param name:  Czar name, arbitrary string.
     *  @return: Czar ID, zero if czar does not exist.
     */
    virtual CzarId getCzarID(std::string const& name) = 0;

    /**
     *  @brief Register new czar, return czar ID.
     *
     *  If czar with the same name is already registered then its ID
     *  will be returned, otherwise new record will be created.
     *  In both cases the czar will be active after this call.
     *
     *  @param name:  Czar name, arbitrary string.
     *  @return: Czar ID, non-negative number.
     */
    virtual CzarId registerCzar(std::string const& name) = 0;

    /**
     *  @brief Mark specified czar as active or inactive.
     *
     *  This method will throw if czar ID is not known.
     *
     *  @param name:  Czar ID, non-negative number.
     *  @param active:  new value if active flag.
     */
    virtual void setCzarActive(CzarId czarId, bool active) = 0;

    /**
     *  @brief Cleanup of query status.
     *
     *  Usually called when czar starts to do post-crash cleanup.
     *
     *  @param name:  Czar ID, non-negative number.
     */
    virtual void cleanup(CzarId czarId) = 0;

    /**
     *  @brief Register new query.
     *
     *  This method will throw if czar ID is not known.
     *
     *  @param qInfo:  Query info instance, time members (submitted/completed) and query status
     *                 are ignored.
     *  @param tables: Set of tables used by the query, may be empty if tables are not needed
     *                 (e.g. for interactive queries).
     *  @return: Query ID, non-negative number
     */
    virtual QueryId registerQuery(QInfo const& qInfo, TableNames const& tables) = 0;

    /**
     *  @brief Add list of chunks to query.
     *
     *  This method will throw if query ID is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param chunks:    Set of chunk numbers.
     */
    virtual void addChunks(QueryId queryId, std::vector<int> const& chunks) = 0;

    /**
     *  @brief Assign or re-assign chunk to a worker.
     *
     *  This method will throw if query ID or chunk number is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param chunk:     Chunk number.
     *  @param xrdEndpoint:  Worker xrootd communication endpoint ("host:port").
     */
    virtual void assignChunk(QueryId queryId, int chunk, std::string const& xrdEndpoint) = 0;

    /**
     *  @brief Mark chunk as completed.
     *
     *  This method will throw if query ID or chunk number is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param chunk:     Sequence of chunk numbers.
     */
    virtual void finishChunk(QueryId queryId, int chunk) = 0;

    /**
     *  @brief Mark query as completed or failed.
     *
     *  This should be called when all data is collected in the result table or
     *  when failure/abort is detected.
     *  This method will throw if query ID is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param qStatus:   Query completion status, one of COMPLETED, FAILED, or ABORTED.
     *  @param collectedRows: number of rows collected from workers for the query.
     *  @param collectedBytes: number of bytes collected from workers for the query.
     */
    virtual void completeQuery(QueryId queryId, QInfo::QStatus qStatus, int64_t collectedRows = 0,
                               size_t collectedBytes = 0, size_t finalRows = 0) = 0;

    /**
     *  @brief Mark query as finished and returned to client.
     *
     *  This should be called after query result is sent back to client.
     *
     *  This method will throw if query ID is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     */
    virtual void finishQuery(QueryId queryId) = 0;

    /**
     *  @brief Generic interface for finding queries.
     *
     *  Returns the set of query IDs which satisfy all selections specified in parameters.
     *
     *  Setting "completed" to true is equivalent to setting "status" to a set of
     *  (COMPLETED, FAILED, ABORTED) but is based on different QInfo attribute,
     *  it uses "completed" instead of "status". Similarly setting "completed"
     *  to false is equivalent to setting "status" to (EXECUTING).
     *
     *  @param czarId:    Czar ID, non-negative number, if zero then
     *                    queries for all czars are returned
     *  @param qType:     Query type, if ANY then all query types are returned.
     *  @param user:      User name, if empty then queries for all users are returned.
     *  @param status:    Set of QInfo::QStatus values, only queries with status
     *                    that match any value in the list are returned, if set is
     *                    empty then all queries are returned.
     *  @param completed: If set to true/1 then select only completed queries (or
     *                    failed/aborted), if set to false/0 then return queries that
     *                    are still executing, if set to -1 (default) return all queries.
     *  @param returned:  If set to true/1 then select only queries with results already
     *                    returned to client, if set to false/0 then return queries with
     *                    result waiting to be returned or still executing, if set to -1
     *                    (default) return all queries.
     *  @return: List of query IDs.
     */
    virtual std::vector<QueryId> findQueries(
            CzarId czarId = 0, QInfo::QType qType = QInfo::ANY, std::string const& user = std::string(),
            std::vector<QInfo::QStatus> const& status = std::vector<QInfo::QStatus>(), int completed = -1,
            int returned = -1) = 0;

    /**
     *  @brief Find all pending queries for given czar.
     *
     *  Pending queries are queries which are either executing or
     *  have their result ready but not sent to client yet.
     *
     *  This method will throw if czar ID is not known.
     *
     *  @param czarId:   Czar ID, non-negative number.
     *  @return: List of query IDs.
     */
    virtual std::vector<QueryId> getPendingQueries(CzarId czarId) = 0;

    /**
     *  @brief Get full query information.
     *
     *  This method will throw if specified query ID does not exist.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @return: Object with query information.
     */
    virtual QInfo getQueryInfo(QueryId queryId) = 0;

    /**
     *  @brief Get queries which use specified database.
     *
     *  Only currently executing queries are returned.
     *
     *  @param dbName:   Database name.
     *  @return: List of query IDs.
     */
    virtual std::vector<QueryId> getQueriesForDb(std::string const& dbName) = 0;

    /**
     *  @brief Get queries which use specified table.
     *
     *  Only currently executing queries are returned.
     *
     *  @param dbName:   Database name.
     *  @param tableName:   Table name.
     *  @return: List of query IDs.
     */
    virtual std::vector<QueryId> getQueriesForTable(std::string const& dbName,
                                                    std::string const& tableName) = 0;

    /**
     * @brief Save the result query in metadata, to give to the proxy when fetching results from an async
     *        query.
     *
     *  This method will throw if query ID is not known.
     *
     * @param queryId: Query ID, non-negative number.
     * @param query : string, the query.
     */
    virtual void saveResultQuery(QueryId queryId, std::string const& query) = 0;

    /// Write messages/errors generated during the query to the QMessages table.
    virtual void addQueryMessages(QueryId queryId, std::shared_ptr<qmeta::MessageStore> const& msgStore) = 0;

    /**
     * Fetch the chunk map which was updated after the specified time point.
     * @param prevUpdateTime The cut off time for the chunk map age. Note that the default
     *   value of the parameter represents the start time of the UNIX Epoch. Leaving the default
     *   value forces an attempt to read the map from the database if the one would exist
     *   in there.
     * @return Return the most current chunk disposition or the empty object if the persistent
     *   map is older than it was requested.The result could be evaluated by calling
     *   method empty() on the result object.
     * @throws EmptyTableError if the corresponding metadata table doesn't have any record
     * @throws SqlError for any other error related to MySQL
     */
    virtual QMetaChunkMap getChunkMap(
            std::chrono::time_point<std::chrono::system_clock> const& prevUpdateTime =
                    std::chrono::time_point<std::chrono::system_clock>()) = 0;

protected:
    // Default constructor
    QMeta() {}
};

}  // namespace lsst::qserv::qmeta

#endif  // LSST_QSERV_QMETA_QMETA_H
