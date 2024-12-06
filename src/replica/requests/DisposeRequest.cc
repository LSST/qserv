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
#include "replica/requests/DisposeRequest.h"

// System headers
#include <functional>
#include <sstream>

// Qserv headers
#include "replica/contr/Controller.h"
#include "replica/requests/Messenger.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ProtocolBuffer.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DisposeRequest");
bool const disposeRequiredNo = false;
}  // namespace

namespace lsst::qserv::replica {

DisposeRequestResult::DisposeRequestResult(ProtocolResponseDispose const& message) {
    for (int idx = 0; idx < message.ids_size(); ++idx) {
        auto&& stat = message.ids(idx);
        ids.emplace_back(Status{stat.id(), stat.disposed()});
    }
}

string DisposeRequest::toString(bool extended) const {
    ostringstream oss;
    oss << Request::toString(extended);
    if (extended) {
        oss << "  Disposed requests:\n";
        for (auto&& entry : responseData().ids) {
            if (entry.disposed) {
                oss << "    " << entry.id << "\n";
            }
        }
    }
    return oss.str();
}

DisposeRequest::Ptr DisposeRequest::createAndStart(shared_ptr<Controller> const& controller,
                                                   string const& workerName,
                                                   std::vector<std::string> const& targetIds,
                                                   CallbackType const& onFinish, int priority,
                                                   bool keepTracking, string const& jobId,
                                                   unsigned int requestExpirationIvalSec) {
    auto ptr = DisposeRequest::Ptr(
            new DisposeRequest(controller, workerName, targetIds, onFinish, priority, keepTracking));
    ptr->start(jobId, requestExpirationIvalSec);
    return ptr;
}

DisposeRequest::DisposeRequest(shared_ptr<Controller> const& controller, string const& workerName,
                               std::vector<std::string> const& targetIds, CallbackType const& onFinish,
                               int priority, bool keepTracking)
        : RequestMessenger(controller, "DISPOSE", workerName, priority, keepTracking, ::disposeRequiredNo),
          _targetIds(targetIds),
          _onFinish(onFinish) {}

DisposeRequestResult const& DisposeRequest::responseData() const { return _responseData; }

void DisposeRequest::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "  worker: " << workerName() << " targetIds.size: " << targetIds().size());

    // Serialize the Request message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_DISPOSE);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestDispose message;
    for (auto&& id : targetIds()) {
        message.add_ids(id);
    }
    buffer()->serialize(message);

    _send(lock);
}

void DisposeRequest::_send(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    controller()->serviceProvider()->messenger()->send<ProtocolResponseDispose>(
            workerName(), id(), priority(), buffer(),
            // Don't forward the first parameter (request's identifier) of the callback
            // to the response's analyzer. A value of the identifier is already known
            // in a context of the method.
            bind(&DisposeRequest::_analyze, shared_from_base<DisposeRequest>(), _2, _3));
}

void DisposeRequest::_analyze(bool success, ProtocolResponseDispose const& message) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  success=" << (success ? "true" : "false"));

    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    // This type of request (if delivered to a worker and if a response from
    // the worker is received) is always considered as "successful".
    if (success) {
        _responseData = DisposeRequestResult(message);
    }
    finish(lock, success ? SUCCESS : CLIENT_ERROR);
}

void DisposeRequest::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<DisposeRequest>(lock, _onFinish);
}

}  // namespace lsst::qserv::replica
