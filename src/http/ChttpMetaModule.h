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
#ifndef LSST_QSERV_HTTP_CHTTPMETAMODULE_H
#define LSST_QSERV_HTTP_CHTTPMETAMODULE_H

// System headers
#include <memory>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "http/ChttpModule.h"

// This header declarations
namespace lsst::qserv::http {

/**
 * Class ChttpMetaModule implements a handler for the metadata queries on the REST API itself.
 * The service responds with an information object provided at the creation time of the module.
 */
class ChttpMetaModule : public http::ChttpModule {
public:
    typedef std::shared_ptr<ChttpMetaModule> Ptr;

    /// The current version of the REST API
    static unsigned int const version;

    /**
     * @note supported values for parameter 'subModuleName' are:
     *   'VERSION' - return a version of the REST API
     *
     * @param info The information object to be returned to clients of the service.
     * @throws std::invalid_argument for unknown values of parameter 'subModuleName'
     */
    static void process(std::string const& context, nlohmann::json const& info, httplib::Request const& req,
                        httplib::Response& resp, std::string const& subModuleName);

    ChttpMetaModule() = delete;
    ChttpMetaModule(ChttpMetaModule const&) = delete;
    ChttpMetaModule& operator=(ChttpMetaModule const&) = delete;

    ~ChttpMetaModule() final = default;

protected:
    virtual nlohmann::json executeImpl(std::string const& subModuleName) final;
    virtual std::string context() const final;

private:
    ChttpMetaModule(std::string const& context, nlohmann::json const& info, httplib::Request const& req,
                    httplib::Response& resp);

    nlohmann::json _version();

    std::string const _context;
    nlohmann::json const _info;
};

}  // namespace lsst::qserv::http

#endif  // LSST_QSERV_HTTP_CHTTPMETAMODULE_H
