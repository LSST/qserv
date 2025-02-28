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
#include "czar/CzarRegistry.h"

// System headers
#include <stdexcept>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "czar/CzarChunkMap.h"
#include "czar/Czar.h"
#include "http/Client.h"
#include "http/Method.h"
#include "util/common.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace nlohmann;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.CzarRegistry");
}  // namespace

namespace lsst::qserv::czar {

CzarRegistry::CzarRegistry(cconfig::CzarConfig::Ptr const& czarConfig,
                           ActiveWorkerMap::Ptr const& activeWorkerMap)
        : _czarConfig(czarConfig), _activeWorkerMap(activeWorkerMap) {
    // Begin periodically updating worker's status in the Replication System's registry.
    // This will continue until the application gets terminated.
    thread registryUpdateThread(&CzarRegistry::_registryUpdateLoop, this);
    _czarHeartbeatThrd = move(registryUpdateThread);

    thread registryWorkerUpdateThread(&CzarRegistry::_registryWorkerInfoLoop, this);
    _czarWorkerInfoThrd = move(registryWorkerUpdateThread);
}

CzarRegistry::~CzarRegistry() {
    _loop = false;
    if (_czarHeartbeatThrd.joinable()) {
        _czarHeartbeatThrd.join();
    }
    if (_czarWorkerInfoThrd.joinable()) {
        _czarWorkerInfoThrd.join();
    }
}

protojson::WorkerContactInfo::WCMapPtr CzarRegistry::getWorkerContactMap() const {
    lock_guard lockG(_cmapMtx);
    return _contactMap;
}

void CzarRegistry::_registryUpdateLoop() {
    auto const method = http::Method::POST;
    string const url = "http://" + _czarConfig->replicationRegistryHost() + ":" +
                       to_string(_czarConfig->replicationRegistryPort()) + "/czar";
    vector<string> const headers = {"Content-Type: application/json"};
    json const request = json::object({{"instance_id", _czarConfig->replicationInstanceId()},
                                       {"auth_key", _czarConfig->replicationAuthKey()},
                                       {"czar",
                                        {{"name", _czarConfig->name()},
                                         {"id", _czarConfig->id()},
                                         {"management-port", _czarConfig->replicationHttpPort()},
                                         {"management-host-name", util::get_current_host_fqdn()}}}});
    string const requestContext = "Czar: '" + http::method2string(method) + "' request to '" + url + "'";
    LOGS(_log, LOG_LVL_TRACE,
         __func__ << " czarPost url=" << url << " request=" << request.dump() << " headers=" << headers[0]);
    http::Client client(method, url, request.dump(), headers);
    while (_loop) {
        LOGS(_log, LOG_LVL_TRACE,
             __func__ << " loop url=" << url << " request=" << request.dump() << " headers=" << headers[0]);
        try {
            json const response = client.readAsJson();
            if (0 == response.at("success").get<int>()) {
                string const error = response.at("error").get<string>();
                LOGS(_log, LOG_LVL_ERROR, requestContext + " was denied, error: '" + error + "'.");
                // TODO: Is there a better thing to do than just log this here?
            }
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_WARN, requestContext + " failed, ex: " + ex.what());
        }
        this_thread::sleep_for(chrono::seconds(max(1U, _czarConfig->replicationRegistryHearbeatIvalSec())));
    }
}

void CzarRegistry::_registryWorkerInfoLoop() {
    // Get worker information from the registry
    string const replicationInstanceId = _czarConfig->replicationInstanceId();
    string const replicationAuthKey = _czarConfig->replicationAuthKey();
    uint64_t const czarStartTime = Czar::czarStartupTime;

    vector<string> const headers;
    auto const method = http::Method::GET;
    string const url = "http://" + _czarConfig->replicationRegistryHost() + ":" +
                       to_string(_czarConfig->replicationRegistryPort()) +
                       "/services?instance_id=" + _czarConfig->replicationInstanceId();
    string const requestContext = "Czar: '" + http::method2string(method) + "' request to '" + url + "'";
    LOGS(_log, LOG_LVL_TRACE, __func__ << " url=" << url);
    http::Client client(method, url, string(), headers);
    while (_loop) {
        try {
            json const response = client.readAsJson();
            if (0 == response.at("success").get<int>()) {
                string const error = response.at("error").get<string>();
                LOGS(_log, LOG_LVL_ERROR, requestContext + " was denied, error: '" + error + "'.");
                // TODO: Is there a better thing to do than just log this here?
            } else {
                protojson::WorkerContactInfo::WCMapPtr wMap = _buildMapFromJson(response);
                // Update the values in the map
                {
                    auto czInfo = protojson::CzarContactInfo::create(
                            _czarConfig->name(), _czarConfig->id(), _czarConfig->replicationHttpPort(),
                            util::get_current_host_fqdn(), czarStartTime);
                    lock_guard lck(_cmapMtx);
                    if (wMap != nullptr) {
                        _contactMap = wMap;
                        _latestMapUpdate = CLOCK::now();
                        _activeWorkerMap->updateMap(*_contactMap, czInfo, replicationInstanceId,
                                                    replicationAuthKey);
                    }
                }
            }
            LOGS(_log, LOG_LVL_TRACE, __func__ << " resp=" << response);
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_WARN, requestContext + " failed, ex: " + ex.what());
        }
        this_thread::sleep_for(chrono::seconds(15));
    }
}

protojson::WorkerContactInfo::WCMapPtr CzarRegistry::_buildMapFromJson(nlohmann::json const& response) {
    auto const& jsServices = response.at("services");
    auto const& jsWorkers = jsServices.at("workers");
    auto wMap = protojson::WorkerContactInfo::WCMapPtr(new protojson::WorkerContactInfo::WCMap());
    for (auto const& [key, value] : jsWorkers.items()) {
        auto const& jsQserv = value.at("qserv");
        LOGS(_log, LOG_LVL_DEBUG, __func__ << " key=" << key << " jsQ=" << jsQserv);

        // The names for items here are different than the names used by workers.
        auto wInfo = protojson::WorkerContactInfo::createFromJsonRegistry(key, jsQserv);

        LOGS(_log, LOG_LVL_DEBUG, __func__ << " wInfot=" << wInfo->dump());
        auto iter = wMap->find(key);
        if (iter != wMap->end()) {
            LOGS(_log, LOG_LVL_ERROR, __func__ << " duplicate key " << key << " in " << response);
            if (!wInfo->isSameContactInfo(*(iter->second))) {
                LOGS(_log, LOG_LVL_ERROR, __func__ << " incongruent key " << key << " in " << response);
                return nullptr;
            }
            // ignore the duplicate, since it matches the previous one.
        } else {
            wMap->insert({key, wInfo});
        }
    }
    return wMap;
}

bool CzarRegistry::_compareMapContactInfo(protojson::WorkerContactInfo::WCMap const& other) const {
    VMUTEX_HELD(_cmapMtx);
    if (_contactMap == nullptr) {
        // If _contactMap is null, it needs to be replaced.
        return false;
    }
    if (other.size() != _contactMap->size()) {
        return false;
    }
    for (auto const& [key, wInfo] : *_contactMap) {
        auto iter = other.find(key);
        if (iter == other.end()) {
            return false;
        } else {
            if (!(iter->second->isSameContactInfo(*wInfo))) {
                return false;
            }
        }
    }
    return true;
}

protojson::WorkerContactInfo::WCMapPtr CzarRegistry::waitForWorkerContactMap() const {
    protojson::WorkerContactInfo::WCMapPtr contMap = nullptr;
    while (contMap == nullptr) {
        {
            lock_guard lockG(_cmapMtx);
            contMap = _contactMap;
        }
        if (contMap == nullptr) {
            // This should only ever happen at startup if there's trouble getting data.
            LOGS(_log, LOG_LVL_WARN, "waitForWorkerContactMap() _contactMap unavailable waiting for info");
            this_thread::sleep_for(1s);
        }
    }
    return contMap;
}

void CzarRegistry::sendActiveWorkersMessages() {
    // Send messages to each active worker as needed
    _activeWorkerMap->sendActiveWorkersMessages();
}

void CzarRegistry::endUserQueryOnWorkers(QueryId qId, bool deleteWorkerResults) {
    // Add query id to the appropriate list.
    if (deleteWorkerResults) {
        _activeWorkerMap->addToDoneDeleteFiles(qId);
    } else {
        _activeWorkerMap->addToDoneKeepFiles(qId);
    }

    // With lists updated, send out messages.
    _activeWorkerMap->sendActiveWorkersMessages();
}

}  // namespace lsst::qserv::czar
