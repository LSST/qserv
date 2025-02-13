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
#include "http/ChttpMetaModule.h"

// System headers
#include <stdexcept>

using namespace std;
using json = nlohmann::json;

namespace {
// Authorization context is not required by this module
lsst::qserv::http::AuthType const authType = lsst::qserv::http::AuthType::NONE;
string const authKey;
string const adminAuthKey;
}  // namespace

namespace lsst::qserv::http {

unsigned int const ChttpMetaModule::version = 40;

void ChttpMetaModule::process(string const& context, nlohmann::json const& info, httplib::Request const& req,
                              httplib::Response& resp, string const& subModuleName) {
    ChttpMetaModule module(context, info, req, resp);
    module.execute(subModuleName, ::authType);
}

ChttpMetaModule::ChttpMetaModule(string const& context, nlohmann::json const& info,
                                 httplib::Request const& req, httplib::Response& resp)
        : http::ChttpModule(::authKey, ::adminAuthKey, req, resp), _context(context), _info(info) {
    if (!_info.is_object()) {
        throw invalid_argument("ChttpMetaModule::" + string(__func__) + " parameter info must be an object.");
    }
}

json ChttpMetaModule::executeImpl(string const& subModuleName) {
    if (subModuleName == "VERSION") return _version();
    throw invalid_argument(context() + "::" + string(__func__) + "  unsupported sub-module: '" +
                           subModuleName + "'");
}

string ChttpMetaModule::context() const { return _context; }

json ChttpMetaModule::_version() {
    debug(__func__);
    json result = _info;
    result["version"] = ChttpMetaModule::version;
    return result;
}

}  // namespace lsst::qserv::http
