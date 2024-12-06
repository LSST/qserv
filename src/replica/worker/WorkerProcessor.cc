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
#include "replica/worker/WorkerProcessor.h"

// System headers
#include <algorithm>
#include <iterator>
#include <stdexcept>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/mysql/DatabaseMySQL.h"
#include "replica/services/ServiceProvider.h"
#include "replica/worker/WorkerDeleteRequest.h"
#include "replica/worker/WorkerEchoRequest.h"
#include "replica/worker/WorkerFindRequest.h"
#include "replica/worker/WorkerFindAllRequest.h"
#include "replica/worker/WorkerReplicationRequest.h"
#include "replica/worker/WorkerSqlRequest.h"
#include "replica/worker/WorkerDirectorIndexRequest.h"
#include "util/BlockPost.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;
using namespace lsst::qserv::replica;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerProcessor");
}  // namespace

namespace lsst::qserv::replica {

string WorkerProcessor::state2string(State state) {
    switch (state) {
        case STATE_IS_RUNNING:
            return "STATE_IS_RUNNING";
        case STATE_IS_STOPPING:
            return "STATE_IS_STOPPING";
        case STATE_IS_STOPPED:
            return "STATE_IS_STOPPED";
    }
    throw logic_error(_classMethodContext(__func__) + "  unhandled state " + to_string(state));
}

WorkerProcessor::Ptr WorkerProcessor::create(ServiceProvider::Ptr const& serviceProvider,
                                             string const& worker) {
    return Ptr(new WorkerProcessor(serviceProvider, worker));
}

WorkerProcessor::WorkerProcessor(ServiceProvider::Ptr const& serviceProvider, string const& worker)
        : _serviceProvider(serviceProvider),
          _worker(worker),
          _connectionPool(database::mysql::ConnectionPool::create(
                  Configuration::qservWorkerDbParams(),
                  serviceProvider->config()->get<size_t>("database", "services-pool-size"))),
          _state(STATE_IS_STOPPED),
          _startTime(util::TimeUtils::now()) {}

void WorkerProcessor::run() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    if (_state == STATE_IS_STOPPED) {
        size_t const numThreads =
                _serviceProvider->config()->get<size_t>("worker", "num-svc-processing-threads");
        if (not numThreads) {
            throw out_of_range(_classMethodContext(__func__) +
                               "invalid configuration parameter for the number of processing threads. "
                               "The value of the parameter must be greater than 0");
        }

        // Create threads if needed
        if (_threads.empty()) {
            auto const self = shared_from_this();
            for (size_t i = 0; i < numThreads; ++i) {
                _threads.push_back(WorkerProcessorThread::create(self));
            }
        }

        // Tell each thread to run
        for (auto&& t : _threads) {
            t->run();
        }
        _state = STATE_IS_RUNNING;
    }
}

void WorkerProcessor::stop() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    if (_state == STATE_IS_RUNNING) {
        // Tell each thread to stop.
        for (auto&& t : _threads) {
            t->stop();
        }

        // Begin transitioning to the final state via this intermediate one.
        // The transition will finish asynchronous when all threads will report
        // desired changes in their states.
        _state = STATE_IS_STOPPING;
    }
}

void WorkerProcessor::drain() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    // Collect identifiers of requests to be affected by the operation
    list<string> ids;
    for (auto&& ptr : _newRequests) ids.push_back(ptr->id());
    for (auto&& entry : _inProgressRequests) ids.push_back(entry.first);
    for (auto&& id : ids) _dequeueOrCancelImpl(lock, id);
}

void WorkerProcessor::reconfig() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));
    _serviceProvider->config()->reload();
}

void WorkerProcessor::enqueueForReplication(string const& id, int32_t priority,
                                            unsigned int requestExpirationIvalSec,
                                            ProtocolRequestReplicate const& request,
                                            ProtocolResponseReplicate& response) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << id << "  db: " << request.database()
                            << "  chunk: " << request.chunk() << "  worker: " << request.worker()
                            << "  worker_host: " << request.worker_host() << "  worker_port: "
                            << request.worker_port() << "  worker_data_dir: " << request.worker_data_dir());

    replica::Lock lock(_mtx, _context(__func__));

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerReplicationRequest::create(
                _serviceProvider, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);
    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::enqueueForDeletion(string const& id, int32_t priority,
                                         unsigned int requestExpirationIvalSec,
                                         ProtocolRequestDelete const& request,
                                         ProtocolResponseDelete& response) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << id << "  db: " << request.database()
                            << "  chunk: " << request.chunk());

    replica::Lock lock(_mtx, _context(__func__));

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerDeleteRequest::create(
                _serviceProvider, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);
    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::enqueueForFind(string const& id, int32_t priority,
                                     unsigned int requestExpirationIvalSec,
                                     ProtocolRequestFind const& request, ProtocolResponseFind& response) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << id << "  db: " << request.database()
                            << "  chunk: " << request.chunk()
                            << "  compute_cs: " << (request.compute_cs() ? "true" : "false"));

    replica::Lock lock(_mtx, _context(__func__));

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerFindRequest::create(
                _serviceProvider, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);
    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::enqueueForFindAll(string const& id, int32_t priority,
                                        unsigned int requestExpirationIvalSec,
                                        ProtocolRequestFindAll const& request,
                                        ProtocolResponseFindAll& response) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__) << "  id: " << id << "  db: " << request.database());

    replica::Lock lock(_mtx, _context(__func__));

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerFindAllRequest::create(
                _serviceProvider, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);
    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::enqueueForEcho(string const& id, int32_t priority,
                                     unsigned int requestExpirationIvalSec,
                                     ProtocolRequestEcho const& request, ProtocolResponseEcho& response) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << id << "  data.size: " << request.data().size()
                            << "  delay: " << request.delay());

    replica::Lock lock(_mtx, _context(__func__));

    // Instant response if no delay was requested
    if (0 == request.delay()) {
        WorkerPerformance performance;
        performance.setUpdateStart();
        performance.setUpdateFinish();
        response.set_status(ProtocolStatus::SUCCESS);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(performance.info().release());
        response.set_data(request.data());
        return;
    }

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerEchoRequest::create(
                _serviceProvider, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);

    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::enqueueForSql(std::string const& id, int32_t priority,
                                    unsigned int requestExpirationIvalSec, ProtocolRequestSql const& request,
                                    ProtocolResponseSql& response) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << id << "  query: " << request.query()
                            << "  user: " << request.user());

    replica::Lock lock(_mtx, _context(__func__));

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerSqlRequest::create(
                _serviceProvider, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);
    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::enqueueForDirectorIndex(string const& id, int32_t priority,
                                              unsigned int requestExpirationIvalSec,
                                              ProtocolRequestDirectorIndex const& request,
                                              ProtocolResponseDirectorIndex& response) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << id << "  db: " << request.database()
                            << "  chunk: " << request.chunk()
                            << "  has_transactions: " << (request.has_transactions() ? "true" : "false")
                            << "  transaction_id: " << request.transaction_id());

    replica::Lock lock(_mtx, _context(__func__));

    // The code below may catch exceptions if other parameters of the request
    // won't pass further validation against the present configuration of the request
    // processing service.
    try {
        auto const ptr = WorkerDirectorIndexRequest::create(
                _serviceProvider, _connectionPool, _worker, id, priority,
                [self = shared_from_this()](string const& requestId) { self->dispose(requestId); },
                requestExpirationIvalSec, request);
        _newRequests.push(ptr);
        response.set_status(ProtocolStatus::QUEUED);
        response.set_status_ext(ProtocolStatusExt::NONE);
        response.set_allocated_performance(ptr->performance().info().release());
        _setInfo(ptr, response);
    } catch (invalid_argument const& ec) {
        LOGS(_log, LOG_LVL_ERROR, _context(__func__) << "  " << ec.what());
        setDefaultResponse(response, ProtocolStatus::BAD, ProtocolStatusExt::INVALID_PARAM);
    }
}

void WorkerProcessor::checkStatus(ProtocolRequestStatus const& request, ProtocolResponseStatus& response) {
    replica::Lock lock(_mtx, _context(__func__));

    // Still waiting in the queue?
    WorkerRequest::Ptr targetRequestPtr;
    for (auto ptr : _newRequests) {
        if (ptr->id() == request.id()) {
            targetRequestPtr = ptr;
            break;
        }
    }
    if (targetRequestPtr == nullptr) {
        // Is it already being processed?
        auto itrInProgress = _inProgressRequests.find(request.id());
        if (itrInProgress != _inProgressRequests.end()) {
            targetRequestPtr = itrInProgress->second;
        }
        if (targetRequestPtr == nullptr) {
            // Has it finished?
            auto itrFinished = _finishedRequests.find(request.id());
            if (itrFinished != _finishedRequests.end()) {
                targetRequestPtr = itrFinished->second;
            }
            // No such request?
            if (targetRequestPtr == nullptr) {
                response.set_status(ProtocolStatus::BAD);
                response.set_status_ext(ProtocolStatusExt::INVALID_ID);
                return;
            }
        }
    }
    response.set_status(ProtocolStatus::SUCCESS);
    response.set_status_ext(ProtocolStatusExt::NONE);
    response.set_target_status(targetRequestPtr->status());
    response.set_target_status_ext(targetRequestPtr->extendedStatus());
    response.set_allocated_target_performance(targetRequestPtr->performance().info().release());
}

void WorkerProcessor::dequeueOrCancel(ProtocolRequestStop const& request, ProtocolResponseStop& response) {
    replica::Lock lock(_mtx, _context(__func__));
    if (_dequeueOrCancelImpl(lock, request.id()) == nullptr) {
        response.set_status(ProtocolStatus::BAD);
        response.set_status_ext(ProtocolStatusExt::INVALID_ID);
    } else {
        response.set_status(ProtocolStatus::SUCCESS);
        response.set_status_ext(ProtocolStatusExt::NONE);
    }
}

WorkerRequest::Ptr WorkerProcessor::_dequeueOrCancelImpl(replica::Lock const& lock, string const& id) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__) << "  id: " << id);

    // Still waiting in the queue?
    //
    // ATTENTION: the loop variable is a copy of (not a reference to) a shared
    // pointer to allow removing (if needed) the corresponding entry from the
    // input collection while retaining a valid copy of the pointer to be placed
    // into the next stage  collection.

    for (auto ptr : _newRequests) {
        if (ptr->id() == id) {
            // Cancel it and move it into the final queue in case if a client
            // won't be able to receive the desired status of the request due to
            // a protocol failure, etc.
            ptr->cancel();

            switch (ptr->status()) {
                case ProtocolStatus::CANCELLED: {
                    _newRequests.remove(id);
                    _finishedRequests[ptr->id()] = ptr;
                    return ptr;
                }
                default:
                    throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                      WorkerRequest::status2string(ptr->status()) + " in new requests");
            }
        }
    }

    // Is it already being processed?
    auto itrInProgress = _inProgressRequests.find(id);
    if (itrInProgress != _inProgressRequests.end()) {
        auto ptr = itrInProgress->second;
        // Tell the request to begin the cancelling protocol. The protocol
        // will take care of moving the request into the final queue when
        // the cancellation will finish.
        //
        // At the meant time we just notify the client about the cancellation status
        // of the request and let it come back later to check the updated status.
        ptr->cancel();

        switch (ptr->status()) {
            // These are the most typical states for request in this queue
            case ProtocolStatus::CANCELLED:
            case ProtocolStatus::IS_CANCELLING:

            // The following two states are also allowed here because
            // in-progress requests are still allowed to progress to the completed
            // states before reporting their new state via method:
            //    WorkerProcessor::_processingFinished()
            // Sometimes, the request just can't finish this in time due to
            // replica::Lock lock(_mtx) held by the current method. We shouldn't worry
            // about this situation here. The request will be moved into the next
            // queue as soon as replica::Lock lock(_mtx) will be released.
            case ProtocolStatus::SUCCESS:
            case ProtocolStatus::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  WorkerRequest::status2string(ptr->status()) + " in in-progress requests");
        }
    }

    // Has it finished?
    auto itrFinished = _finishedRequests.find(id);
    if (itrFinished != _finishedRequests.end()) {
        auto ptr = itrFinished->second;
        // There is nothing else we can do here other than just
        // reporting the completion status of the request. It's up to a client
        // to figure out what to do about this situation.
        switch (ptr->status()) {
            case ProtocolStatus::CANCELLED:
            case ProtocolStatus::SUCCESS:
            case ProtocolStatus::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  WorkerRequest::status2string(ptr->status()) + " in finished requests");
        }
    }

    // No request found!
    return WorkerRequest::Ptr();
}

WorkerRequest::Ptr WorkerProcessor::_trackRequestImpl(replica::Lock const& lock, string const& id) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__) << "  id: " << id);

    // Still waiting in the queue?
    for (auto&& ptr : _newRequests) {
        if (ptr->id() == id) {
            switch (ptr->status()) {
                // This state requirement is strict for the non-active requests
                case ProtocolStatus::CREATED:
                    return ptr;
                default:
                    throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                      WorkerRequest::status2string(ptr->status()) + " in new requests");
            }
        }
    }

    // Is it already being processed?
    auto itrInProgress = _inProgressRequests.find(id);
    if (itrInProgress != _inProgressRequests.end()) {
        auto ptr = itrInProgress->second;
        switch (ptr->status()) {
            // These are the most typical states for request in this queue
            case ProtocolStatus::IS_CANCELLING:
            case ProtocolStatus::IN_PROGRESS:

            // The following three states are also allowed here because
            // in-progress requests are still allowed to progress to the completed
            // states before reporting their new state via method:
            //    WorkerProcessor::_processingFinished()
            // Sometimes, the request just can't finish this in time due to
            // replica::Lock lock(_mtx) held by the current method. We shouldn't worry
            // about this situation here. The request will be moved into the next
            // queue as soon as replica::Lock lock(_mtx) will be released.
            case ProtocolStatus::CANCELLED:
            case ProtocolStatus::SUCCESS:
            case ProtocolStatus::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  WorkerRequest::status2string(ptr->status()) + " in in-progress requests");
        }
    }

    // Has it finished?
    auto itrFinished = _finishedRequests.find(id);
    if (itrFinished != _finishedRequests.end()) {
        auto ptr = itrFinished->second;
        switch (ptr->status()) {
            // This state requirement is strict for the completed requests
            case ProtocolStatus::CANCELLED:
            case ProtocolStatus::SUCCESS:
            case ProtocolStatus::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  WorkerRequest::status2string(ptr->status()) + " in finished requests");
        }
    }

    // No request found!
    return WorkerRequest::Ptr();
}

void WorkerProcessor::setServiceResponse(ProtocolServiceResponse& response, string const& id,
                                         ProtocolStatus status, bool extendedReport) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    response.set_status(status);
    response.set_start_time(_startTime);

    switch (state()) {
        case WorkerProcessor::State::STATE_IS_RUNNING:
            response.set_service_state(ProtocolServiceResponse::RUNNING);
            break;
        case WorkerProcessor::State::STATE_IS_STOPPING:
            response.set_service_state(ProtocolServiceResponse::SUSPEND_IN_PROGRESS);
            break;
        case WorkerProcessor::State::STATE_IS_STOPPED:
            response.set_service_state(ProtocolServiceResponse::SUSPENDED);
            break;
    }
    response.set_num_new_requests(_newRequests.size());
    response.set_num_in_progress_requests(_inProgressRequests.size());
    response.set_num_finished_requests(_finishedRequests.size());

    if (extendedReport) {
        for (auto&& request : _newRequests) {
            _setServiceResponseInfo(request, response.add_new_requests());
        }
        for (auto&& entry : _inProgressRequests) {
            _setServiceResponseInfo(entry.second, response.add_in_progress_requests());
        }
        for (auto&& entry : _finishedRequests) {
            _setServiceResponseInfo(entry.second, response.add_finished_requests());
        }
    }
}

void WorkerProcessor::_setServiceResponseInfo(WorkerRequest::Ptr const& request,
                                              ProtocolServiceResponseInfo* info) const {
    if (nullptr != dynamic_pointer_cast<WorkerReplicationRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::REPLICA_CREATE);
    } else if (nullptr != dynamic_pointer_cast<WorkerDeleteRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::REPLICA_DELETE);
    } else if (nullptr != dynamic_pointer_cast<WorkerFindRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::REPLICA_FIND);
    } else if (nullptr != dynamic_pointer_cast<WorkerFindAllRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::REPLICA_FIND_ALL);
    } else if (nullptr != dynamic_pointer_cast<WorkerEchoRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::TEST_ECHO);
    } else if (nullptr != dynamic_pointer_cast<WorkerSqlRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::SQL);
    } else if (nullptr != dynamic_pointer_cast<WorkerDirectorIndexRequest>(request)) {
        info->set_queued_type(ProtocolQueuedRequestType::INDEX);
    } else {
        throw logic_error(_classMethodContext(__func__) + "  unsupported request type: " + request->type() +
                          " id: " + request->id());
    }
    info->set_id(request->id());
    info->set_priority(request->priority());
}

bool WorkerProcessor::dispose(string const& id) {
    replica::Lock lock(_mtx, _context(__func__));
    // Note that only the finished requests are allowed to be disposed.
    bool found = false;
    if (auto itr = _finishedRequests.find(id); itr != _finishedRequests.end()) {
        itr->second->dispose();
        _finishedRequests.erase(itr);
        found = true;
    }
    LOGS(_log, LOG_LVL_TRACE, _context(__func__) << " id: " << id << " found: " << bool2str(found));
    return found;
}

size_t WorkerProcessor::numNewRequests() const {
    replica::Lock lock(_mtx, _context(__func__));
    return _newRequests.size();
}

size_t WorkerProcessor::numInProgressRequests() const {
    replica::Lock lock(_mtx, _context(__func__));
    return _inProgressRequests.size();
}

size_t WorkerProcessor::numFinishedRequests() const {
    replica::Lock lock(_mtx, _context(__func__));
    return _finishedRequests.size();
}

string WorkerProcessor::_classMethodContext(string const& func) { return "WorkerProcessor::" + func; }

WorkerRequest::Ptr WorkerProcessor::_fetchNextForProcessing(WorkerProcessorThread::Ptr const& processorThread,
                                                            unsigned int timeoutMilliseconds) {
    LOGS(_log, LOG_LVL_TRACE,
         _context(__func__) << "  thread: " << processorThread->id() << "  timeout: " << timeoutMilliseconds);

    // For generating random intervals within the maximum range of seconds
    // requested by a client.
    //
    // TODO: Re-implement this loop to use a condition variable instead.
    // This will improve the performance of the processor which is limited
    // by the half-latency of the wait interval.
    util::BlockPost blockPost(0, min(10U, timeoutMilliseconds));

    unsigned int totalElapsedTime = 0;
    while (totalElapsedTime < timeoutMilliseconds) {
        // IMPORTANT: make sure no wait is happening within the same
        // scope where the thread safe block is defined. Otherwise
        // the queue will be locked for all threads for the duration of
        // the wait.
        {
            replica::Lock lock(_mtx, _context(__func__));
            if (not _newRequests.empty()) {
                WorkerRequest::Ptr request = _newRequests.top();
                _newRequests.pop();
                request->start();
                _inProgressRequests[request->id()] = request;
                return request;
            }
        }
        totalElapsedTime += blockPost.wait();
    }

    // Return null pointer since noting has been found within the specified
    // timeout.

    return WorkerRequest::Ptr();
}

void WorkerProcessor::_processingRefused(WorkerRequest::Ptr const& request) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__) << "  id: " << request->id());
    replica::Lock lock(_mtx, _context(__func__));

    // Note that disposed requests won't be found in any queue.
    auto itr = _inProgressRequests.find(request->id());
    if (itr != _inProgressRequests.end()) {
        // Update request's state before moving it back into
        // the input queue.
        itr->second->stop();
        _newRequests.push(itr->second);
        _inProgressRequests.erase(itr);
    }
}

void WorkerProcessor::_processingFinished(WorkerRequest::Ptr const& request) {
    LOGS(_log, LOG_LVL_DEBUG,
         _context(__func__) << "  id: " << request->id()
                            << "  status: " << WorkerRequest::status2string(request->status()));
    replica::Lock lock(_mtx, _context(__func__));

    // Note that disposed requests won't be found in any queue.
    auto itr = _inProgressRequests.find(request->id());
    if (itr != _inProgressRequests.end()) {
        _finishedRequests[itr->first] = itr->second;
        _inProgressRequests.erase(itr);
    }
}

void WorkerProcessor::_processorThreadStopped(WorkerProcessorThread::Ptr const& processorThread) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__) << "  thread: " << processorThread->id());
    replica::Lock lock(_mtx, _context(__func__));
    if (_state == STATE_IS_STOPPING) {
        // Complete state transition if all threads are stopped
        for (auto&& t : _threads) {
            if (t->isRunning()) return;
        }
        _state = STATE_IS_STOPPED;
    }
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseReplicate& response) {
    if (nullptr == request) return;
    auto ptr = dynamic_pointer_cast<WorkerReplicationRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerReplicationRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseDelete& response) {
    auto ptr = dynamic_pointer_cast<WorkerDeleteRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerDeleteRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseFind& response) {
    auto ptr = dynamic_pointer_cast<WorkerFindRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerFindRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseFindAll& response) {
    auto ptr = dynamic_pointer_cast<WorkerFindAllRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerFindAllRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseEcho& response) {
    auto ptr = dynamic_pointer_cast<WorkerEchoRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerEchoRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseSql& response) {
    auto ptr = dynamic_pointer_cast<WorkerSqlRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerSqlRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

void WorkerProcessor::_setInfo(WorkerRequest::Ptr const& request, ProtocolResponseDirectorIndex& response) {
    auto ptr = dynamic_pointer_cast<WorkerDirectorIndexRequest>(request);
    if (not ptr) {
        throw logic_error(_classMethodContext(__func__) +
                          "(WorkerDirectorIndexRequest)"
                          "  incorrect dynamic type of request id: " +
                          request->id());
    }
    ptr->setInfo(response);
}

}  // namespace lsst::qserv::replica
