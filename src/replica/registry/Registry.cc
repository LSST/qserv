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
#include "replica/registry/Registry.h"

// Qserv headers
#include "http/Client.h"
#include "http/MetaModule.h"
#include "qmeta/types.h"
#include "replica/config/Configuration.h"
#include "replica/config/ConfigWorker.h"
#include "util/common.h"

// LSST headers
#include "lsst/log/Log.h"

// Standard headers
#include <stdexcept>
#include <string.h>
#include <thread>

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.Registry");

string context(string const& func) { return "REGISTRY " + func + " "; }

}  // namespace

namespace lsst::qserv::replica {

Registry::Ptr Registry::create(ServiceProvider::Ptr const& serviceProvider) {
    return Ptr(new Registry(serviceProvider));
}

Registry::Registry(ServiceProvider::Ptr const& serviceProvider)
        : _serviceProvider(serviceProvider),
          _baseUrl("http://" + serviceProvider->config()->get<string>("registry", "host") + ":" +
                   to_string(serviceProvider->config()->get<uint16_t>("registry", "port"))) {}

vector<ConfigWorker> Registry::workers() const {
    vector<ConfigWorker> coll;
    string const resource = "/services?instance_id=" + _serviceProvider->instanceId() +
                            "&version=" + to_string(http::MetaModule::version);
    json const resultJson = _request(http::Method::GET, resource);
    for (auto const& [workerName, workerJson] : resultJson.at("services").at("workers").items()) {
        ConfigWorker worker;
        if (_serviceProvider->config()->isKnownWorker(workerName)) {
            worker = _serviceProvider->config()->worker(workerName);
        } else {
            worker.name = workerName;
        }
        if (workerJson.contains("replication")) {
            json const& replicationWorker = workerJson.at("replication");
            string const hostAddr = replicationWorker.at("host-addr").get<string>();
            worker.svcHost.addr = hostAddr;
            worker.svcHost.name = replicationWorker.at("svc-host-name").get<string>();
            worker.svcPort = replicationWorker.at("svc-port").get<uint16_t>();
            worker.httpSvcHost.addr = hostAddr;
            worker.httpSvcHost.name = replicationWorker.at("http-svc-host-name").get<string>();
            worker.httpSvcPort = replicationWorker.at("http-svc-port").get<uint16_t>();
            worker.fsHost.addr = hostAddr;
            worker.fsHost.name = replicationWorker.at("fs-host-name").get<string>();
            worker.fsPort = replicationWorker.at("fs-port").get<uint16_t>();
            worker.dataDir = replicationWorker.at("data-dir").get<string>();
            worker.loaderHost.addr = hostAddr;
            worker.loaderHost.name = replicationWorker.at("loader-host-name").get<string>();
            worker.loaderPort = replicationWorker.at("loader-port").get<uint16_t>();
            worker.loaderTmpDir = replicationWorker.at("loader-tmp-dir").get<string>();
            worker.exporterHost.addr = hostAddr;
            worker.exporterHost.name = replicationWorker.at("exporter-host-name").get<string>();
            worker.exporterPort = replicationWorker.at("exporter-port").get<uint16_t>();
            worker.exporterTmpDir = replicationWorker.at("exporter-tmp-dir").get<string>();
            worker.httpLoaderHost.addr = hostAddr;
            worker.httpLoaderHost.name = replicationWorker.at("http-loader-host-name").get<string>();
            worker.httpLoaderPort = replicationWorker.at("http-loader-port").get<uint16_t>();
            worker.httpLoaderTmpDir = replicationWorker.at("http-loader-tmp-dir").get<string>();
        }
        if (workerJson.contains("qserv")) {
            json const& qservWorker = workerJson.at("qserv");
            worker.qservWorker.host.addr = qservWorker.at("host-addr").get<string>();
            worker.qservWorker.host.name = qservWorker.at("management-host-name").get<string>();
            worker.qservWorker.port = qservWorker.at("management-port").get<uint16_t>();
        }
        coll.push_back(std::move(worker));
    }
    return coll;
}

void Registry::addWorker(string const& name) const {
    bool const all = true;
    string const hostName = util::get_current_host_fqdn(all);
    auto const config = _serviceProvider->config();
    json const request =
            json::object({{"version", http::MetaModule::version},
                          {"instance_id", _serviceProvider->instanceId()},
                          {"auth_key", _serviceProvider->authKey()},
                          {"worker",
                           {{"name", name},
                            {"svc-host-name", hostName},
                            {"svc-port", config->get<uint16_t>("worker", "svc-port")},
                            {"http-svc-host-name", hostName},
                            {"http-svc-port", config->get<uint16_t>("worker", "http-svc-port")},
                            {"fs-host-name", hostName},
                            {"fs-port", config->get<uint16_t>("worker", "fs-port")},
                            {"data-dir", config->get<string>("worker", "data-dir")},
                            {"loader-host-name", hostName},
                            {"loader-port", config->get<uint16_t>("worker", "loader-port")},
                            {"loader-tmp-dir", config->get<string>("worker", "loader-tmp-dir")},
                            {"exporter-host-name", hostName},
                            {"exporter-port", config->get<uint16_t>("worker", "exporter-port")},
                            {"exporter-tmp-dir", config->get<string>("worker", "exporter-tmp-dir")},
                            {"http-loader-host-name", hostName},
                            {"http-loader-port", config->get<uint16_t>("worker", "http-loader-port")},
                            {"http-loader-tmp-dir", config->get<string>("worker", "http-loader-tmp-dir")}}}});
    _request(http::Method::POST, "/worker", request);
}

void Registry::removeWorker(string const& name) const {
    json const request = json::object(
            {{"instance_id", _serviceProvider->instanceId()}, {"auth_key", _serviceProvider->authKey()}});
    _request(http::Method::DELETE, "/worker/" + name, request);
}

vector<ConfigCzar> Registry::czars() const {
    vector<ConfigCzar> coll;
    string const resource = "/services?instance_id=" + _serviceProvider->instanceId();
    json const resultJson = _request(http::Method::GET, resource);
    for (auto const& [czarName, czarJson] : resultJson.at("services").at("czars").items()) {
        ConfigCzar czar;
        if (_serviceProvider->config()->isKnownCzar(czarName)) {
            czar = _serviceProvider->config()->czar(czarName);
        } else {
            czar.name = czarName;
        }
        czar.id = czarJson.at("id").get<qmeta::CzarId>();
        czar.host.addr = czarJson.at("host-addr").get<string>();
        czar.host.name = czarJson.at("management-host-name").get<string>();
        czar.port = czarJson.at("management-port").get<uint16_t>();
        coll.push_back(std::move(czar));
    }
    return coll;
}

json Registry::_request(http::Method method, string const& resource, json const& request) const {
    string const url = _baseUrl + resource;
    vector<string> const headers =
            request.empty() ? vector<string>({}) : vector<string>({"Content-Type: application/json"});
    http::Client client(method, url, request.empty() ? string() : request.dump(), headers);
    json const response = client.readAsJson();
    if (0 == response.at("success").get<int>()) {
        string const msg = ::context(__func__) + "'" + http::method2string(method) + "' request to '" + url +
                           "' failed, error: '" + response.at("error").get<string>() + "'.";
        LOGS(_log, LOG_LVL_ERROR, msg);
        throw runtime_error(msg);
    }
    return response;
}

}  // namespace lsst::qserv::replica
