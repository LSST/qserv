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
#include "czar/HttpSvc.h"

// System headers
#include <stdexcept>

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "czar/HttpMonitorModule.h"
#include "czar/HttpCzarWorkerModule.h"
#include "http/MetaModule.h"
#include "qhttp/Server.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace nlohmann;
using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.HttpSvc");

string const serviceName = "CZAR-MANAGEMENT ";

}  // namespace

namespace lsst::qserv::czar {

shared_ptr<HttpSvc> HttpSvc::create(uint16_t port, unsigned int numThreads) {
    return shared_ptr<HttpSvc>(new HttpSvc(port, numThreads));
}

HttpSvc::HttpSvc(uint16_t port, unsigned int numThreads) : _port(port), _numThreads(numThreads) {}

uint16_t HttpSvc::start() {
    string const context = "czar::HttpSvc::" + string(__func__) + " ";
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
                          {{"kind", "qserv-czar-manager"},
                           {"id", cconfig::CzarConfig::instance()->id()},
                           {"instance_id", cconfig::CzarConfig::instance()->replicationInstanceId()}});
                  http::MetaModule::process(::serviceName, info, req, resp, "VERSION");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/config",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, req, resp, "CONFIG");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/query-progress",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, req, resp, "QUERY-PROGRESS");
              }}});
    _httpServerPtr->addHandlers(
            {{"GET", "/status",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpMonitorModule::process(::serviceName, req, resp, "STATUS");
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/queryjob-error",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpCzarWorkerModule::process(::serviceName, req, resp, "QUERYJOB-ERROR");
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/queryjob-ready",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpCzarWorkerModule::process(::serviceName, req, resp, "QUERYJOB-READY");
              }}});
    _httpServerPtr->addHandlers(
            {{"POST", "/workerczarcomissue",
              [self](shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp) {
                  HttpCzarWorkerModule::process(::serviceName, req, resp, "WORKERCZARCOMISSUE");
              }}});
    _httpServerPtr->start();

    // Initialize the I/O context and start the service threads. At this point
    // the server will be ready to service incoming requests.
    for (unsigned int i = 0; i < _numThreads; ++i) {
        _threads.push_back(make_unique<thread>([self]() { self->_io_service.run(); }));
    }
    auto const actualPort = _httpServerPtr->getPort();
    LOGS(_log, LOG_LVL_INFO, context + "started on port " + to_string(actualPort));
    return actualPort;
}

void HttpSvc::stop() {
    string const context = "czar::HttpSvc::" + string(__func__) + " ";
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

}  // namespace lsst::qserv::czar
