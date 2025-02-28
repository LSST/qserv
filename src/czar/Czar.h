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
#ifndef LSST_QSERV_CZAR_CZAR_H
#define LSST_QSERV_CZAR_CZAR_H

// System headers
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Third-party headers

// Qserv headers
#include "ccontrol/UserQuery.h"
#include "ccontrol/UserQueryFactory.h"
#include "czar/SubmitResult.h"
#include "global/clock_defs.h"
#include "global/intTypes.h"
#include "global/stringTypes.h"
#include "mysql/MySqlConfig.h"
#include "util/ConfigStore.h"
#include "util/Timer.h"

// Forward declarations

namespace lsst::qserv::cconfig {
class CzarConfig;
}  // namespace lsst::qserv::cconfig

namespace lsst::qserv::czar {
class ActiveWorkerMap;
class HttpSvc;
}  // namespace lsst::qserv::czar

namespace lsst::qserv::http {
class ClientConnPool;
}  // namespace lsst::qserv::http

namespace lsst::qserv::util {
class FileMonitor;
}  // namespace lsst::qserv::util

namespace lsst::qserv::qdisp {
class Executive;
}  // namespace lsst::qserv::qdisp

namespace lsst::qserv::czar {

class CzarFamilyMap;
class CzarRegistry;

/// @addtogroup czar

/**
 * @ingroup czar
 * @brief Class representing czar "entry points".
 */

class Czar {
public:
    using Ptr = std::shared_ptr<Czar>;

    Czar(Czar const&) = delete;
    Czar& operator=(Czar const&) = delete;
    ~Czar();

    /**
     * Submit query for execution.
     *
     * @param query: Query text.
     * @param hints: Optional query hints, default database name should be
     *               provided as "db" key.
     * @return Structure with info about submitted query.
     */
    SubmitResult submitQuery(std::string const& query, std::map<std::string, std::string> const& hints);

    /**
     * Process a kill query command (experimental).
     *
     * @param query: (client)proxy-provided "KILL QUERY ..." string
     * @param clientId : client name from proxy
     * @throws Exception is thrown if query Id is not known
     */
    void killQuery(std::string const& query, std::string const& clientId);

    /**
     * Make new instance.
     *
     * @param configFilePath: Path to the configuration file.
     * @param czarName:       Name if this instance, must be unique. If empty name
     *                        is given then random name will be constructed.
     */
    static Ptr createCzar(std::string const& configFilePath, std::string const& czarName);

    /**
     * During startup, this may return nullptr.
     *
     * @return a pointer to the czar.
     *
     */
    static Ptr getCzar() { return _czar; }

    /// Remove all old tables in the qservResult database.
    void removeOldResultTables();

    /// @return true if trivial queries should be treated as
    ///         interactive queries to stress test the czar.
    bool getQueryDistributionTestVer() { return _queryDistributionTestVer; }

    /// @param queryId The unique identifier of the previously submitted user query
    /// @return The reconstructed info for the query
    SubmitResult getQueryInfo(QueryId queryId) const;

    std::shared_ptr<CzarFamilyMap> getCzarFamilyMap() const { return _czarFamilyMap; }

    std::shared_ptr<CzarRegistry> getCzarRegistry() const { return _czarRegistry; }

    /// Add an Executive to the map of executives.
    void insertExecutive(QueryId qId, std::shared_ptr<qdisp::Executive> const& execPtr);

    /// Get the executive associated with `qId`, this may be nullptr.
    std::shared_ptr<qdisp::Executive> getExecutiveFromMap(QueryId qId);

    std::shared_ptr<ActiveWorkerMap> getActiveWorkerMap() const { return _activeWorkerMap; }

    std::map<QueryId, std::weak_ptr<qdisp::Executive>> getExecMapCopy() const;

    /// This function kills incomplete UberJobs associated with `workerId`.
    /// This is done when it is believed a worker has died. The executive
    /// un-assignes the Jobs associated with the UberJobs and then
    /// adds the ids to lists for the affected worker. If the worker
    /// reconnects, it will stop work on those UberJobs when it gets the
    /// list.
    void killIncompleteUbjerJobsOn(std::string const& workerId);

    std::shared_ptr<util::QdispPool> getQdispPool() const { return _qdispPool; }

    std::shared_ptr<http::ClientConnPool> getCommandHttpPool() const { return _commandHttpPool; }

    /// Startup time of czar, sent to workers so they can detect that the czar was
    /// was restarted when this value changes.
    static uint64_t const czarStartupTime;

private:
    /// Private constructor for singleton.
    Czar(std::string const& configFilePath, std::string const& czarName);

    /// Clean query maps from expired entries, _mutex must be locked
    void _cleanupQueryHistoryLocked();

    /// Clean query maps from expired entries
    void _cleanupQueryHistory();

    /// Clean query maps from expired entries, add new query
    void _updateQueryHistory(std::string const& clientId, int threadId, ccontrol::UserQuery::Ptr const& uq);

    /// Create and fill async result table
    void _makeAsyncResult(std::string const& asyncResultTable, QueryId queryId, std::string const& resultLoc);

    /// @return An identifier of the last query that was recorded in the query metadata table
    QueryId _lastQueryIdBeforeRestart() const;

    /// Periodically check for system changes and use those changes to try to finish queries.
    void _monitor();

    static Ptr _czar;  ///< Pointer to single instance of the Czar.

    // combines client name (ID) and its thread ID into one unique ID
    typedef std::pair<std::string, int> ClientThreadId;
    typedef std::map<ClientThreadId, std::weak_ptr<ccontrol::UserQuery>> ClientToQuery;
    typedef std::map<QueryId, std::weak_ptr<ccontrol::UserQuery>> IdToQuery;

    std::string const _czarName;  ///< Unique czar name
    std::shared_ptr<cconfig::CzarConfig> const _czarConfig;

    std::atomic<uint64_t> _idCounter;  ///< Query/task identifier for next query
    std::unique_ptr<ccontrol::UserQueryFactory> _uqFactory;
    ClientToQuery _clientToQuery;  ///< maps client ID to query
    IdToQuery _idToQuery;          ///< maps query ID to query (for currently running queries)
    std::mutex _mutex;             ///< protects _uqFactory, _clientToQuery, and _idToQuery

    util::Timer _lastRemovedTimer;  ///< Timer to limit table deletions.
    std::mutex _lastRemovedMtx;     ///< protects _lastRemovedTimer

    /// Prevents multiple concurrent calls to _removeOldTables().
    std::atomic<bool> _removingOldTables{false};
    std::thread _oldTableRemovalThread;  ///< thread needs to remain valid while running.

    bool _queryDistributionTestVer;  ///< True if config says this is distribution test version.

    /// Reloads the log configuration file on log config file change.
    std::shared_ptr<util::FileMonitor> _logFileMonitor;

    /// The HTTP server processing Czar management requests.
    std::shared_ptr<HttpSvc> _controlHttpSvc;

    /// Map of which chunks on which workers and shared scan order.
    std::shared_ptr<CzarFamilyMap> _czarFamilyMap;

    /// Connection to the registry to register the czar and get worker contact information.
    std::shared_ptr<CzarRegistry> _czarRegistry;

    mutable std::mutex _executiveMapMtx;  ///< protects _executiveMap
    std::map<QueryId, std::weak_ptr<qdisp::Executive>>
            _executiveMap;  ///< Map of executives for queries in progress.

    std::thread _monitorThrd;  ///< Thread to run the _monitor()

    /// Set to false on system shutdown to stop _monitorThrd.
    std::atomic<bool> _monitorLoop{true};

    /// Wait time between checks to.
    std::chrono::milliseconds _monitorSleepTime;

    /// Keeps track of all workers (alive or otherwise) that this czar
    /// may communicate with. Once created, the pointer never changes.
    std::shared_ptr<ActiveWorkerMap> _activeWorkerMap;

    /// A combined priority queue and thread pool to regulate czar communications
    /// with workers. Once created, the pointer never changes.
    /// TODO:UJ - It may be better to have a pool for each worker as it
    ///           may be possible for a worker to have communications
    ///           problems in a way that would wedge the pool. This can
    ///           probably be done fairly easily by having pools
    ///           attached to ActiveWorker in _activeWorkerMap.
    ///           This was not possible in xrootd as the czar had
    ///           no reasonable way to know where Jobs were going.
    std::shared_ptr<util::QdispPool> _qdispPool;

    /// Pool of http client connections for sending commands (UberJobs
    /// and worker status requests).
    std::shared_ptr<http::ClientConnPool> _commandHttpPool;
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_CZAR_H
