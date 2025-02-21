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
#include "xrdsvc/HttpReplicaMgtModule.h"

// System headers
#include <sstream>
#include <stdexcept>
#include <vector>

// Third party headers
#include "lsst/log/Log.h"
#include "XrdSsi/XrdSsiCluster.hh"

// Qserv headers
#include "http/Exceptions.h"
#include "http/RequestBodyJSON.h"
#include "http/RequestQuery.h"
#include "mysql/MySqlUtils.h"
#include "util/String.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/Foreman.h"
#include "wcontrol/ResourceMonitor.h"
#include "wpublish/ChunkInventory.h"
#include "xrdsvc/SsiProvider.h"
#include "xrdsvc/XrdName.h"

extern XrdSsiProvider* XrdSsiProviderLookup;

using namespace std;
using json = nlohmann::json;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.HttpReplicaMgt");
}

namespace {
// These markers if reported in the extended error response object of the failed
// requests could be used by a caller for refining the completion status
// of the corresponding Controller-side operation.
json const extErrorInvalidParam = json::object({{"invalid_param", 1}});
json const extErrorReplicaInUse = json::object({{"in_use", 1}});

string makeResource(string const& database, int chunk) { return "/chk/" + database + "/" + to_string(chunk); }

}  // namespace

namespace lsst::qserv::xrdsvc {

void HttpReplicaMgtModule::process(string const& context, shared_ptr<wcontrol::Foreman> const& foreman,
                                   shared_ptr<qhttp::Request> const& req,
                                   shared_ptr<qhttp::Response> const& resp, string const& subModuleName,
                                   http::AuthType const authType) {
    HttpReplicaMgtModule module(context, foreman, req, resp);
    module.execute(subModuleName, authType);
}

HttpReplicaMgtModule::HttpReplicaMgtModule(string const& context,
                                           shared_ptr<wcontrol::Foreman> const& foreman,
                                           shared_ptr<qhttp::Request> const& req,
                                           shared_ptr<qhttp::Response> const& resp)
        : HttpModule(context, foreman, req, resp) {}

json HttpReplicaMgtModule::executeImpl(string const& subModuleName) {
    string const func = string(__func__) + "[sub-module='" + subModuleName + "']";
    enforceInstanceId(func, wconfig::WorkerConfig::instance()->replicationInstanceId());
    enforceWorkerId(func);
    if (subModuleName == "GET")
        return _getReplicas();
    else if (subModuleName == "SET")
        return _setReplicas();
    else if (subModuleName == "ADD")
        return _addReplica();
    else if (subModuleName == "REMOVE")
        return _removeReplica();
    else if (subModuleName == "REBUILD")
        return _rebuildInventory();
    throw invalid_argument(context() + func + " unsupported sub-module");
}

json HttpReplicaMgtModule::_getReplicas() {
    debug(__func__);
    checkApiVersion(__func__, 27);
    bool const inUseOnly = query().optionalUInt("in_use_only", 0) != 0;
    vector<string> const databases = query().requiredVectorStr("databases");
    debug(__func__, "in_use_only: " + string(inUseOnly ? "1" : "0"));
    debug(__func__, "databases: " + util::String::toString(databases));
    set<string> databaseFilter;
    for (string const& database : databases) {
        databaseFilter.insert(database);
    }
    return _replicas(databaseFilter, inUseOnly);
}

json HttpReplicaMgtModule::_setReplicas() {
    debug(__func__);
    checkApiVersion(__func__, 27);
    json const replicas = body().required<json>("replicas");
    bool const force = body().optional<int>("force", 0) != 0;
    vector<string> const databases = body().requiredColl<string>("databases");
    debug(__func__, "force: " + string(force ? "1" : "0"));
    debug(__func__, "databases: " + util::String::toString(databases));
    set<string> databaseFilter;
    for (string const& database : databases) {
        databaseFilter.insert(database);
    }
    json const prevReplicas = _replicas(databaseFilter);
    if (replicas.empty()) return prevReplicas;

    if (!replicas.is_object()) {
        throw http::Error(__func__, "the replica collection must be an object.");
    }

    // Build a temporary object representing a desired collection of replicas to be deployed.
    wpublish::ChunkInventory::ExistMap newExistMap;
    for (auto&& [database, chunks] : replicas.items()) {
        if (databaseFilter.contains(database)) {
            if (!chunks.is_array()) {
                throw http::Error(__func__, "the chunk collection must be an array.");
            }
            for (int const chunk : chunks) {
                newExistMap[database].insert(chunk);
            }
        }
    }
    // Make desired adjustments to the current inventory.
    wpublish::ChunkInventory const newChunkInventory(newExistMap, foreman()->chunkInventory()->name(),
                                                     foreman()->chunkInventory()->id());
    _updateInventory(__func__, newChunkInventory, databaseFilter, force);
    return prevReplicas;
}

json HttpReplicaMgtModule::_addReplica() {
    debug(__func__);
    checkApiVersion(__func__, 27);
    _modifyReplica(__func__, Direction::ADD);
    return json::object();
}

json HttpReplicaMgtModule::_removeReplica() {
    debug(__func__);
    checkApiVersion(__func__, 27);
    _modifyReplica(__func__, Direction::REMOVE);
    return json::object();
}

json HttpReplicaMgtModule::_rebuildInventory() {
    debug(__func__);
    checkApiVersion(__func__, 27);
    bool const rebuild = body().optional<int>("rebuild", 0) != 0;
    bool const reload = body().optional<int>("reload", 0) != 0;
    bool const force = body().optional<int>("force", 0) != 0;
    debug(__func__, "rebuild: " + string(rebuild ? "1" : "0"));
    debug(__func__, "reload: " + string(reload ? "1" : "0"));
    debug(__func__, "force: " + string(force ? "1" : "0"));
    if (!rebuild && !reload) {
        throw http::Error(__func__, "the 'rebuild' or 'reload' or both actions are required.");
    }

    // Start with updating the persistent inventory if requested.
    if (rebuild) _rebuildPersistentInventory();

    // Proceed to reload the transient one from the persistent inventory.
    // When it will be done then Qserv will be able to see changes in the available replicas.
    json result = json::object({{"added", json::object()}, {"removed", json::object()}});
    if (reload) {
        // Load the persistent inventory data into the transient one.
        wpublish::ChunkInventory newChunkInventory;
        try {
            xrdsvc::XrdName x;
            newChunkInventory.init(x.getName(), foreman()->mySqlConfig());
        } catch (exception const& ex) {
            throw http::Error(__func__, "persistent inventory read failed, ex: " + string(ex.what()));
        }
        _dumpInventory(__func__, *(foreman()->chunkInventory()), "transient");
        _dumpInventory(__func__, newChunkInventory, "persistent");

        // Deploy the new inventory and record changes to be reported to a caller
        // of the REST service.
        auto const resourceMonitor = foreman()->resourceMonitor();
        OnModifiedChunkCallback const onModifiedChunk =
                [&result, &resourceMonitor](int chunk, string const& database, Direction direction) {
                    if (Direction::ADD == direction) {
                        result["added"][database].push_back(json::array({chunk, 0}));
                    } else {
                        auto const useCount = resourceMonitor->count(chunk, database);
                        result["removed"][database].push_back(json::array({chunk, useCount}));
                    }
                };
        // All databases mentioned in the persistent inventory will be considered by the filter.
        set<string> const databaseFilter = newChunkInventory.databases();
        _updateInventory(__func__, newChunkInventory, databaseFilter, force, onModifiedChunk);
    }
    return result;
}

void HttpReplicaMgtModule::_rebuildPersistentInventory() const {
    wpublish::ChunkInventory newChunkInventory;
    try {
        xrdsvc::XrdName x;
        newChunkInventory.rebuild(x.getName(), foreman()->mySqlConfig());
    } catch (exception const& ex) {
        throw http::Error(__func__, "inventory rebuild stage failed, ex: " + string(ex.what()));
    }
}

void HttpReplicaMgtModule::_dumpInventory(string const& func, wpublish::ChunkInventory const& inventory,
                                          string const& kind) const {
    ostringstream os;
    inventory.dbgPrint(os);
    debug(func, "ChunkInventory[" + kind + "]: " + os.str());
}

void HttpReplicaMgtModule::_updateInventory(string const& func,
                                            wpublish::ChunkInventory const& newChunkInventory,
                                            set<string> const& databaseFilter, bool force,
                                            OnModifiedChunkCallback const onModifiedChunk) {
    // Compare two maps and worker identifiers to see which resources were
    // were added or removed.
    wpublish::ChunkInventory::ExistMap const toBeRemovedExistMap =
            *(foreman()->chunkInventory()) - newChunkInventory;
    wpublish::ChunkInventory::ExistMap const toBeAddedExistMap =
            newChunkInventory - *(foreman()->chunkInventory());

    // Make sure none of the chunks in the 'to be removed' group is being used
    // unless processing the request in the 'force' mode.
    auto const resourceMonitor = foreman()->resourceMonitor();
    if (!force) {
        for (auto&& [database, chunks] : toBeRemovedExistMap) {
            if (databaseFilter.contains(database)) {
                for (const int chunk : chunks) {
                    if (resourceMonitor->count(chunk, database) != 0) {
                        throw http::Error(func, "the replica is in use", ::extErrorReplicaInUse);
                    }
                }
            }
        }
    }

    // Update the current map and notify XRootD accordingly.
    for (auto&& [database, chunks] : toBeRemovedExistMap) {
        if (databaseFilter.contains(database)) {
            for (int const chunk : chunks) {
                auto const direction = Direction::REMOVE;
                _modifyChunk(func, chunk, database, direction);
                if (onModifiedChunk != nullptr) onModifiedChunk(chunk, database, direction);
            }
        }
    }
    for (auto&& [database, chunks] : toBeAddedExistMap) {
        if (databaseFilter.contains(database)) {
            for (int const chunk : chunks) {
                auto const direction = Direction::ADD;
                _modifyChunk(func, chunk, database, direction);
                if (onModifiedChunk != nullptr) onModifiedChunk(chunk, database, direction);
            }
        }
    }
}

json HttpReplicaMgtModule::_replicas(set<string> const& databaseFilter, bool inUseOnly) const {
    auto const resourceMonitor = foreman()->resourceMonitor();
    json replicas = json::object();
    for (auto&& [database, chunks] : foreman()->chunkInventory()->existMap()) {
        // Apply the mandatory database filter.
        if (databaseFilter.contains(database)) {
            replicas[database] = json::array();
            json& chunksJson = replicas[database];
            for (int const chunk : chunks) {
                // Apply the optional chunk usage filter.
                auto const useCount = resourceMonitor->count(chunk, database);
                if (!inUseOnly || (inUseOnly && (useCount != 0))) {
                    chunksJson.push_back(json::array({chunk, useCount}));
                }
            }
        }
    }
    return json::object({{"replicas", replicas}});
}

void HttpReplicaMgtModule::_modifyReplica(string const& func, Direction direction) {
    int const chunk = body().required<int>("chunk");
    vector<string> const databases = body().requiredColl<string>("databases");
    bool const force = body().optional<int>("force", 0) != 0;

    debug(func, "chunk: " + to_string(chunk));
    debug(func, "databases: " + util::String::toString(databases));
    debug(func, "force: " + string(force ? "1" : "0"));

    if (databases.empty()) {
        throw http::Error(func, "the database collection is empty.", ::extErrorInvalidParam);
    }

    // Make sure none of the chunks in the group is in use unless forced removal.
    if (Direction::REMOVE == direction) {
        if (!force && (0 != foreman()->resourceMonitor()->count(chunk, databases))) {
            throw http::Error(func, "the replica is in use", ::extErrorReplicaInUse);
        }
    }
    for (auto&& database : databases) {
        _modifyChunk(func, chunk, database, direction);
    }
}

void HttpReplicaMgtModule::_modifyChunk(string const& func, int chunk, string const& database,
                                        Direction direction) {
    string const operation = Direction::ADD == direction ? "add" : "remove";
    string const resource = ::makeResource(database, chunk);
    debug(func, operation + " resource: " + resource + ", DataContext: " + to_string(_dataContext));
    try {
        // Notify XRootD/cmsd and (depending on a mode) modify the provider's
        // copy of the inventory. After that modify both (persistent and
        // transient) inventories.
        if (Direction::ADD == direction) {
            foreman()->chunkInventory()->add(database, chunk, foreman()->mySqlConfig());
        } else {
            foreman()->chunkInventory()->remove(database, chunk, foreman()->mySqlConfig());
        }
    } catch (wpublish::InvalidParamError const& ex) {
        throw http::Error(func, "invalid parameter, ex: " + string(ex.what()), ::extErrorInvalidParam);
    } catch (wpublish::QueryError const& ex) {
        throw http::Error(func, "persistent " + operation + " failed, ex: " + string(ex.what()));
    } catch (exception const& ex) {
        throw http::Error(func, "transient " + operation + " failed, ex: " + string(ex.what()));
    }
}

}  // namespace lsst::qserv::xrdsvc
