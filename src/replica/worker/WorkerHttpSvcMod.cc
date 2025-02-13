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
#include "replica/worker/WorkerHttpSvcMod.h"

// System headers
#include <stdexcept>

// Third-party headers
#include <httplib.h>

// Qserv header
#include "http/Method.h"
#include "replica/proto/Protocol.h"
#include "replica/worker/WorkerHttpProcessor.h"
#include "replica/services/ServiceProvider.h"

using namespace std;
using json = nlohmann::json;

namespace lsst::qserv::replica {

void WorkerHttpSvcMod::process(shared_ptr<ServiceProvider> const& serviceProvider,
                               shared_ptr<WorkerHttpProcessor> const& processor, string const& workerName,
                               httplib::Request const& req, httplib::Response& resp,
                               string const& subModuleName, http::AuthType const authType) {
    WorkerHttpSvcMod module(serviceProvider, processor, workerName, req, resp);
    module.execute(subModuleName, authType);
}

WorkerHttpSvcMod::WorkerHttpSvcMod(shared_ptr<ServiceProvider> const& serviceProvider,
                                   shared_ptr<WorkerHttpProcessor> const& processor, string const& workerName,
                                   httplib::Request const& req, httplib::Response& resp)
        : http::ChttpModule(serviceProvider->authKey(), serviceProvider->adminAuthKey(), req, resp),
          _serviceProvider(serviceProvider),
          _processor(processor),
          _workerName(workerName) {}

string WorkerHttpSvcMod::context() const { return "WORKER-HTTP-SVC "; }

json WorkerHttpSvcMod::executeImpl(string const& subModuleName) {
    debug(__func__, "subModuleName: '" + subModuleName + "'");
    enforceInstanceId(__func__, _serviceProvider->instanceId());
    if (subModuleName == "ECHO")
        return _echo();
    else if (subModuleName == "REPLICA-CREATE")
        return _replicaCreate();
    else if (subModuleName == "REPLICA-DELETE")
        return _replicaDelete();
    else if (subModuleName == "REPLICA-FIND")
        return _replicaFind();
    else if (subModuleName == "REPLICA-FIND-ALL")
        return _replicaFindAll();
    else if (subModuleName == "SQL")
        return _sql();
    else if (subModuleName == "INDEX")
        return _index();
    else if (subModuleName == "REQUEST-TRACK")
        return _requestTrack();
    else if (subModuleName == "REQUEST-STATUS")
        return _requestStatus();
    else if (subModuleName == "REQUEST-STOP")
        return _requestStop();
    else if (subModuleName == "REQUEST-DISPOSE")
        return _requestDispose();
    else if (subModuleName == "SERVICE-SUSPEND")
        return _serviceSuspend();
    else if (subModuleName == "SERVICE-RESUME")
        return _serviceResume();
    else if (subModuleName == "SERVICE-STATUS")
        return _serviceStatus();
    else if (subModuleName == "SERVICE-REQUESTS")
        return _serviceRequests();
    else if (subModuleName == "SERVICE-DRAIN")
        return _serviceDrain();
    else if (subModuleName == "SERVICE-RECONFIG")
        return _serviceReconfig();
    throw invalid_argument(context() + "::" + string(__func__) + "  unsupported sub-module: '" +
                           subModuleName + "'");
}

protocol::QueuedRequestHdr WorkerHttpSvcMod::_parseHdr(string const& func) const {
    protocol::QueuedRequestHdr const hdr(body().required<string>("id"), body().optional<int>("priority", 0),
                                         body().optional<unsigned int>("timeout", 0));
    debug(func, "id:       '" + hdr.id + "'");
    debug(func, "priority: " + to_string(hdr.priority));
    debug(func, "timeout:  " + to_string(hdr.timeout));
    return hdr;
}

json WorkerHttpSvcMod::_echo() const {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->echo(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_replicaCreate() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->createReplica(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_replicaDelete() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->deleteReplica(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_replicaFind() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->findReplica(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_replicaFindAll() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->findAllReplicas(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_index() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->index(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_sql() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->sql(_parseHdr(__func__), body().required<json>("req"));
}

json WorkerHttpSvcMod::_requestTrack() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    string const id = params().at("id");
    debug(__func__, "id: '" + id + "'");
    return _processor->trackRequest(id);
}

json WorkerHttpSvcMod::_requestStatus() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    string const id = params().at("id");
    debug(__func__, "id: '" + id + "'");
    return _processor->requestStatus(id);
}

json WorkerHttpSvcMod::_requestStop() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    string const id = params().at("id");
    debug(__func__, "id: '" + id + "'");
    return _processor->stopRequest(id);
}

json WorkerHttpSvcMod::_requestDispose() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    auto const idsJson = body().required<json>("ids");
    if (!idsJson.is_array())
        throw invalid_argument(context() + "::" + string(__func__) + "  'ids' is not an array");

    json idsDisposedJson = json::object();
    for (auto const& idJson : idsJson) {
        string const id = idJson.get<string>();
        idsDisposedJson[id] = _processor->disposeRequest(id) ? 1 : 0;
    }
    return json::object({{"status", protocol::Status::SUCCESS},
                         {"status_str", protocol::toString(protocol::Status::SUCCESS)},
                         {"status_ext", protocol::StatusExt::NONE},
                         {"status_ext_str", protocol::toString(protocol::StatusExt::NONE)},
                         {"ids_disposed", idsDisposedJson}});
}

json WorkerHttpSvcMod::_serviceSuspend() {
    debug(__func__);
    checkApiVersion(__func__, 40);

    // This operation is allowed to be asynchronous as it may take
    // extra time for the processor's threads to finish on-going processing
    _processor->stop();
    return _processor->toJson(_processor->state() == protocol::ServiceState::RUNNING
                                      ? protocol::Status::FAILED
                                      : protocol::Status::SUCCESS);
}

json WorkerHttpSvcMod::_serviceResume() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    _processor->run();
    return _processor->toJson(_processor->state() == protocol::ServiceState::RUNNING
                                      ? protocol::Status::SUCCESS
                                      : protocol::Status::FAILED);
}

json WorkerHttpSvcMod::_serviceStatus() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    return _processor->toJson(protocol::Status::SUCCESS);
}

json WorkerHttpSvcMod::_serviceRequests() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    const bool includeRequests = true;
    return _processor->toJson(protocol::Status::SUCCESS, includeRequests);
}

json WorkerHttpSvcMod::_serviceDrain() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    _processor->drain();
    const bool includeRequests = true;
    return _processor->toJson(protocol::Status::SUCCESS, includeRequests);
}

json WorkerHttpSvcMod::_serviceReconfig() {
    debug(__func__);
    checkApiVersion(__func__, 40);
    _processor->reconfig();
    return _processor->toJson(protocol::Status::SUCCESS);
}

}  // namespace lsst::qserv::replica
