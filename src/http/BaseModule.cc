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
#include "http/BaseModule.h"

// Qserv headers
#include "http/Exceptions.h"
#include "http/MetaModule.h"
#include "http/RequestQuery.h"

// LSST headers
#include "lsst/log/Log.h"

// System headers
#include <stdexcept>

using namespace std;
using json = nlohmann::json;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.http.BaseModule");

string packWarnings(list<string> const& warnings) {
    string packed;
    for (auto const& msg : warnings) {
        if (!packed.empty()) packed += "; ";
        packed += msg;
    }
    return packed;
}
}  // namespace

namespace lsst::qserv::http {

BaseModule::BaseModule(string const& authKey, string const& adminAuthKey)
        : _authKey(authKey), _adminAuthKey(adminAuthKey) {}

void BaseModule::checkApiVersion(string const& func, unsigned int minVersion, string const& warning) const {
    unsigned int const maxVersion = MetaModule::version;
    unsigned int version = 0;
    string const versionAttrName = "version";
    json const errorEx = json::object({{"min_version", minVersion}, {"max_version", maxVersion}});

    // Intercept exceptions thrown when converting the attribute's value (if provided)
    // in order to inject the allowed range of the version numbers into the extended
    // error sent back to the caller.
    //
    // Note that requests sent w/o explicitly specified API version will still be
    // processed. In this case a warning will be sent in the response object.
    try {
        if (method() == "GET") {
            if (!query().has(versionAttrName)) {
                warn("No version number was provided in the request's query.");
                return;
            }
            version = query().requiredUInt(versionAttrName);
        } else {
            if (!body().has(versionAttrName)) {
                warn("No version number was provided in the request's body.");
                return;
            }
            version = body().requiredUInt(versionAttrName);
        }
    } catch (...) {
        throw http::Error(func, "The required parameter " + versionAttrName + " is not a number.", errorEx);
    }
    if (!(minVersion <= version && version <= maxVersion)) {
        if (!warning.empty()) warn(warning);
        throw http::Error(func,
                          "The requested version " + to_string(version) +
                                  " of the API is not in the range supported by the service.",
                          errorEx);
    }
}

void BaseModule::enforceInstanceId(string const& func, string const& requiredInstanceId) const {
    string const instanceId = method() == "GET" ? query().requiredString("instance_id")
                                                : body().required<string>("instance_id");
    trace(func, "instance_id: " + instanceId);
    if (instanceId != requiredInstanceId) {
        throw invalid_argument(context() + func + " Qserv instance identifier mismatch. Client sent '" +
                               instanceId + "' instead of '" + requiredInstanceId + "'.");
    }
}

void BaseModule::trace(string const& msg) const { LOGS(_log, LOG_LVL_TRACE, context() << msg); }

void BaseModule::info(string const& msg) const { LOGS(_log, LOG_LVL_INFO, context() << msg); }

void BaseModule::debug(string const& msg) const { LOGS(_log, LOG_LVL_DEBUG, context() << msg); }

void BaseModule::warn(string const& msg) const {
    LOGS(_log, LOG_LVL_WARN, context() << msg);
    _warnings.push_back(msg);
}

void BaseModule::error(string const& msg) const { LOGS(_log, LOG_LVL_ERROR, context() << msg); }

void BaseModule::sendError(string const& func, string const& errorMsg, json const& errorExt) {
    error(func, errorMsg);
    json result;
    result["success"] = 0;
    result["error"] = errorMsg;
    result["error_ext"] = errorExt.is_null() ? json::object() : errorExt;
    result["warning"] = ::packWarnings(_warnings);
    sendResponse(result.dump(), "application/json");
}

void BaseModule::sendData(json& result) {
    result["success"] = 1;
    result["error"] = "";
    result["error_ext"] = json::object();
    result["warning"] = ::packWarnings(_warnings);
    sendResponse(result.dump(), "application/json");
}

void BaseModule::enforceAuthorization(http::AuthType const authType) {
    if (authType != http::AuthType::REQUIRED) return;
    if (body().has("admin_auth_key")) {
        auto const adminAuthKey = body().required<string>("admin_auth_key");
        if (adminAuthKey != _adminAuthKey) {
            throw AuthError(context() +
                            "administrator's authorization key 'admin_auth_key' in the request"
                            " doesn't match the one in server configuration");
        }
        _isAdmin = true;
        return;
    }
    if (body().has("auth_key")) {
        auto const authKey = body().required<string>("auth_key");
        if (authKey != _authKey) {
            throw AuthError(context() +
                            "authorization key 'auth_key' in the request doesn't match"
                            " the one in server configuration");
        }
        return;
    }
    throw AuthError(context() +
                    "none of the authorization keys 'auth_key' or 'admin_auth_key' was found"
                    " in the request. Please, provide one.");
}

}  // namespace lsst::qserv::http
