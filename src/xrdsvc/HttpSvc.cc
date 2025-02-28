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
#include "xrdsvc/HttpSvc.h"

// System headers
#include <stdexcept>

// Qserv headers
#include "http/MetaModule.h"
#include "qhttp/Server.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/Foreman.h"
#include "wpublish/ChunkInventory.h"
#include "xrdsvc/HttpMonitorModule.h"
#include "xrdsvc/HttpReplicaMgtModule.h"
#include "xrdsvc/HttpWorkerCzarModule.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace nlohmann;
using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.HttpSvc");

string const serviceName = "WORKER-MANAGEMENT ";

}  // namespace

namespace lsst::qserv::xrdsvc {

shared_ptr<HttpSvc> HttpSvc::create(shared_ptr<wcontrol::Foreman> const& foreman, uint16_t port,
                                    unsigned int numThreads) {
    return shared_ptr<HttpSvc>(new HttpSvc(foreman, port, numThreads));
}

HttpSvc::HttpSvc(shared_ptr<wcontrol::Foreman> const& foreman, uint16_t port, unsigned int numThreads)
        : _foreman(foreman), _port(port), _numThreads(numThreads) {}

uint16_t HttpSvc::start() {
    string const context = "xrdsvc::HttpSvc::" + string(__func__) + " ";
    lock_guard<mutex> const lock(_mtx);
    if (_httpServerPtr != nullptr) {
        throw logic_error(context + "the service is already running.");
    }
    _httpServerPtr = qhttp::Server::create(_io_service, _port);

    auto const self = shared_from_this();

    // Make sure the handlers are registered and the server is started before
    // launching any BOOST ASIO threads. This will prevent threads from finishing
    // due to a lack of work to be done.
    _httpServerPtr->addHandlers(
            {{"GET", "/meta/version",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  json const info = json::object(
                          {{"kind", "qserv-worker-manager"},
                           {"id", self->_foreman->chunkInventory()->id()},
                           {"instance_id", wconfig::WorkerConfig::instance()->replicationInstanceId()}});
                  http::MetaModule::process(::serviceName, info, req, resp, "VERSION");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/config",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, self->_foreman, req, resp, "CONFIG");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/mysql",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, self->_foreman, req, resp, "MYSQL");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/status",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, self->_foreman, req, resp, "STATUS");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/files",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, self->_foreman, req, resp, "FILES");
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/echo",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, self->_foreman, req, resp, "ECHO");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/replicas",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpReplicaMgtModule::process(::serviceName, self->_foreman, req, resp, "GET");
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/replicas",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpReplicaMgtModule::process(::serviceName, self->_foreman, req, resp, "SET",
                                                http::AuthType::REQUIRED);
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/replica",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpReplicaMgtModule::process(::serviceName, self->_foreman, req, resp, "ADD",
                                                http::AuthType::REQUIRED);
              }}});
    _httpServerPtr->addHandlers(
            {{"DELETE", "/replica",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpReplicaMgtModule::process(::serviceName, self->_foreman, req, resp, "REMOVE",
                                                http::AuthType::REQUIRED);
              }}});
    _httpServerPtr->addHandlers(
            {{"PUT", "/inventory",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpReplicaMgtModule::process(::serviceName, self->_foreman, req, resp, "REBUILD",
                                                http::AuthType::REQUIRED);
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/queryjob",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpWorkerCzarModule::process(::serviceName, self->_foreman, req, resp, "/queryjob",
                                                http::AuthType::REQUIRED);
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/querystatus",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpWorkerCzarModule::process(::serviceName, self->_foreman, req, resp, "/querystatus",
                                                http::AuthType::REQUIRED);
              }}});
    _httpServerPtr->start();

    // Initialize the I/O context and start the service threads. At this point
    // the server will be ready to service incoming requests.
    for (unsigned int i = 0; i < _numThreads; ++i) {
        _threads.push_back(make_unique<thread>([self]() { self->_io_service.run(); }));
    }
    auto const actualPort = _httpServerPtr->getPort();
    LOGS(_log, LOG_LVL_INFO,
         context + "started on port " + to_string(actualPort) + " numThreads=" + to_string(_numThreads));
    return actualPort;
}

void HttpSvc::stop() {
    string const context = "xrdsvc::HttpSvc::" + string(__func__) + " ";
    lock_guard<mutex> const lock(_mtx);
    if (_httpServerPtr == nullptr) {
        throw logic_error(context + "the service is not running.");
    }

    // Stopping the server and resetting the I/O context will abort the ongoing
    // requests and unblock the service threads.
    _httpServerPtr->stop();
    _httpServerPtr = nullptr;
    _io_service.reset();
    for (auto&& t : _threads) {
        t->join();
    }
    _threads.clear();
    LOGS(_log, LOG_LVL_INFO, context + "stopped");
}

}  // namespace lsst::qserv::xrdsvc
