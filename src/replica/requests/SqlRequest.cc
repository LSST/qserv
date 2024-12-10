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
#include "replica/requests/SqlRequest.h"

// System headers
#include <functional>
#include <stdexcept>

// Third party headers
#include "boost/date_time/posix_time/posix_time.hpp"

// Qserv headers
#include "replica/contr/Controller.h"
#include "replica/requests/Messenger.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "util/String.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.SqlRequest");
bool const disposeRequired = true;
}  // namespace

namespace lsst::qserv::replica {

void SqlRequest::extendedPrinter(Ptr const& ptr) {
    Request::defaultPrinter(ptr);
    for (auto&& itr : ptr->responseData().queryResultSet) {
        auto&& scope = itr.first;
        auto&& resultSet = itr.second;
        if (resultSet.hasResult) {
            string const caption = "RESULT SET [" + scope + "]";
            string const indent = "";
            auto const table = resultSet.toColumnTable(caption, indent);
            bool const topSeparator = false;
            bool const bottomSeparator = false;
            bool const repeatedHeader = false;
            size_t const pageSize = 0;
            table.print(cout, topSeparator, bottomSeparator, pageSize, repeatedHeader);
        }
    }
}

SqlRequest::SqlRequest(shared_ptr<Controller> const& controller, std::string const& requestName,
                       string const& workerName, uint64_t maxRows, int priority, bool keepTracking)
        : Request(controller, requestName, workerName, priority, keepTracking, ::disposeRequired) {
    // Partial initialization of the request body's content. Other members
    // will be set in the request type-specific subclasses.
    requestBody.set_max_rows(maxRows);
}

list<pair<string, string>> SqlRequest::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("type", ProtocolRequestSql_Type_Name(requestBody.type()));
    result.emplace_back("max_rows", to_string(requestBody.max_rows()));
    result.emplace_back("query", requestBody.query());
    result.emplace_back("user", requestBody.user());
    result.emplace_back("database", requestBody.database());
    result.emplace_back("table", requestBody.table());
    result.emplace_back("engine", requestBody.engine());
    result.emplace_back("partition_by_column", requestBody.partition_by_column());
    result.emplace_back("transaction_id", to_string(requestBody.transaction_id()));
    result.emplace_back("num_columns", to_string(requestBody.columns_size()));
    result.emplace_back("tables", util::String::toString(requestBody.tables()));
    result.emplace_back("batch_mode", bool2str(requestBody.batch_mode()));
    return result;
}

SqlResultSet const& SqlRequest::responseData() const { return _responseData; }

void SqlRequest::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Serialize the Request message header and the request body into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::QUEUED);
    hdr.set_queued_type(ProtocolQueuedRequestType::SQL);
    hdr.set_timeout(requestExpirationIvalSec());
    hdr.set_priority(priority());
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);
    buffer()->serialize(requestBody);

    _send(lock);
}

void SqlRequest::awaken(boost::system::error_code const& ec) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    if (isAborted(ec)) return;
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    // Serialize the Status message header and the status request's body into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_TRACK);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestTrack statusRequestBody;
    statusRequestBody.set_id(id());
    statusRequestBody.set_queued_type(ProtocolQueuedRequestType::SQL);
    buffer()->serialize(statusRequestBody);

    _send(lock);
}

void SqlRequest::_send(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    auto self = shared_from_base<SqlRequest>();
    controller()->serviceProvider()->messenger()->send<ProtocolResponseSql>(
            workerName(), id(), priority(), buffer(),
            [self](string const& id, bool success, ProtocolResponseSql const& response) {
                self->_analyze(success, response);
            });
}

void SqlRequest::_analyze(bool success, ProtocolResponseSql const& response) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  success=" << (success ? "true" : "false"));

    // This method is called on behalf of an asynchronous callback fired
    // upon a completion of the request within method _send() - the only
    // client of _analyze(). So, we should take care of proper locking and watch
    // for possible state transition which might occur while the async I/O was
    // still in a progress.
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;
    if (not success) {
        finish(lock, CLIENT_ERROR);
        return;
    }

    // Always use  the latest status reported by the remote server
    setExtendedServerStatus(lock, response.status_ext());

    // Performance counters are updated from either of two sources,
    // depending on the availability of the 'target' performance counters
    // filled in by the 'STATUS' queries. If the later is not available
    // then fallback to the one of the current request.
    if (response.has_target_performance()) {
        mutablePerformance().update(response.target_performance());
    } else {
        mutablePerformance().update(response.performance());
    }

    // Always extract extended data regardless of the completion status
    // reported by the worker service.
    _responseData.set(response);
    _responseData.performanceSec = (util::TimeUtils::now() - performance(lock).c_create_time) / 1000.;

    // Extract target request type-specific parameters from the response
    if (response.has_request()) {
        _targetRequestParams = SqlRequestParams(response.request());
    }
    switch (response.status()) {
        case ProtocolStatus::SUCCESS:
            finish(lock, SUCCESS);
            break;
        case ProtocolStatus::CREATED:
            keepTrackingOrFinish(lock, SERVER_CREATED);
            break;
        case ProtocolStatus::QUEUED:
            keepTrackingOrFinish(lock, SERVER_QUEUED);
            break;
        case ProtocolStatus::IN_PROGRESS:
            keepTrackingOrFinish(lock, SERVER_IN_PROGRESS);
            break;
        case ProtocolStatus::IS_CANCELLING:
            keepTrackingOrFinish(lock, SERVER_IS_CANCELLING);
            break;
        case ProtocolStatus::BAD:
            finish(lock, SERVER_BAD);
            break;
        case ProtocolStatus::FAILED:
            finish(lock, SERVER_ERROR);
            break;
        case ProtocolStatus::CANCELLED:
            finish(lock, SERVER_CANCELLED);
            break;
        default:
            throw logic_error("SqlRequest::" + string(__func__) + "  unknown status '" +
                              ProtocolStatus_Name(response.status()) + "' received from server");
    }
}

void SqlRequest::savePersistentState(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    controller()->serviceProvider()->databaseServices()->saveState(*this, performance(lock));
}

}  // namespace lsst::qserv::replica
