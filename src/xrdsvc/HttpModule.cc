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
#include "xrdsvc/HttpModule.h"

// System headers
#include <stdexcept>

// Qserv headers
#include "http/Exceptions.h"
#include "http/RequestBodyJSON.h"
#include "http/RequestQuery.h"
#include "qhttp/Request.h"
#include "wbase/TaskState.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/Foreman.h"
#include "wpublish/ChunkInventory.h"

using namespace std;

namespace lsst::qserv::xrdsvc {

HttpModule::HttpModule(string const& context, shared_ptr<wcontrol::Foreman> const& foreman,
                       shared_ptr<qhttp::Request> const& req, shared_ptr<qhttp::Response> const& resp)
        : http::QhttpModule(wconfig::WorkerConfig::instance()->replicationAuthKey(),
                            wconfig::WorkerConfig::instance()->replicationAdminAuthKey(), req, resp),
          _context(context),
          _foreman(foreman) {}

string HttpModule::context() const { return _context; }

void HttpModule::enforceWorkerId(string const& func) const {
    string const workerIdAttrName = "worker";
    string workerId;
    if (req()->method == "GET") {
        if (!query().has(workerIdAttrName)) {
            throw http::Error(func, "No worker identifier was provided in the request query.");
        }
        workerId = query().requiredString(workerIdAttrName);
    } else {
        if (!body().has(workerIdAttrName)) {
            throw http::Error(func, "No worker identifier was provided in the request body.");
        }
        workerId = body().required<string>(workerIdAttrName);
    }
    string const expectedWorkerId = _foreman->chunkInventory()->id();
    if (expectedWorkerId != workerId) {
        string const msg = "Requested worker identifier '" + workerId + "' does not match the one '" +
                           expectedWorkerId + "' of the current worker.";
        throw http::Error(func, msg);
    }
}

wbase::TaskSelector HttpModule::translateTaskSelector(string const& func) const {
    wbase::TaskSelector selector;
    selector.includeTasks = query().optionalUInt("include_tasks", 0) != 0;
    selector.queryIds = query().optionalVectorUInt64("query_ids");
    string const taskStatesParam = "task_states";
    for (auto&& str : query().optionalVectorStr(taskStatesParam)) {
        try {
            auto const state = wbase::str2taskState(str);
            selector.taskStates.push_back(state);
            debug(func, "str='" + str + "', task state=" + wbase::taskState2str(state));
        } catch (exception const& ex) {
            string const msg =
                    "failed to parse query parameter '" + taskStatesParam + "', ex: " + string(ex.what());
            error(func, msg);
            throw invalid_argument(msg);
        }
    }
    selector.maxTasks = query().optionalUInt("max_tasks", 0);
    trace(func, "include_tasks=" + string(selector.includeTasks ? "1" : "0") +
                        " queryIds.size()=" + to_string(selector.queryIds.size()) + " taskStates.size()=" +
                        to_string(selector.taskStates.size()) + " max_tasks=" + to_string(selector.maxTasks));
    return selector;
}

}  // namespace lsst::qserv::xrdsvc
