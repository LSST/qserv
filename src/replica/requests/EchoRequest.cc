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
#include "replica/requests/EchoRequest.h"

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
#include "replica/util/ProtocolBuffer.h"
#include "replica/util/ReplicaInfo.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.EchoRequest");
bool const disposeRequired = true;
}  // namespace

namespace lsst::qserv::replica {

EchoRequest::Ptr EchoRequest::createAndStart(shared_ptr<Controller> const& controller,
                                             string const& workerName, string const& data, uint64_t delay,
                                             CallbackType const& onFinish, int priority, bool keepTracking,
                                             std::string const& jobId,
                                             unsigned int requestExpirationIvalSec) {
    auto ptr = EchoRequest::Ptr(
            new EchoRequest(controller, workerName, data, delay, onFinish, priority, keepTracking));
    ptr->start(jobId, requestExpirationIvalSec);
    return ptr;
}

EchoRequest::EchoRequest(shared_ptr<Controller> const& controller, string const& workerName,
                         string const& data, uint64_t delay, CallbackType const& onFinish, int priority,
                         bool keepTracking)
        : RequestMessenger(controller, "TEST_ECHO", workerName, priority, keepTracking, ::disposeRequired),
          _data(data),
          _delay(delay),
          _onFinish(onFinish) {}

string const& EchoRequest::responseData() const { return _responseData; }

void EchoRequest::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "  worker: " << workerName() << " data.length: " << data().size()
                   << " delay: " << delay());

    // Serialize the Request message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::QUEUED);
    hdr.set_queued_type(ProtocolQueuedRequestType::TEST_ECHO);
    hdr.set_timeout(requestExpirationIvalSec());
    hdr.set_priority(priority());
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestEcho message;
    message.set_data(data());
    message.set_delay(delay());
    buffer()->serialize(message);

    _send(lock);
}

void EchoRequest::awaken(boost::system::error_code const& ec) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    if (isAborted(ec)) return;
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    // Serialize the Status message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_TRACK);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestTrack message;
    message.set_id(id());
    message.set_queued_type(ProtocolQueuedRequestType::TEST_ECHO);
    buffer()->serialize(message);

    _send(lock);
}

void EchoRequest::_send(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    controller()->serviceProvider()->messenger()->send<ProtocolResponseEcho>(
            workerName(), id(), priority(), buffer(),
            [self = shared_from_base<EchoRequest>()](string const& id, bool success,
                                                     ProtocolResponseEcho const& response) {
                self->_analyze(success, response);
            });
}

void EchoRequest::_analyze(bool success, ProtocolResponseEcho const& message) {
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
    setExtendedServerStatus(lock, message.status_ext());

    // Performance counters are updated from either of two sources,
    // depending on the availability of the 'target' performance counters
    // filled in by the 'STATUS' queries. If the later is not available
    // then fallback to the one of the current request.
    if (message.has_target_performance()) {
        mutablePerformance().update(message.target_performance());
    } else {
        mutablePerformance().update(message.performance());
    }

    // Always extract extended data regardless of the completion status
    // reported by the worker service.
    _responseData = message.data();

    // Extract target request type-specific parameters from the response
    if (message.has_request()) {
        _targetRequestParams = EchoRequestParams(message.request());
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
            throw logic_error("EchoRequest::" + string(__func__) + "  unknown status '" +
                              ProtocolStatus_Name(message.status()) + "' received from server");
    }
}

void EchoRequest::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<EchoRequest>(lock, _onFinish);
}

void EchoRequest::savePersistentState(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    controller()->serviceProvider()->databaseServices()->saveState(*this, performance(lock));
}

list<pair<string, string>> EchoRequest::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("data_length_bytes", to_string(data().size()));
    result.emplace_back("delay_milliseconds", to_string(delay()));
    return result;
}

}  // namespace lsst::qserv::replica
