
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
#ifndef LSST_QSERV_HTTP_MODULE_H
#define LSST_QSERV_HTTP_MODULE_H

// System headers
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "http/BaseModule.h"

// Forward declarations
namespace lsst::qserv::http {
class RequestQuery;
}  // namespace lsst::qserv::http

// This header declarations
namespace lsst::qserv::http {

/**
 * Class Module is a specialization of the class BaseModule serving as an intermediate
 * base class for the simple request processing modules of the HTTP servers. Modules
 * in this hierachy do not allow uploading files or any other data in the streaming mode.
 */
class Module : public BaseModule {
public:
    Module() = delete;
    Module(Module const&) = delete;
    Module& operator=(Module const&) = delete;

    virtual ~Module() = default;
    virtual void execute(std::string const& subModuleName = std::string(),
                         http::AuthType const authType = http::AuthType::NONE);

protected:
    /**
     * @param authKey  An authorization key for operations which require extra security.
     * @param adminAuthKey  An administrator-level authorization key.
     */
    Module(std::string const& authKey, std::string const& adminAuthKey);

    /**
     * Get the raw body of a request if it's available and if the content type
     * meets expectations.
     * @note An assumption is made that the body is small enough to fit into memory.
     * @param content The content of the body is set of a request if all conditions are met.
     * @param requiredContentType The required content type of the body.
     */
    virtual void getRequestBody(std::string& content, std::string const& requiredContentType) = 0;

    /**
     * To implement a subclass-specific request processing.
     * @note All exceptions thrown by the implementations will be intercepted and
     *   reported as errors to callers. Exceptions are now the only way to report
     *   errors from modules.
     * @return A result to be sent back to a service requester in case of a successful
     *   completion of the requested operation.
     * @throws http::Error In case if a module needs to pass extra details
     *   on a error back to a service requester.
     */
    virtual nlohmann::json executeImpl(std::string const& subModuleName) = 0;

    /**
     * Send a response back to a requester of a service.
     * @param content The content to be sent back.
     * @param contentType The type of the content to be sent back.
     */
    virtual void sendResponse(std::string const& content, std::string const& contentType) = 0;

private:
    /**
     * Pull the raw request body and translate it into a JSON object.
     * @note The body will be set only if the request has a body and the content
     * type is "application/json". Otherwise the body will be left empty.
     */
    void _parseRequestBodyJSON();
};

}  // namespace lsst::qserv::http

#endif  // LSST_QSERV_HTTP_MODULE_H
