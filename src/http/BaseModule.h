
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
#ifndef LSST_QSERV_HTTP_BASEMODULE_H
#define LSST_QSERV_HTTP_BASEMODULE_H

// System headers
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "http/RequestBodyJSON.h"

// Forward declarations
namespace lsst::qserv::http {
class RequestQuery;
}  // namespace lsst::qserv::http

// This header declarations
namespace lsst::qserv::http {

/// The enumeration type which is used for configuring/enforcing
/// module's authorization requirements.
enum class AuthType { REQUIRED, NONE };

/// Class AuthError represent exceptions thrown when the authorization
/// requirements aren't met.
class AuthError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

/**
 * Class BaseModule is the very base class for the request processing modules of the HTTP servers.
 */
class BaseModule {
public:
    BaseModule() = delete;
    BaseModule(BaseModule const&) = delete;
    BaseModule& operator=(BaseModule const&) = delete;

    virtual ~BaseModule() = default;

    /**
     * Invokes a subclass-specific request processing provided by implementations.
     *
     * @param subModuleName  this optional parameter allows modules to have
     *   multiple sub-modules. A value of this parameter will be forwarded to
     *   the subclass-specific implementation of the pure virtual method
     *   Module::executeImpl().
     * @param authType  Authorization requirements of the module. If 'http::AuthType::REQUIRED' is
     *   requested then the method will enforce the authorization. A lack of required
     *   authorization key in a request, or an incorrect value of such key would result
     *   in a error sent back to a client.
     *
     * @note For requests with 'http::AuthType::REQUIRED' authorization keys must be sent
     *   by a requestor in the body of a request. There are two types of keys. The normal
     *   authorization level key "auth_key" is required for most operations resulting
     *   in any changes made to a persistent or transient states of Qserv, and its
     *   Replication/Ingest systems. The key is also required when requesting sensitive
     *   information from the system. The "administrator"-level "admin_auth_key" superseeds
     *   "auth_key" by adding elevated privileges to requests. If "admin_auth_key" is found
     *   in the body then "auth_key" (if any provided) will be ignored, and it won't be
     *   validated if present. It's up to a specific module to decide on how to (or if)
     *   use the administrative privileges.
     */
    virtual void execute(std::string const& subModuleName = std::string(),
                         http::AuthType const authType = http::AuthType::NONE) = 0;

protected:
    /**
     * @param authKey  An authorization key for operations which require extra security.
     * @param adminAuthKey  An administrator-level authorization key.
     */
    BaseModule(std::string const& authKey, std::string const& adminAuthKey);

    /// @return Authorization level of the request.
    bool isAdmin() const { return _isAdmin; }

    /// @return The method of a request.
    virtual std::string method() const = 0;

    /// @return Captured URL path elements.
    virtual std::unordered_map<std::string, std::string> params() const = 0;

    /// @return Parameters of the request's query captured from the request's URL.
    virtual RequestQuery query() const = 0;

    /// @return Optional parameters of a request extracted from the request's body (if any).
    RequestBodyJSON const& body() const { return _body; }

    /// @return A reference to the modifiable object that stores optional parameters of a request
    /// extracted from the request's body (if any). The method is used by subclasses to set the
    /// body of a request.
    RequestBodyJSON& body() { return _body; }

    // Message loggers for the corresponding log levels
    // TODO:UJ all of these defeat the purpose of using macros, warn and error are probably fine to keep
    //         as they should rarely be called.
    void trace(std::string const& msg) const;
    void trace(std::string const& context, std::string const& msg) const { trace(context + "  " + msg); }

    void info(std::string const& msg) const;
    void info(std::string const& context, std::string const& msg) const { info(context + "  " + msg); }

    void debug(std::string const& msg) const;
    void debug(std::string const& context, std::string const& msg) const { debug(context + "  " + msg); }

    void warn(std::string const& msg) const;
    void warn(std::string const& context, std::string const& msg) const { warn(context + "  " + msg); }

    void error(std::string const& msg) const;
    void error(std::string const& context, std::string const& msg) const { error(context + "  " + msg); }

    /**
     * @return A context in which a module runs. This is used for error adn info reporting.
     *   The method is required to be implemented by a subclass.
     */
    virtual std::string context() const = 0;

    /**
     * @brief Check the API version in the request's query or its body.
     *
     * The version is specified in the optional attribute 'version'. If the attribute
     * was found present in the request then its value would be required to be within
     * the specified minimum and the implied maximum, that's the current version number
     * of the REST API. In case if no version info was found in the request the method
     * will simply note this and the service will report a lack of the version number
     * in the "warning" attribute at the returned JSON object.
     *
     * The method will look for th eversion attribute in the query string of the "GET"
     * requests. For requests that are called using methods "POST", "PUT" or "DELETE"
     * the attribute will be located in the requests's body.
     *
     * @note Services that are calling the method should adjust the minimum version
     *   number to be the same as the current value in the implementation of
     *   http::MetaModule::version if the expected JSON schema of the corresponding
     *   request changes.
     * @see http::MetaModule::version
     *
     * @param func The name of the calling context (it's used for error reporting).
     * @param minVersion The minimum version number of the valid version range.
     * @param warning The optional warning to be sent to a client along with the usual
     *   error if the minimum version requirement won't be satisfied. This mechanism
     *   allows REST serivices to notify clients on possible problems encountered
     *   when validating parameters of a request.
     *
     * @throw http::Error if a value of the attribute is not within the expected range.
     */
    void checkApiVersion(std::string const& func, unsigned int minVersion,
                         std::string const& warning = std::string()) const;

    /**
     * @brief Check if the specified identifier of the Qserv instance that was received
     *   from a client matches the one that is required in the service context. Throw
     *   an exception in case of mismatch.
     *
     * @param func The name of the calling context (it's used for error reporting).
     * @param requiredInstanceId An instance identifier required in the service context.
     * @throws std::invalid_argument If the dentifiers didn't match.
     */
    void enforceInstanceId(std::string const& func, std::string const& requiredInstanceId) const;

    /**
     * Send a response back to a requester of a service.
     * @param content The content to be sent back.
     * @param contentType The type of the content to be sent back.
     */
    virtual void sendResponse(std::string const& content, std::string const& contentType) = 0;

    /**
     * Inspect the body of a request or a presence of a user-supplied authorization key.
     * Its value will be compared against a value of the corresponding configuration
     * parameter of the service (processorConfig) passed into the constructor of the class.
     * In the absence of the message body, or in the absence of the key in the body, or
     * in case of any mismatch between the keys would result in an exception thrown.
     *
     * @param authType  Authorization requirements of the module. If 'http::AuthType::REQUIRED' is
     *   requested then the method will enforce the authorization. A lack of required
     *   authorization key in a request, or an incorrect value of such key would result
     *   in a error sent back to a client.
     * @throw AuthError This exception is thrown if the authorization requirements weren't met.
     */
    void enforceAuthorization(http::AuthType const authType = http::AuthType::NONE);

    /**
     * Report a error condition and send an error message back to a requester
     * of a service.
     *
     * @param func The name of a context from which the operation was initiated.
     * @param errorMsg An error condition to be reported.
     * @param errorExt (optional) The additional information on the error.
     */
    void sendError(std::string const& func, std::string const& errorMsg,
                   nlohmann::json const& errorExt = nlohmann::json::object());

    /**
     * Report a result back to a requester of a service upon its successful
     * completion.
     * @param result A JSON object to be sent back.
     */
    void sendData(nlohmann::json& result);

    std::string authKey() const { return _authKey; }

private:
    // Input parameters
    std::string const _authKey;
    std::string const _adminAuthKey;

    /// The flag indicating if a request has been granted the "administrator"-level privileges.
    bool _isAdmin = false;

    /// The body of a request is initialized by BaseModule::execute().
    RequestBodyJSON _body;

    /// The optional warning message to be sent to a caller if the API version
    /// number wasn't mentoned in the request.
    mutable std::list<std::string> _warnings;
};

}  // namespace lsst::qserv::http

#endif  // LSST_QSERV_HTTP_BASEMODULE_H
