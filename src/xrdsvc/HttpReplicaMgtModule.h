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
#ifndef LSST_QSERV_XRDSVC_HTTPREPLICAMGTMODULE_H
#define LSST_QSERV_XRDSVC_HTTPREPLICAMGTMODULE_H

// System headers
#include <functional>
#include <memory>
#include <set>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "xrdsvc/HttpModule.h"

// Forward declarations
class XrdSsiCluster;

namespace lsst::qserv::qhttp {
class Request;
class Response;
}  // namespace lsst::qserv::qhttp

namespace lsst::qserv::wcontrol {
class Foreman;
}  // namespace lsst::qserv::wcontrol

namespace lsst::qserv::wpublish {
class ChunkInventory;
}  // namespace lsst::qserv::wpublish

namespace lsst::qserv::xrdsvc {
class SsiProviderServer;
}  // namespace lsst::qserv::xrdsvc

// This header declarations
namespace lsst::qserv::xrdsvc {

/**
 * Class HttpReplicaMgtModule implements a handler for managing chunk replicas
 * in the given scope, which could be a database family (a collection of related
 * databases that were partitioned based on the same partitioning configuration)
 * or all known databases.
 */
class HttpReplicaMgtModule : public xrdsvc::HttpModule {
public:
    /**
     * @note supported values for parameter 'subModuleName' are:
     *   'GET'     - get all replicas (across all database families)
     *   'SET'     - set/replace all replicas (one family)
     *   'ADD'     - register a new replica (one family)
     *   'REMOVE'  - unregister an existing replica (one family)
     *   'REBUILD' - rebuild and update the chunk inventory
     *
     * @throws std::invalid_argument for unknown values of parameter 'subModuleName'
     */
    static void process(std::string const& context, std::shared_ptr<wcontrol::Foreman> const& foreman,
                        std::shared_ptr<qhttp::Request> const& req,
                        std::shared_ptr<qhttp::Response> const& resp, std::string const& subModuleName,
                        http::AuthType const authType = http::AuthType::NONE);

    HttpReplicaMgtModule() = delete;
    HttpReplicaMgtModule(HttpReplicaMgtModule const&) = delete;
    HttpReplicaMgtModule& operator=(HttpReplicaMgtModule const&) = delete;

    ~HttpReplicaMgtModule() final = default;

protected:
    virtual nlohmann::json executeImpl(std::string const& subModuleName) final;

private:
    HttpReplicaMgtModule(std::string const& context, std::shared_ptr<wcontrol::Foreman> const& foreman,
                         std::shared_ptr<qhttp::Request> const& req,
                         std::shared_ptr<qhttp::Response> const& resp);

    /// @return A collection of known replicas.
    nlohmann::json _getReplicas();

    /// @return A collection of the previously registered replicas.
    nlohmann::json _setReplicas();

    /// @return An empty result.
    nlohmann::json _addReplica();

    /// @return A flag indicating of the replica is still in use or if it
    ///   was in use (force mode).
    nlohmann::json _removeReplica();

    /// @return Two collections: added and removed replicas
    nlohmann::json _rebuildInventory();

    /// The type defining a direction of the change in the chunk inventory.
    enum class Direction : int { ADD, REMOVE };

    /**
     * The callback type for post-processing changes applied to the chuk inventory.
     * @param int The chunk number
     * @param std::string const& The name of a database.
     * @param Direction The direction of the change.
     */
    using OnModifiedChunkCallback = std::function<void(int, std::string const&, Direction)>;

    /**
     * Get a collection of existing replicas.
     * @param databaseFilter A filter for narrowing a scope of the operation to specified databases.
     * @param inUseOnly The optional filter for the replica usage status. If a value
     *   of the filter is set to 'false' then replicas in any status will be included
     *   into the result.
     * @return A collection of replicas matching the filters.
     */
    nlohmann::json _replicas(std::set<std::string> const& databaseFilter, bool inUseOnly = false) const;

    /**
     * Get chunk, database names and the optional 'force' flag from the request's body
     * and implement the requested (addition or removal) operation over the replica.
     * @param func The calling context (for diagnostic and error reporting).
     * @param direction A direction of the change.
     * @throws http::Error in case of any errors.
     */
    void _modifyReplica(std::string const& func, Direction direction);

    /**
     * Rebuild the persisent inventory table based on replicas that are actually
     * present in this Qserv worker's MySQL.
     * @note The Qserv worker still won't see any changes in the available replicas
     *   before pushing the new collection of chunks into the transient store.
     * @throws http::Error in case of any errors.
     */
    void _rebuildPersistentInventory() const;

    /**
     * Print the inventory status onto the logging stream.
     * @param func The calling context (for diagnostic and error reporting).
     * @param inventory The inventory to be dumped.
     * @param kind The name of a kind (transient or persistent) of the inventry.
     */
    void _dumpInventory(std::string const& func, wpublish::ChunkInventory const& inventory,
                        std::string const& kind) const;

    /**
     * Deploy the new versin of the chunk inventory.
     * @param func The calling context (for diagnostic and error reporting).
     * @param newChunkInventory The new inventory to be deployed.
     * @param databaseFilter A filter for narrowing a scope of the operation to specified databases.
     * @param force If 'true' then force replica removal even if the replica is in use.
     * @param onModifiedChunk (Optional) callback to be invoked on each modification.
     * @throws http::Error for any errors encountered when deploying requested changes.
     */
    void _updateInventory(std::string const& func, wpublish::ChunkInventory const& newChunkInventory,
                          std::set<std::string> const& databaseFilter, bool force,
                          OnModifiedChunkCallback const onModifiedChunk = nullptr);

    /**
     * Implement the requested (addition or removal) operation over the chunk.
     *
     * @note Both operation require basically the same sequence of steps,
     *   except the names of underlying functions called by the implementation
     *   are different.
     * @param func The calling context (for diagnostic and error reporting).
     * @param chunk The chunk number.
     * @param database The name of a database.
     * @param direction A direction of the change.
     * @throws http::Error in case of any errors.
     */
    void _modifyChunk(std::string const& func, int chunk, std::string const& database, Direction direction);

    bool _dataContext = false;
};

}  // namespace lsst::qserv::xrdsvc

#endif  // LSST_QSERV_XRDSVC_HTTPREPLICAMGTMODULE_H
