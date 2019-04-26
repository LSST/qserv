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
#include "replica/HttpModule.h"

// LSST headers
#include "lsst/log/Log.h"

// System headers
#include <stdexcept>

using namespace std;
using json = nlohmann::json;

namespace {
    LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.HttpModule");
}

namespace lsst {
namespace qserv {
namespace replica {

HttpModule::HttpModule(Controller::Ptr const& controller,
                       string const& taskName,
                       unsigned int workerResponseTimeoutSec)
    :   EventLogger(controller,
                    taskName),
        _workerResponseTimeoutSec(workerResponseTimeoutSec) {
}


void HttpModule::execute(qhttp::Request::Ptr const& req,
                         qhttp::Response::Ptr const& resp,
                         string const& subModuleName) {
    try {
        executeImpl(req, resp, subModuleName);
    } catch (invalid_argument const& ex) {
        sendError(resp, __func__, "invalid parameters of the request, ex: " + string(ex.what()));
    } catch (exception const& ex) {
        sendError(resp, __func__, "operation failed due to: " + string(ex.what()));
    }
}


string HttpModule::context() const {
    return name() + " ";
}


void HttpModule::info(string const& msg) const {
    LOGS(_log, LOG_LVL_INFO, context() << msg);
}


void HttpModule::debug(string const& msg) const {
    LOGS(_log, LOG_LVL_DEBUG, context() << msg);
}


void HttpModule::error(string const& msg) const {
    LOGS(_log, LOG_LVL_ERROR, context() << msg);
}


void HttpModule::sendError(qhttp::Response::Ptr const& resp,
                           string const& func,
                           string const& errorMsg) const {
    error(func, errorMsg);

    json result;
    result["success"] = 0;
    result["error"] = errorMsg;

    resp->send(result.dump(), "application/json");
}


void HttpModule::sendData(qhttp::Response::Ptr const& resp,
                          json& result,
                          bool success) {
    result["success"] = success ? 1 : 0;
    result["error"] = "";

    resp->send(result.dump(), "application/json");
}

}}}  // namespace lsst::qserv::replica
