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
#include "replica/requests/StopRequest.h"

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
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.StopRequest");
bool const disposeRequiredNo = false;
}  // namespace

namespace lsst::qserv::replica {

StopRequest::Ptr StopRequest::createAndStart(shared_ptr<Controller> const& controller,
                                             string const& workerName, string const& targetRequestId,
                                             CallbackType const& onFinish, int priority, bool keepTracking,
                                             string const& jobId, unsigned int requestExpirationIvalSec) {
    auto ptr = StopRequest::Ptr(
            new StopRequest(controller, workerName, targetRequestId, onFinish, priority, keepTracking));
    ptr->start(jobId, requestExpirationIvalSec);
    return ptr;
}

StopRequest::StopRequest(shared_ptr<Controller> const& controller, string const& workerName,
                         string const& targetRequestId, CallbackType const& onFinish, int priority,
                         bool keepTracking)
        : Request(controller, "REQUEST_STOP", workerName, priority, keepTracking, ::disposeRequiredNo),
          _targetRequestId(targetRequestId),
          _onFinish(onFinish) {}

string StopRequest::toString(bool extended) const {
    return Request::toString(extended) + "  targetRequestId: " + targetRequestId() + "\n";
}

list<pair<string, string>> StopRequest::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("target_request_id", targetRequestId());
    return result;
}

void StopRequest::notify(replica::Lock const& lock) { notifyDefaultImpl<StopRequest>(lock, _onFinish); }

void StopRequest::savePersistentState(replica::Lock const& lock) {
    controller()->serviceProvider()->databaseServices()->saveState(*this, performance(lock));
}

void StopRequest::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Serialize the Stop message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_STOP);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestStop message;
    message.set_id(_targetRequestId);
    buffer()->serialize(message);

    _send(lock);
}

void StopRequest::awaken(boost::system::error_code const& ec) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    if (isAborted(ec)) return;
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    // Send the same message again.
    _send(lock);
}

void StopRequest::_send(replica::Lock const& lock) {
    controller()->serviceProvider()->messenger()->send<ProtocolResponseStop>(
            workerName(), id(), priority(), buffer(),
            [self = shared_from_base<StopRequest>()](string const& id, bool success,
                                                     ProtocolResponseStop const& response) {
                self->_analyze(success, response);
            });
}

void StopRequest::_analyze(bool success, ProtocolResponseStop message) {
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
            throw logic_error("StopRequest::" + string(__func__) + "  unknown status '" +
                              ProtocolStatus_Name(message.status()) + "' received from server");
    }
}

}  // namespace lsst::qserv::replica
