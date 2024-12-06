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
#include "replica/requests/RequestMessenger.h"

// Qserv headers
#include "replica/contr/Controller.h"
#include "replica/proto/protocol.pb.h"
#include "replica/requests/Messenger.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ProtocolBuffer.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.RequestMessenger");

}  // namespace

namespace lsst::qserv::replica {

RequestMessenger::RequestMessenger(shared_ptr<Controller> const& controller, string const& type,
                                   string const& workerName, int priority, bool keepTracking,
                                   bool disposeRequired)
        : Request(controller, type, workerName, priority, keepTracking, disposeRequired) {}

void RequestMessenger::finishImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Make sure the request (if any) has been eliminated from the messenger.
    // This operation is unnecessary if the request has successfully finished,
    // in which case it's guaranteed that no outstanding message for the request
    // will be at the messenger's queue. This optimization also reduces extra
    // locking (and delays) in the messenger because the operation is synchronized.
    if (extendedState() != Request::ExtendedState::SUCCESS) {
        controller()->serviceProvider()->messenger()->cancel(workerName(), id());
    }

    // Tell the worker to dispose the request if a subclass made such requirement,
    // and only if the request has successfully finished. This will remove the request
    // from the worker's "finished" queue and release memory taken by the request
    // much earlier than after request expiration deadline.
    // Don't dispose requests in other states since any such actions may result in
    // unnecessary increase of the traffic on a communication channel with the worker
    // and increase processing latency (and increasing a probability of running into
    // the Controller side timeouts while waiting for the completion of the requests)
    // of the on-going or queued requests.
    // Requests in other states ended up at workers would be automatically disposed
    // by workers after requests's expiration deadlines.
    if (disposeRequired() && (extendedState() == Request::ExtendedState::SUCCESS)) {
        // Don't require any callback notification for the completion of
        // the operation. This will also prevent incrementing a shared pointer
        // counter for the current object.
        dispose(lock, priority(), nullptr);
    }
}

void RequestMessenger::dispose(replica::Lock const& lock, int priority,
                               OnDisposeCallbackType const& onFinish) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_DISPOSE);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());

    buffer()->serialize(hdr);
    ProtocolRequestDispose message;
    message.add_ids(id());
    buffer()->serialize(message);

    controller()->serviceProvider()->messenger()->send<ProtocolResponseDispose>(workerName(), id(), priority,
                                                                                buffer(), onFinish);
}

}  // namespace lsst::qserv::replica
