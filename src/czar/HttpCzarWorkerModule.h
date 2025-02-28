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
#ifndef LSST_QSERV_CZAR_HTTPCZARWORKERMODULE_H
#define LSST_QSERV_CZAR_HTTPCZARWORKERMODULE_H

// System headers
#include <memory>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "czar/QhttpModule.h"

// Forward declarations
namespace lsst::qserv::qhttp {
class Request;
class Response;
}  // namespace lsst::qserv::qhttp

// This header declarations
namespace lsst::qserv::czar {

/// This class is used to handle messages to this czar from the workers.
class HttpCzarWorkerModule : public QhttpModule {
public:
    /// @note supported values for parameter 'subModuleName' are:
    ///   'QUERYJOB-ERROR' - error in a QUERYJOB
    ///   'QUERYJOB-READY' -
    /// @throws std::invalid_argument for unknown values of parameter 'subModuleName'
    static void process(std::string const& context, std::shared_ptr<qhttp::Request> const& req,
                        std::shared_ptr<qhttp::Response> const& resp, std::string const& subModuleName,
                        http::AuthType const authType = http::AuthType::NONE);

    HttpCzarWorkerModule() = delete;
    HttpCzarWorkerModule(HttpCzarWorkerModule const&) = delete;
    HttpCzarWorkerModule& operator=(HttpCzarWorkerModule const&) = delete;

    ~HttpCzarWorkerModule() final = default;

protected:
    nlohmann::json executeImpl(std::string const& subModuleName) final;

private:
    HttpCzarWorkerModule(std::string const& context, std::shared_ptr<qhttp::Request> const& req,
                         std::shared_ptr<qhttp::Response> const& resp);

    /// Called to handle message indicating this czar needs to handle an error on a worker.
    nlohmann::json _queryJobError();

    /// Called to indicate an UberJob is ready with data that needs to be collected.
    nlohmann::json _queryJobReady();

    /// Called to indicate there were problems with the worker trying to reach this czar.
    nlohmann::json _workerCzarComIssue();

    /// Translates the message and calls the Czar to collect the data.
    nlohmann::json _handleJobReady(std::string const& func);

    /// Translates the error and calls the Czar to take action.
    nlohmann::json _handleJobError(std::string const& func);

    /// Translates the issues and calls the Czar to take action.
    nlohmann::json _handleWorkerCzarComIssue(std::string const& func);
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_HTTPCZARWORKERMODULE_H
