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
#include "replica/requests/StatusRequest.h"

// System headers
#include <sstream>

// Qserv headers
#include "replica/contr/Controller.h"
#include "replica/requests/Messenger.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ProtocolBuffer.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.StatusRequest");
bool const disposeRequiredNo = false;
}  // namespace

namespace lsst::qserv::replica {

StatusRequest::Ptr StatusRequest::createAndStart(shared_ptr<Controller> const& controller,
                                                 string const& workerName, string const& targetRequestId,
                                                 CallbackType const& onFinish, int priority,
                                                 bool keepTracking, string const& jobId,
                                                 unsigned int requestExpirationIvalSec) {
    auto ptr = StatusRequest::Ptr(
            new StatusRequest(controller, workerName, targetRequestId, onFinish, priority, keepTracking));
    ptr->start(jobId, requestExpirationIvalSec);
    return ptr;
}

StatusRequest::StatusRequest(shared_ptr<Controller> const& controller, string const& workerName,
                             string const& targetRequestId, CallbackType const& onFinish, int priority,
                             bool keepTracking)
        : RequestMessenger(controller, "REQUEST_STATUS", workerName, priority, keepTracking,
                           ::disposeRequiredNo),
          _targetRequestId(targetRequestId),
          _onFinish(onFinish) {}

string StatusRequest::toString(bool extended) const {
    ostringstream oss;
    oss << Request::toString(extended) << "  targetRequestId: " + targetRequestId() << "\n";
    if ((state() == State::FINISHED) && (extendedState() == ExtendedState::SUCCESS)) {
        oss << "  targetStatus: " << ProtocolStatus_Name(targetRequestStatus()) << "\n";
        oss << "  targetStatusExt: " << ProtocolStatusExt_Name(targetRequestExtendedStatus()) << "\n";
        oss << "  targetPerformance: " << targetRequestPerformance() << "\n";
    } else {
        oss << "  targetStatus: N/A\n";
        oss << "  targetStatusExt: N/A\n";
        oss << "  targetPerformance: N/A\n";
    }
    return oss.str();
}

ProtocolStatus StatusRequest::targetRequestStatus() const {
    _assertIsSucceeded();
    return _targetStatus;
}

ProtocolStatusExt StatusRequest::targetRequestExtendedStatus() const {
    _assertIsSucceeded();
    return _targetExtendedServerStatus;
}

Performance const& StatusRequest::targetRequestPerformance() const {
    _assertIsSucceeded();
    return _targetPerformance;
}

void StatusRequest::notify(replica::Lock const& lock) { notifyDefaultImpl<StatusRequest>(lock, _onFinish); }

void StatusRequest::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Serialize the Status message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_STOP);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestStatus message;
    message.set_id(_targetRequestId);
    buffer()->serialize(message);

    _send(lock);
}

void StatusRequest::awaken(boost::system::error_code const& ec) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    if (isAborted(ec)) return;
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    // Send the same message again.
    _send(lock);
}

void StatusRequest::_send(replica::Lock const& lock) {
    controller()->serviceProvider()->messenger()->send<ProtocolResponseStatus>(
            workerName(), id(), priority(), buffer(),
            [self = shared_from_base<StatusRequest>()](string const& id, bool success,
                                                       ProtocolResponseStatus const& response) {
                self->_analyze(success, response);
            });
}

void StatusRequest::_analyze(bool success, ProtocolResponseStatus message) {
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

    // Always get the latest status reported by the remote server
    setExtendedServerStatus(lock, message.status_ext());

    // Always update performance counters obtained from the worker service.
    mutablePerformance().update(message.performance());

    // Extract status of the target request if available
    if (message.has_target_status()) {
        _targetStatus = message.target_status();
    }
    if (message.has_target_status_ext()) {
        _targetExtendedServerStatus = message.target_status_ext();
    }
    if (message.has_target_performance()) {
        _targetPerformance.update(message.target_performance());
    }
    switch (message.status()) {
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
            throw logic_error("StatusRequest::" + string(__func__) + "  unknown status '" +
                              ProtocolStatus_Name(message.status()) + "' received from server");
    }
}

void StatusRequest::_assertIsSucceeded() const {
    if (state() != State::FINISHED) {
        throw logic_error("StatusRequest::" + string(__func__) + "  the status request hasn't finished yet");
    }
    if (extendedState() != ExtendedState::SUCCESS) {
        throw logic_error("StatusRequest::" + string(__func__) + "  the status request has failed");
    }
}

}  // namespace lsst::qserv::replica
