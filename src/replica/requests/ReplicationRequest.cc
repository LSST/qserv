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
#include "replica/requests/ReplicationRequest.h"

// System headers
#include <functional>
#include <stdexcept>

// Third party headers
#include "boost/date_time/posix_time/posix_time.hpp"

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/contr/Controller.h"
#include "replica/requests/Messenger.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ProtocolBuffer.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.ReplicationRequest");
bool const disposeRequired = true;
}  // namespace

namespace lsst::qserv::replica {

ReplicationRequest::Ptr ReplicationRequest::createAndStart(
        shared_ptr<Controller> const& controller, string const& workerName, string const& sourceWorkerName,
        string const& database, unsigned int chunk, CallbackType const& onFinish, int priority,
        bool keepTracking, string const& jobId, unsigned int requestExpirationIvalSec) {
    auto ptr = ReplicationRequest::Ptr(new ReplicationRequest(
            controller, workerName, sourceWorkerName, database, chunk, onFinish, priority, keepTracking));
    ptr->start(jobId, requestExpirationIvalSec);
    return ptr;
}

ReplicationRequest::ReplicationRequest(shared_ptr<Controller> const& controller, string const& workerName,
                                       string const& sourceWorkerName, string const& database,
                                       unsigned int chunk, CallbackType const& onFinish, int priority,
                                       bool keepTracking)
        : RequestMessenger(controller, "REPLICA_CREATE", workerName, priority, keepTracking,
                           ::disposeRequired),
          _database(database),
          _chunk(chunk),
          _sourceWorkerName(sourceWorkerName),
          _onFinish(onFinish),
          _replicaInfo() {
    controller->serviceProvider()->config()->assertWorkerIsValid(sourceWorkerName);
    controller->serviceProvider()->config()->assertWorkersAreDifferent(sourceWorkerName, workerName);
    controller->serviceProvider()->config()->assertDatabaseIsValid(database);
}

void ReplicationRequest::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    ConfigWorker const sourceConfigWorker =
            controller()->serviceProvider()->config()->worker(sourceWorkerName());

    // Serialize the Request message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::QUEUED);
    hdr.set_queued_type(ProtocolQueuedRequestType::REPLICA_CREATE);
    hdr.set_timeout(requestExpirationIvalSec());
    hdr.set_priority(priority());
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestReplicate message;
    message.set_database(database());
    message.set_chunk(chunk());
    message.set_worker(sourceWorkerName());
    message.set_worker_host(sourceConfigWorker.fsHost.addr);
    message.set_worker_port(sourceConfigWorker.fsPort);
    message.set_worker_data_dir(sourceConfigWorker.dataDir);
    buffer()->serialize(message);

    _send(lock);
}

void ReplicationRequest::awaken(boost::system::error_code const& ec) {
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
    message.set_queued_type(ProtocolQueuedRequestType::REPLICA_CREATE);
    buffer()->serialize(message);

    _send(lock);
}

void ReplicationRequest::_send(replica::Lock const& lock) {
    auto self = shared_from_base<ReplicationRequest>();
    controller()->serviceProvider()->messenger()->send<ProtocolResponseReplicate>(
            workerName(), id(), priority(), buffer(),
            [self](string const& id, bool success, ProtocolResponseReplicate const& response) {
                self->_analyze(success, response);
            });
}

void ReplicationRequest::_analyze(bool success, ProtocolResponseReplicate const& message) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  success=" << (success ? "true" : "false"));

    // This method is called on behalf of an asynchronous callback fired
    // upon a completion of the request within method _send() - the only
    // client of analyze(). So, we should take care of proper locking and watch
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

    // Performance counters are updated from either of two sources,
    // depending on the availability of the 'target' performance counters
    // filled in by the 'STATUS' queries. If the later is not available
    // then fallback to the one of the current request.
    if (message.has_target_performance())
        mutablePerformance().update(message.target_performance());
    else
        mutablePerformance().update(message.performance());

    // Always extract extended data regardless of the completion status
    // reported by the worker service.
    _replicaInfo = ReplicaInfo(&(message.replica_info()));

    // Extract target request type-specific parameters from the response
    if (message.has_request()) {
        _targetRequestParams = ReplicationRequestParams(message.request());
    }
    switch (message.status()) {
        case ProtocolStatus::SUCCESS:
            controller()->serviceProvider()->databaseServices()->saveReplicaInfo(_replicaInfo);
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
            throw logic_error("ReplicationRequest::" + string(__func__) + "  unknown status '" +
                              ProtocolStatus_Name(message.status()) + "' received from server");
    }
}

void ReplicationRequest::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<ReplicationRequest>(lock, _onFinish);
}

void ReplicationRequest::savePersistentState(replica::Lock const& lock) {
    controller()->serviceProvider()->databaseServices()->saveState(*this, performance(lock));
}

list<pair<string, string>> ReplicationRequest::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("database", database());
    result.emplace_back("chunk", to_string(chunk()));
    result.emplace_back("source_worker", sourceWorkerName());
    return result;
}

}  // namespace lsst::qserv::replica
