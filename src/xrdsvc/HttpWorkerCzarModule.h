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
#ifndef LSST_QSERV_XRDSVC_HTTPWORKERCZARMODULE_H
#define LSST_QSERV_XRDSVC_HTTPWORKERCZARMODULE_H

// System headers
#include <functional>
#include <memory>
#include <set>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "qmeta/types.h"
#include "xrdsvc/HttpModule.h"

namespace lsst::qserv::protojson {
class CzarContactInfo;
class UberJobMsg;
}  // namespace lsst::qserv::protojson

namespace lsst::qserv::qhttp {
class Request;
class Response;
}  // namespace lsst::qserv::qhttp

namespace lsst::qserv::wbase {
class UberJobData;
class UserQueryInfo;
}  // namespace lsst::qserv::wbase

namespace lsst::qserv::wcontrol {
class Foreman;
}  // namespace lsst::qserv::wcontrol

namespace lsst::qserv::xrdsvc {
class SsiProviderServer;
}  // namespace lsst::qserv::xrdsvc

// This header declarations
namespace lsst::qserv::xrdsvc {

/// This class handles Http message from the czar to the worker.
class HttpWorkerCzarModule : public xrdsvc::HttpModule {
public:
    /// @note supported values for parameter 'subModuleName' are:
    ///  'QUERYJOB'     - Convert an UberJob message into Tasks and a send channel.
    /// @throws std::invalid_argument for unknown values of parameter 'subModuleName'
    static void process(std::string const& context, std::shared_ptr<wcontrol::Foreman> const& foreman,
                        std::shared_ptr<qhttp::Request> const& req,
                        std::shared_ptr<qhttp::Response> const& resp, std::string const& subModuleName,
                        http::AuthType const authType = http::AuthType::NONE);

    HttpWorkerCzarModule() = delete;
    HttpWorkerCzarModule(HttpWorkerCzarModule const&) = delete;
    HttpWorkerCzarModule& operator=(HttpWorkerCzarModule const&) = delete;

    ~HttpWorkerCzarModule() final = default;

protected:
    virtual nlohmann::json executeImpl(std::string const& subModuleName) final;

private:
    HttpWorkerCzarModule(std::string const& context, std::shared_ptr<wcontrol::Foreman> const& foreman,
                         std::shared_ptr<qhttp::Request> const& req,
                         std::shared_ptr<qhttp::Response> const& resp);

    /// Handle an UberJob message from the czar to run it on this worker by calling _handleQueryJob.
    nlohmann::json _queryJob();

    /// Handle an UberJob message from the czar to run it on this worker, this does
    /// work of deciphering the message, creating UberJobData objects and Task objects.
    nlohmann::json _handleQueryJob(std::string const& func);

    static void _buildTasks(UberJobId ujId, QueryId ujQueryId,
                            std::shared_ptr<protojson::CzarContactInfo> const& ujCzInfo, int ujRowLimit,
                            uint64_t maxTableSizeBytes, std::string const& targetWorkerId,
                            std::shared_ptr<wbase::UserQueryInfo> const& userQueryInfo,
                            std::shared_ptr<protojson::UberJobMsg> const& uberJobMsg,
                            std::shared_ptr<wcontrol::Foreman> const& foremanPtr,
                            std::string const& authKeyStr, std::shared_ptr<wbase::UberJobData> const& ujData);

    /// Verify some aspects of the query and call _handleQueryStatus
    nlohmann::json _queryStatus();

    /// Reconstruct the message, absorb the lists into this worker's state,
    /// queue the ComIssue message and needed, and send the lists back to
    /// the czar.
    nlohmann::json _handleQueryStatus(std::string const& func);
};

}  // namespace lsst::qserv::xrdsvc

#endif  // LSST_QSERV_XRDSVC_HTTPWORKERCZARMODULE_H
