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
#include "czar/HttpModule.h"

// Forward declarations
namespace lsst::qserv::qhttp {
class Request;
class Response;
}  // namespace lsst::qserv::qhttp

// This header declarations
namespace lsst::qserv::czar {

/// &&& doc This class is used to handle messages to this czar from workers.
class HttpCzarWorkerModule : public czar::HttpModule {
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

    ///  &&& doc
    nlohmann::json _queryJobError();

    ///  &&& doc
    nlohmann::json _queryJobReady();

    ///  &&& doc
    nlohmann::json _handleJobReady(std::string const& func);

    nlohmann::json _handleJobError(std::string const& func);
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_HTTPCZARWORKERMODULE_H
