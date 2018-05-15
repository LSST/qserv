/*
 * LSST Data Management System
 * Copyright 2017 LSST Corporation.
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
#include "replica/StatusRequestBase.h"

// System headers
#include <stdexcept>

// Third party headers
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

// Qserv headers
#include "lsst/log/Log.h"
#include "replica/LockUtils.h"
#include "replica/ProtocolBuffer.h"
#include "replica/ServiceProvider.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.StatusRequest");

} /// namespace

namespace lsst {
namespace qserv {
namespace replica {

StatusRequestBase::StatusRequestBase(ServiceProvider::Ptr const& serviceProvider,
                                     boost::asio::io_service& io_service,
                                     char const*              requestTypeName,
                                     std::string const&       worker,
                                     std::string const&       targetRequestId,
                                     proto::ReplicationReplicaRequestType requestType,
                                     bool                     keepTracking,
                                     std::shared_ptr<Messenger> const& messenger)
    :   RequestMessenger(serviceProvider,
                         io_service,
                         requestTypeName,
                         worker,
                         0,    /* priority */
                         keepTracking,
                         false /* allowDuplicate */,
                         messenger),
        _targetRequestId(targetRequestId),
        _requestType(requestType) {
}

void StatusRequestBase::startImpl() {

    LOGS(_log, LOG_LVL_DEBUG, context() << "startImpl");

    ASSERT_LOCK(_mtx, context() + "startImpl");

    // Serialize the Request message header and the request itself into
    // the network buffer.

    _bufferPtr->resize();

    proto::ReplicationRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(proto::ReplicationRequestHeader::REQUEST);
    hdr.set_management_type(proto::ReplicationManagementRequestType::REQUEST_STATUS);

    _bufferPtr->serialize(hdr);

    proto::ReplicationRequestStatus message;
    message.set_id(_targetRequestId);
    message.set_type(_requestType);

    _bufferPtr->serialize(message);

    send();
}

void StatusRequestBase::wait() {

    LOGS(_log, LOG_LVL_DEBUG, context() << "wait");

    ASSERT_LOCK(_mtx, context() + "wait");

    // Allways need to set the interval before launching the timer.

    _timer.expires_from_now(boost::posix_time::seconds(_timerIvalSec));
    _timer.async_wait(
        boost::bind(
            &StatusRequestBase::awaken,
            shared_from_base<StatusRequestBase>(),
            boost::asio::placeholders::error
        )
    );
}

void StatusRequestBase::awaken(boost::system::error_code const& ec) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "awaken");

    if (isAborted(ec)) return;

    // IMPORTANT: the final state is required to be tested twice. The first time
    // it's done in order to avoid deadlock on the "in-flight" callbacks reporting
    // their completion while the request termination is in a progress. And the second
    // test is made after acquering the lock to recheck the state in case if it
    // has transitioned while acquering the lock.

    if (_state== State::FINISHED) return;

    LOCK(_mtx, context() + "awaken");

    if (_state== State::FINISHED) return;

    // Serialize the Status message header and the request itself into
    // the network buffer.

    _bufferPtr->resize();

    proto::ReplicationRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(proto::ReplicationRequestHeader::REQUEST);
    hdr.set_management_type(proto::ReplicationManagementRequestType::REQUEST_STATUS);

    _bufferPtr->serialize(hdr);

    proto::ReplicationRequestStatus message;
    message.set_id(_targetRequestId);
    message.set_type(_requestType);

    _bufferPtr->serialize(message);

    send();
}

void StatusRequestBase::analyze(bool success,
                                proto::ReplicationStatus status) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "analyze  success=" << (success ? "true" : "false"));

    // This method is called on behalf of an asynchronious callback fired
    // upon a completion of the request within method send() - the only
    // client of analyze(). So, we should take care of proper locking and watch
    // for possible state transition which might occure while the async I/O was
    // still in a progress.

    // IMPORTANT: the final state is required to be tested twice. The first time
    // it's done in order to avoid deadlock on the "in-flight" callbacks reporting
    // their completion while the request termination is in a progress. And the second
    // test is made after acquering the lock to recheck the state in case if it
    // has transitioned while acquering the lock.

    if (_state == State::FINISHED) return;

    LOCK(_mtx, context() + "analyze");

    if (_state == State::FINISHED) return;

    if (success) {

        switch (status) {

            case proto::ReplicationStatus::SUCCESS:

                // Save the replica state
                saveReplicaInfo();

                finish(SUCCESS);
                break;

            case proto::ReplicationStatus::QUEUED:
                if (_keepTracking) wait();
                else               finish(SERVER_QUEUED);
                break;

            case proto::ReplicationStatus::IN_PROGRESS:
                if (_keepTracking) wait();
                else               finish(SERVER_IN_PROGRESS);
                break;

            case proto::ReplicationStatus::IS_CANCELLING:
                if (_keepTracking) wait();
                else               finish(SERVER_IS_CANCELLING);
                break;

            case proto::ReplicationStatus::BAD:
                finish(SERVER_BAD);
                break;

            case proto::ReplicationStatus::FAILED:
                finish(SERVER_ERROR);
                break;

            case proto::ReplicationStatus::CANCELLED:
                finish(SERVER_CANCELLED);
                break;

            default:
                throw std::logic_error(
                        "StatusRequestBase::analyze() unknown status '"
                        + proto::ReplicationStatus_Name(status) +
                        "' received from server");
        }

    } else {
        finish(CLIENT_ERROR);
    }

    if (_state == State::FINISHED) notify();
}

}}} // namespace lsst::qserv::replica
