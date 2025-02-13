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
#include "replica/config/ConfigWorker.h"

// System headers
#include <iostream>
#include <stdexcept>

// Qserv headers
#include "replica/config/ConfigParserUtils.h"

using namespace std;
using json = nlohmann::json;

namespace lsst::qserv::replica {

json ConfigQservWorker::toJson() const {
    json infoJson = json::object();
    infoJson["host"] = host.toJson();
    infoJson["port"] = port;
    return infoJson;
}

bool ConfigQservWorker::operator==(ConfigQservWorker const& other) const {
    return (host == other.host) && (port == other.port);
}

ConfigWorker::ConfigWorker(json const& obj) {
    string const context = "ConfigWorker::" + string(__func__) + "[json]: ";
    if (obj.empty()) return;
    if (!obj.is_object()) {
        throw invalid_argument(context + "a JSON object is required.");
    }
    try {
        parseRequired<string>(name, obj, "name");
        parseRequired<bool>(isEnabled, obj, "is-enabled");
        parseRequired<bool>(isReadOnly, obj, "is-read-only");
        parseRequired<string>(svcHost.addr, obj.at("svc-host"), "addr");
        parseRequired<string>(svcHost.name, obj.at("svc-host"), "name");
        parseOptional<uint16_t>(svcPort, obj, "svc-port");
        parseRequired<string>(httpSvcHost.addr, obj.at("http-svc-host"), "addr");
        parseRequired<string>(httpSvcHost.name, obj.at("http-svc-host"), "name");
        parseOptional<uint16_t>(httpSvcPort, obj, "http-svc-port");
        parseRequired<string>(fsHost.addr, obj.at("fs-host"), "addr");
        parseRequired<string>(fsHost.name, obj.at("fs-host"), "name");
        parseOptional<uint16_t>(fsPort, obj, "fs-port");
        parseOptional<string>(dataDir, obj, "data-dir");
        parseRequired<string>(loaderHost.addr, obj.at("loader-host"), "addr");
        parseRequired<string>(loaderHost.name, obj.at("loader-host"), "name");
        parseOptional<uint16_t>(loaderPort, obj, "loader-port");
        parseOptional<string>(loaderTmpDir, obj, "loader-tmp-dir");
        parseRequired<string>(exporterHost.addr, obj.at("exporter-host"), "addr");
        parseRequired<string>(exporterHost.name, obj.at("exporter-host"), "name");
        parseOptional<uint16_t>(exporterPort, obj, "exporter-port");
        parseOptional<string>(exporterTmpDir, obj, "exporter-tmp-dir");
        parseRequired<string>(httpLoaderHost.addr, obj.at("http-loader-host"), "addr");
        parseRequired<string>(httpLoaderHost.name, obj.at("http-loader-host"), "name");
        parseOptional<uint16_t>(httpLoaderPort, obj, "http-loader-port");
        parseOptional<string>(httpLoaderTmpDir, obj, "http-loader-tmp-dir");
        parseRequired<string>(qservWorker.host.addr, obj.at("qserv-worker").at("host"), "addr");
        parseRequired<string>(qservWorker.host.name, obj.at("qserv-worker").at("host"), "name");
        parseOptional<uint16_t>(qservWorker.port, obj.at("qserv-worker"), "port");
    } catch (exception const& ex) {
        throw invalid_argument(context + "the JSON object is not valid, ex: " + string(ex.what()));
    }
}

json ConfigWorker::toJson() const {
    json infoJson = json::object();
    infoJson["name"] = name;
    infoJson["is-enabled"] = isEnabled ? 1 : 0;
    infoJson["is-read-only"] = isReadOnly ? 1 : 0;
    infoJson["svc-host"] = svcHost.toJson();
    infoJson["svc-port"] = svcPort;
    infoJson["http-svc-host"] = httpSvcHost.toJson();
    infoJson["http-svc-port"] = httpSvcPort;
    infoJson["fs-host"] = fsHost.toJson();
    infoJson["fs-port"] = fsPort;
    infoJson["data-dir"] = dataDir;
    infoJson["loader-host"] = loaderHost.toJson();
    infoJson["loader-port"] = loaderPort;
    infoJson["loader-tmp-dir"] = loaderTmpDir;
    infoJson["exporter-host"] = exporterHost.toJson();
    infoJson["exporter-port"] = exporterPort;
    infoJson["exporter-tmp-dir"] = exporterTmpDir;
    infoJson["http-loader-host"] = httpLoaderHost.toJson();
    infoJson["http-loader-port"] = httpLoaderPort;
    infoJson["http-loader-tmp-dir"] = httpLoaderTmpDir;
    infoJson["qserv-worker"] = qservWorker.toJson();
    return infoJson;
}

bool ConfigWorker::operator==(ConfigWorker const& other) const {
    return (name == other.name) && (isEnabled == other.isEnabled) && (isReadOnly == other.isReadOnly) &&
           (svcHost == other.svcHost) && (svcPort == other.svcPort) && (httpSvcHost == other.httpSvcHost) &&
           (httpSvcPort == other.httpSvcPort) && (fsHost == other.fsHost) && (fsPort == other.fsPort) &&
           (dataDir == other.dataDir) && (loaderHost == other.loaderHost) &&
           (loaderPort == other.loaderPort) && (loaderTmpDir == other.loaderTmpDir) &&
           (exporterHost == other.exporterHost) && (exporterPort == other.exporterPort) &&
           (exporterTmpDir == other.exporterTmpDir) && (httpLoaderHost == other.httpLoaderHost) &&
           (httpLoaderPort == other.httpLoaderPort) && (httpLoaderTmpDir == other.httpLoaderTmpDir) &&
           (qservWorker == other.qservWorker);
}

ostream& operator<<(ostream& os, ConfigWorker const& info) {
    os << "ConfigWorker: " << info.toJson().dump();
    return os;
}

}  // namespace lsst::qserv::replica
