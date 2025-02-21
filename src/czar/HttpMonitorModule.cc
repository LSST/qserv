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
#include "czar/HttpMonitorModule.h"

// System headers
#include <set>
#include <stdexcept>

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "global/intTypes.h"
#include "http/Exceptions.h"
#include "http/RequestQuery.h"
#include "qdisp/CzarStats.h"
#include "util/String.h"

using namespace std;
using json = nlohmann::json;

namespace lsst::qserv::czar {

void HttpMonitorModule::process(string const& context, shared_ptr<qhttp::Request> const& req,
                                shared_ptr<qhttp::Response> const& resp, string const& subModuleName,
                                http::AuthType const authType) {
    HttpMonitorModule module(context, req, resp);
    module.execute(subModuleName, authType);
}

HttpMonitorModule::HttpMonitorModule(string const& context, shared_ptr<qhttp::Request> const& req,
                                     shared_ptr<qhttp::Response> const& resp)
        : QhttpModule(context, req, resp) {}

json HttpMonitorModule::executeImpl(string const& subModuleName) {
    string const func = string(__func__) + "[sub-module='" + subModuleName + "']";
    trace(func);
    enforceInstanceId(func, cconfig::CzarConfig::instance()->replicationInstanceId());
    enforceCzarName(func);
    if (subModuleName == "CONFIG")
        return _config();
    else if (subModuleName == "STATUS")
        return _status();
    else if (subModuleName == "QUERY-PROGRESS")
        return _queryProgress();
    throw invalid_argument(context() + func + " unsupported sub-module");
}

json HttpMonitorModule::_config() {
    debug(__func__);
    checkApiVersion(__func__, 29);
    return cconfig::CzarConfig::instance()->toJson();
}

json HttpMonitorModule::_status() {
    debug(__func__);
    checkApiVersion(__func__, 29);
    auto const stats = qdisp::CzarStats::get();
    return json::object(
            {{"qdisp_stats", stats->getQdispStatsJson()}, {"transmit_stats", stats->getTransmitStatsJson()}});
}

json HttpMonitorModule::_queryProgress() {
    debug(__func__);
    checkApiVersion(__func__, 29);
    auto queryIds = query().optionalVectorUInt64("query_ids");
    if (queryIds.empty()) {
        // Injecting 0 forces the query progress method to return info
        // on all known queries matching the (optionaly) specified age.
        queryIds.push_back(0);
    }
    unsigned int lastSeconds = query().optionalUInt("last_seconds", 0);
    debug(__func__, "query_ids=" + util::String::toString(queryIds));
    debug(__func__, "last_seconds=" + to_string(lastSeconds));

    auto const stats = qdisp::CzarStats::get();
    json queries = json::object();
    for (auto const id : queryIds) {
        QueryId const selectQueryId = id;
        for (auto&& [queryId, history] : stats->getQueryProgress(selectQueryId, lastSeconds)) {
            string const queryIdStr = to_string(queryId);
            queries[queryIdStr] = json::array();
            json& queryJson = queries[queryIdStr];
            for (auto&& point : history) {
                queryJson.push_back(json::array({point.timestampMs, point.numJobs}));
            }
        }
    }
    return json::object({{"queries", queries}});
}

}  // namespace lsst::qserv::czar
