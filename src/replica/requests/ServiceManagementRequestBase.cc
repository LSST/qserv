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
#include "replica/requests/ServiceManagementRequestBase.h"

// System headers
#include <stdexcept>

// Third party headers
#include "boost/date_time/posix_time/posix_time.hpp"

// Qserv headers
#include "replica/contr/Controller.h"
#include "replica/requests/Messenger.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ProtocolBuffer.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace lsst::qserv::replica;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.ServiceManagementRequest");

/// Dump a collection of request descriptions onto the output stream
void dumpRequestInfo(ostream& os, vector<ProtocolServiceResponseInfo> const& requests) {
    for (auto&& r : requests) {
        os << "\n"
           << "    type:     " << ProtocolQueuedRequestType_Name(r.queued_type()) << "\n"
           << "    id:       " << r.id() << "\n"
           << "    priority: " << r.priority() << "\n";
    }
}

bool const keepTrackingNo = false;
bool const disposeRequiredNo = false;

}  // namespace

namespace lsst::qserv::replica {

///////////////////////////////////
//         ServiceState          //
///////////////////////////////////

string ServiceState::state2string() const {
    switch (state) {
        case SUSPEND_IN_PROGRESS:
            return "SUSPEND_IN_PROGRESS";
        case SUSPENDED:
            return "SUSPENDED";
        case RUNNING:
            return "RUNNING";
        default:
            throw runtime_error("ServiceState::" + string(__func__) +
                                "  unhandled state: " + to_string(state));
    }
    return string();
}

void ServiceState::set(ProtocolServiceResponse const& message) {
    switch (message.service_state()) {
        case ProtocolServiceResponse::SUSPEND_IN_PROGRESS:
            state = ServiceState::State::SUSPEND_IN_PROGRESS;
            break;

        case ProtocolServiceResponse::SUSPENDED:
            state = ServiceState::State::SUSPENDED;
            break;

        case ProtocolServiceResponse::RUNNING:
            state = ServiceState::State::RUNNING;
            break;

        default:
            throw runtime_error("ServiceState::" + string(__func__) +
                                "  service state found in protocol is unknown");
    }
    startTime = message.start_time();

    numNewRequests = message.num_new_requests();
    numInProgressRequests = message.num_in_progress_requests();
    numFinishedRequests = message.num_finished_requests();

    for (int num = message.new_requests_size(), idx = 0; idx < num; ++idx) {
        newRequests.emplace_back(message.new_requests(idx));
    }
    for (int num = message.in_progress_requests_size(), idx = 0; idx < num; ++idx) {
        inProgressRequests.emplace_back(message.in_progress_requests(idx));
    }
    for (int num = message.finished_requests_size(), idx = 0; idx < num; ++idx) {
        finishedRequests.emplace_back(message.finished_requests(idx));
    }
}

ostream& operator<<(ostream& os, ServiceState const& ss) {
    unsigned int const secondsAgo = (util::TimeUtils::now() - ss.startTime) / 1000.0f;

    os << "ServiceState:\n"
       << "\n  Summary:\n\n"
       << "    service state:              " << ss.state2string() << "\n"
       << "    start time [ms]:            " << ss.startTime << " (" << secondsAgo << " seconds ago)\n"
       << "    total new requests:         " << ss.numNewRequests << "\n"
       << "    total in-progress requests: " << ss.numInProgressRequests << "\n"
       << "    total finished requests:    " << ss.numFinishedRequests << "\n";

    os << "\n  New:\n";
    ::dumpRequestInfo(os, ss.newRequests);

    os << "\n  In-Progress:\n";
    ::dumpRequestInfo(os, ss.inProgressRequests);

    os << "\n  Finished:\n";
    ::dumpRequestInfo(os, ss.finishedRequests);

    return os;
}

void ServiceManagementRequestBase::extendedPrinter(Ptr const& ptr) {
    Request::defaultPrinter(ptr);
    cout << ptr->getServiceState();
}

ServiceState const& ServiceManagementRequestBase::getServiceState() const {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    switch (Request::state()) {
        case Request::State::FINISHED:
            switch (Request::extendedState()) {
                case Request::ExtendedState::SUCCESS:
                case Request::ExtendedState::SERVER_ERROR:
                    return _serviceState;
                default:
                    break;
            }
        default:
            break;
    }
    throw logic_error("ServiceManagementRequestBase::" + string(__func__) +
                      "  not allowed in the current state of the request");
}

ServiceManagementRequestBase::ServiceManagementRequestBase(shared_ptr<Controller> const& controller,
                                                           char const* requestName, string const& workerName,
                                                           ProtocolServiceRequestType requestType,
                                                           int priority)
        : RequestMessenger(controller, requestName, workerName, priority, ::keepTrackingNo,
                           ::disposeRequiredNo),
          _requestType(requestType) {}

void ServiceManagementRequestBase::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Serialize the Request message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::SERVICE);
    hdr.set_service_type(_requestType);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    // Send the message
    controller()->serviceProvider()->messenger()->send<ProtocolServiceResponse>(
            workerName(), id(), priority(), buffer(),
            [self = shared_from_base<ServiceManagementRequestBase>()](
                    string const& id, bool success, ProtocolServiceResponse const& response) {
                self->_analyze(success, response);
            });
}

void ServiceManagementRequestBase::_analyze(bool success, ProtocolServiceResponse const& message) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  success=" << (success ? "true" : "false"));

    // This method is called on behalf of an asynchronous callback fired
    // upon a completion of the request within method send() - the only
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

    // Update performance counters.
    mutablePerformance().update(message.performance());

    // Capture the general status of the operation.
    switch (message.status()) {
        case ProtocolStatus::SUCCESS:
            // Transfer the state of the remote service into a local data member
            // before initiating state transition of the request object.
            _serviceState.set(message);
            finish(lock, SUCCESS);
            break;
        default:
            finish(lock, SERVER_ERROR);
            break;
    }
}

void ServiceManagementRequestBase::savePersistentState(replica::Lock const& lock) {
    controller()->serviceProvider()->databaseServices()->saveState(*this, performance(lock));
}

}  // namespace lsst::qserv::replica
