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
#ifndef LSST_QSERV_CZAR_CZARREGISTRY_H
#define LSST_QSERV_CZAR_CZARREGISTRY_H

// System headers
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "czar/ActiveWorker.h"
#include "global/clock_defs.h"
#include "util/Mutex.h"

namespace lsst::qserv::cconfig {
class CzarConfig;
}  // namespace lsst::qserv::cconfig

namespace lsst::qserv::czar {

/// This class connects to the Replication System's Registry to register this czar and get
/// worker contact information.
/// The assumptions going forward are that the CzarChunkMap provides the real location of
/// where all chunks are located and any workers in that map that are missing from this
/// map are just temporary communications problems. A real prolonged failure of a worker
/// will result in a new CzarChunkMap being created. As such, problems with missing
/// worker contact information will be handled in Job creation
/// in UserQueryFactory::newUserQuery and will be treated in similar manner as not being
/// able to contact a worker.
///
/// There really shouldn't be communications problems, but there are, the best course of
/// action would probably be to destroy the first instance of this and create a new one.
///
class CzarRegistry {
public:
    using Ptr = std::shared_ptr<CzarRegistry>;

    /// Return a pointer to a new CzarRegistry object.
    static Ptr create(std::shared_ptr<cconfig::CzarConfig> const& czarConfig,
                      std::shared_ptr<ActiveWorkerMap> const& activeWorkerMap) {
        return Ptr(new CzarRegistry(czarConfig, activeWorkerMap));
    }

    ~CzarRegistry();

    /// Return _contactMap, the object that the returned pointer points to is
    /// constant and no attempts should be made to change it.
    protojson::WorkerContactInfo::WCMapPtr getWorkerContactMap() const;

    /// Return _contactMap, the object that the returned pointer points to is
    /// constant and no attempts should be made to change it. This
    /// function will wait forever for a valid contact map to be ready.
    protojson::WorkerContactInfo::WCMapPtr waitForWorkerContactMap() const;

    /// Send all live workers the `WorkerQueryStatusData` message for
    /// that worker. This may result in the worker sending back the
    /// `WorkerCzarComIssue` message if there were communication problems.
    void sendActiveWorkersMessages();

    /// Add the query id to the list of queries to end on workers and
    /// send the messages, deleting all result files if
    /// `deleteWorkerResults` is true.
    void endUserQueryOnWorkers(QueryId qId, bool deleteWorkerResults);

private:
    CzarRegistry() = delete;
    CzarRegistry(std::shared_ptr<cconfig::CzarConfig> const& czarConfig,
                 std::shared_ptr<ActiveWorkerMap> const& activeWorkerMap);

    /// This function will keep periodically updating Czar's info in the Replication System's Registry
    /// until _loop is set to false.
    /// Communications problems are logged but ignored. This should probably change.
    void _registryUpdateLoop();

    /// This function collects worker contact information from the Replication System's Registry
    /// until _loop is set to false.
    /// Communications problems are logged but ignored. This should probably change.
    void _registryWorkerInfoLoop();

    /// Build a new WorkerContactMap from the json `response`
    protojson::WorkerContactInfo::WCMapPtr _buildMapFromJson(nlohmann::json const& response);

    /// Return true if maps are the same size and all of the elements have the same contact info.
    /// NOTE: _cmapMtx must be held when calling.
    bool _compareMapContactInfo(protojson::WorkerContactInfo::WCMap const& other) const;

    std::shared_ptr<cconfig::CzarConfig> const _czarConfig;  ///< Pointer to the CzarConfig.

    std::atomic<bool> _loop{true};    ///< Threads will continue to run until this is set false.
    std::thread _czarHeartbeatThrd;   ///< This thread continually registers this czar with the registry.
    std::thread _czarWorkerInfoThrd;  ///< This thread continuously collects worker contact information.

    /// Pointer to the map of worker contact information.
    protojson::WorkerContactInfo::WCMapPtr _contactMap;
    TIMEPOINT _latestMapUpdate;  ///< The last time the _contactMap was updated, unrelated to
                                 ///< WorkerContactInfo update.
    mutable MUTEX _cmapMtx;      /// Protects _contactMap, _latestUpdate

    /// Map for tracking worker aliveness, it has its own internal mutex.
    std::shared_ptr<ActiveWorkerMap> const _activeWorkerMap;
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_CZARREGISTRY_H
