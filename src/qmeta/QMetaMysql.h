/*
 * LSST Data Management System
 * Copyright 2015 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */
#ifndef LSST_QSERV_QMETA_QMETAMYSQL_H
#define LSST_QSERV_QMETA_QMETAMYSQL_H

// System headers
#include <chrono>
#include <map>
#include <mutex>

// Third-party headers

// Qserv headers
#include "global/constants.h"
#include "mysql/MySqlConfig.h"
#include "qmeta/QMeta.h"

// Forward declarations
namespace lsst::qserv::sql {
class SqlConnection;
}  // namespace lsst::qserv::sql

namespace lsst::qserv::qmeta {

class QueryMessage;

/// @addtogroup qmeta

/**
 *  @ingroup qmeta
 *  @brief Mysql-based implementation of qserv metadata.
 */

class QMetaMysql : public QMeta {
public:
    /**
     *  @param mysqlConf: Configuration object for mysql connection
     */
    QMetaMysql(mysql::MySqlConfig const& mysqlConf, int maxMsgSourceStore);

    // Instances cannot be copied
    QMetaMysql(QMetaMysql const&) = delete;
    QMetaMysql& operator=(QMetaMysql const&) = delete;

    // Destructor
    ~QMetaMysql() override;

    /**
     *  @brief Return czar ID given czar "name".
     *
     *  Negative number is returned if czar does not exist.
     *
     *  @param name:  Czar name, arbitrary string.
     *  @return: Czar ID, zero if czar does not exist.
     */
    CzarId getCzarID(std::string const& name) override;

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
    CzarId registerCzar(std::string const& name) override;

    /**
     *  @brief Mark specified czar as active or inactive.
     *
     *  This method will throw if czar ID is not known.
     *
     *  @param name:  Czar ID, non-negative number.
     *  @param active:  new value if active flag.
     */
    void setCzarActive(CzarId czarId, bool active) override;

    /**
     *  @brief Cleanup of query status.
     *
     *  Usually called when czar starts to do post-crash cleanup.
     *
     *  @param name:  Czar ID, non-negative number.
     */
    void cleanup(CzarId czarId) override;

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
    QueryId registerQuery(QInfo const& qInfo, TableNames const& tables) override;

    /**
     *  @brief Add list of chunks to query.
     *
     *  This method will throw if query ID is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param chunks:    Set of chunk numbers.
     */
    void addChunks(QueryId queryId, std::vector<int> const& chunks) override;

    /**
     *  @brief Assign or re-assign chunk to a worker.
     *
     *  This method will throw if query ID or chunk number is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param chunk:     Chunk number.
     *  @param xrdEndpoint:  Worker xrootd communication endpoint ("host:port").
     */
    void assignChunk(QueryId queryId, int chunk, std::string const& xrdEndpoint) override;

    /**
     *  @brief Mark chunk as completed.
     *
     *  This method will throw if query ID or chunk number is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @param chunk:     Sequence of chunk numbers.
     */
    void finishChunk(QueryId queryId, int chunk) override;

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
     *  @param finalRows: number of rows in the final result.
     */
    void completeQuery(QueryId queryId, QInfo::QStatus qStatus, int64_t collectedRows, size_t collectedBytes,
                       size_t finalRows) override;

    /**
     *  @brief Mark query as finished and returned to client.
     *
     *  This should be called after query result is sent back to client.
     *
     *  This method will throw if query ID is not known.
     *
     *  @param queryId:   Query ID, non-negative number.
     */
    void finishQuery(QueryId queryId) override;

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
    std::vector<QueryId> findQueries(
            CzarId czarId = 0, QInfo::QType qType = QInfo::ANY, std::string const& user = std::string(),
            std::vector<QInfo::QStatus> const& status = std::vector<QInfo::QStatus>(), int completed = -1,
            int returned = -1) override;

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
    std::vector<QueryId> getPendingQueries(CzarId czarId) override;

    /**
     *  @brief Get full query information.
     *
     *  This method will throw if specified query ID does not exist.
     *
     *  @param queryId:   Query ID, non-negative number.
     *  @return: Object with query information.
     */
    QInfo getQueryInfo(QueryId queryId) override;

    /**
     *  @brief Get queries which use specified database.
     *
     *  Only currently executing queries are returned.
     *
     *  @param dbName:   Database name.
     *  @return: List of query IDs.
     */
    std::vector<QueryId> getQueriesForDb(std::string const& dbName) override;

    /**
     *  @brief Get queries which use specified table.
     *
     *  Only currently executing queries are returned.
     *
     *  @param dbName:   Database name.
     *  @param tableName:   Table name.
     *  @return: List of query IDs.
     */
    std::vector<QueryId> getQueriesForTable(std::string const& dbName, std::string const& tableName) override;

    /**
     * @brief Save the result query in metadata, to give to the proxy when fetching results from an async
     *        query.
     *
     *  This method will throw if query ID is not known.
     *
     * @param queryId: Query ID, non-negative number.
     * @param query : string, the query.
     */
    void saveResultQuery(QueryId queryId, std::string const& query) override;

    /// @see QMeta::addQueryMessages()
    void addQueryMessages(QueryId queryId, std::shared_ptr<qmeta::MessageStore> const& msgStore) override;

    /// @see QMeta::getChunkMap
    QMetaChunkMap getChunkMap(std::chrono::time_point<std::chrono::system_clock> const& prevUpdateTime =
                                      std::chrono::time_point<std::chrono::system_clock>()) override;

protected:
    ///  Check that all necessary tables exist
    void _checkDb();

    /// Simple class for storing information about multiple messages from a single source.
    class ManyMsg {
    public:
        ManyMsg() = default;
        ManyMsg(int count_, MessageSeverity severity_) : count(count_), severity(severity_) {}
        int count = 0;
        MessageSeverity severity = MSG_INFO;
    };

private:
    /**
     * Read the last update time of the chunk map.
     * @param A lock acquired on the mutex _dbMutex.
     * @return The update time
     * @throw EmptyTableError If the corrresponding table is epty
     * @throw SqlError For any SQL-specific error
     * @throw ConsistencyError For any problem met when parsing or interpreting results read
     *   from the table.
     */
    std::chrono::time_point<std::chrono::system_clock> _getChunkMapUpdateTime(
            std::lock_guard<std::mutex> const& lock);

    /// Add qMsg to the permanent message table.
    void _addQueryMessage(QueryId queryId, qmeta::QueryMessage const& qMsg, int& cancelCount,
                          int& completeCount, int& execFailCount,
                          std::map<std::string, ManyMsg>& msgCountMap);

    std::shared_ptr<sql::SqlConnection> _conn;
    std::mutex _dbMutex;  ///< Synchronizes access to certain DB operations
    /// Maximum number of each msgSource type to store for one user query.
    int _maxMsgSourceStore = 3;
};

}  // namespace lsst::qserv::qmeta

#endif  // LSST_QSERV_QMETA_QMETAMYSQL_H
