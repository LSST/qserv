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
#include "replica/worker/WorkerHttpProcessor.h"

// System headers
#include <algorithm>
#include <iterator>
#include <stdexcept>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/mysql/DatabaseMySQL.h"
#include "replica/services/ServiceProvider.h"
#include "replica/worker/WorkerHttpProcessorThread.h"
#include "replica/worker/WorkerHttpRequest.h"
#include "replica/worker/WorkerCreateReplicaHttpRequest.h"
#include "replica/worker/WorkerDeleteReplicaHttpRequest.h"
#include "replica/worker/WorkerDirectorIndexHttpRequest.h"
#include "replica/worker/WorkerEchoHttpRequest.h"
#include "replica/worker/WorkerFindReplicaHttpRequest.h"
#include "replica/worker/WorkerFindAllReplicasHttpRequest.h"
#include "replica/worker/WorkerSqlHttpRequest.h"
#include "util/BlockPost.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;
using json = nlohmann::json;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerHttpProcessor");
}  // namespace

namespace lsst::qserv::replica {

bool WorkerHttpProcessor::PriorityQueueType::remove(string const& id) {
    auto itr = find_if(c.begin(), c.end(),
                       [&id](shared_ptr<WorkerHttpRequest> const& ptr) { return ptr->id() == id; });
    if (itr != c.end()) {
        c.erase(itr);
        make_heap(c.begin(), c.end(), comp);
        return true;
    }
    return false;
}

shared_ptr<WorkerHttpProcessor> WorkerHttpProcessor::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker) {
    return shared_ptr<WorkerHttpProcessor>(new WorkerHttpProcessor(serviceProvider, worker));
}

WorkerHttpProcessor::WorkerHttpProcessor(shared_ptr<ServiceProvider> const& serviceProvider,
                                         string const& worker)
        : _serviceProvider(serviceProvider),
          _worker(worker),
          _connectionPool(database::mysql::ConnectionPool::create(
                  Configuration::qservWorkerDbParams(),
                  serviceProvider->config()->get<size_t>("database", "services-pool-size"))),
          _state(protocol::ServiceState::SUSPENDED),
          _startTime(util::TimeUtils::now()) {}

void WorkerHttpProcessor::run() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    if (_state == protocol::ServiceState::SUSPENDED) {
        size_t const numThreads =
                _serviceProvider->config()->get<size_t>("worker", "num-svc-processing-threads");
        if (numThreads == 0) {
            throw out_of_range(_classMethodContext(__func__) +
                               "invalid configuration parameter for the number of processing threads. "
                               "The value of the parameter must be greater than 0");
        }

        // Create threads if needed
        if (_threads.empty()) {
            auto const self = shared_from_this();
            for (size_t i = 0; i < numThreads; ++i) {
                _threads.push_back(WorkerHttpProcessorThread::create(self));
            }
        }

        // Tell each thread to run
        for (auto&& t : _threads) {
            t->run();
        }
        _state = protocol::ServiceState::RUNNING;
    }
}

void WorkerHttpProcessor::stop() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    if (_state == protocol::ServiceState::RUNNING) {
        // Tell each thread to stop.
        for (auto&& t : _threads) {
            t->stop();
        }

        // Begin transitioning to the final state via this intermediate one.
        // The transition will finish asynchronous when all threads will report
        // desired changes in their states.
        _state = protocol::ServiceState::SUSPEND_IN_PROGRESS;
    }
}

void WorkerHttpProcessor::drain() {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__));
    replica::Lock lock(_mtx, _context(__func__));

    // Collect identifiers of requests to be affected by the operation
    list<string> ids;
    for (auto&& ptr : _newRequests) ids.push_back(ptr->id());
    for (auto&& entry : _inProgressRequests) ids.push_back(entry.first);
    for (auto&& id : ids) _stopRequestImpl(lock, id);
}

void WorkerHttpProcessor::reconfig() {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context);
    replica::Lock lock(_mtx, context);
    _serviceProvider->config()->reload();
}

json WorkerHttpProcessor::createReplica(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerCreateReplicaHttpRequest>(replica::Lock(_mtx, context), context, hdr, req);
}

json WorkerHttpProcessor::deleteReplica(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerDeleteReplicaHttpRequest>(replica::Lock(_mtx, context), context, hdr, req);
}

json WorkerHttpProcessor::findReplica(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerFindReplicaHttpRequest>(replica::Lock(_mtx, context), context, hdr, req);
}

json WorkerHttpProcessor::findAllReplicas(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerFindAllReplicasHttpRequest>(replica::Lock(_mtx, context), context, hdr, req);
}

json WorkerHttpProcessor::echo(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerEchoHttpRequest>(replica::Lock(_mtx, context), context, hdr, req);
}

json WorkerHttpProcessor::sql(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerSqlHttpRequest>(replica::Lock(_mtx, context), context, hdr, req);
}

json WorkerHttpProcessor::index(protocol::QueuedRequestHdr const& hdr, json const& req) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << hdr.id);
    return _submit<WorkerDirectorIndexHttpRequest>(replica::Lock(_mtx, context), context, hdr, req,
                                                   _connectionPool);
}

json WorkerHttpProcessor::requestStatus(string const& id) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << id);

    replica::Lock lock(_mtx, context);

    // Still waiting in the queue?
    shared_ptr<WorkerHttpRequest> targetRequestPtr;
    for (auto ptr : _newRequests) {
        if (ptr->id() == id) {
            targetRequestPtr = ptr;
            break;
        }
    }
    if (targetRequestPtr == nullptr) {
        // Is it already being processed?
        auto itrInProgress = _inProgressRequests.find(id);
        if (itrInProgress != _inProgressRequests.end()) {
            targetRequestPtr = itrInProgress->second;
        }
        if (targetRequestPtr == nullptr) {
            // Has it finished?
            auto itrFinished = _finishedRequests.find(id);
            if (itrFinished != _finishedRequests.end()) {
                targetRequestPtr = itrFinished->second;
            }
            // No such request?
            if (targetRequestPtr == nullptr) {
                return json::object(
                        {{"status", protocol::Status::BAD},
                         {"status_str", protocol::toString(protocol::Status::BAD)},
                         {"status_ext", protocol::StatusExt::INVALID_ID},
                         {"status_ext_str", protocol::toString(protocol::StatusExt::INVALID_ID)}});
            }
        }
    }
    return targetRequestPtr->toJson();
}

json WorkerHttpProcessor::stopRequest(string const& id) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << id);

    replica::Lock lock(_mtx, context);

    json response = json::object();
    auto const request = _stopRequestImpl(lock, id);
    if (request == nullptr) {
        response["status"] = protocol::Status::BAD;
        response["status_str"] = protocol::toString(protocol::Status::BAD);
        response["status_ext"] = protocol::StatusExt::INVALID_ID;
        response["status_ext_str"] = protocol::toString(protocol::StatusExt::INVALID_ID);
    } else {
        response = request->toJson();
    }
    return response;
}

json WorkerHttpProcessor::trackRequest(string const& id) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << id);

    replica::Lock lock(_mtx, context);

    json response = json::object();
    auto const request = _trackRequestImpl(lock, id);
    if (request == nullptr) {
        response["status"] = protocol::Status::BAD;
        response["status_str"] = protocol::toString(protocol::Status::BAD);
        response["status_ext"] = protocol::StatusExt::INVALID_ID;
        response["status_ext_str"] = protocol::toString(protocol::StatusExt::INVALID_ID);
    } else {
        bool const includeResultIfFinished = true;
        response = request->toJson(includeResultIfFinished);
    }
    return response;
}

bool WorkerHttpProcessor::disposeRequest(string const& id) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << id);

    replica::Lock lock(_mtx, context);

    // Note that only the finished requests are allowed to be disposed.
    if (auto itr = _finishedRequests.find(id); itr != _finishedRequests.end()) {
        itr->second->dispose();
        _finishedRequests.erase(itr);
        return true;
    }
    return false;
}

size_t WorkerHttpProcessor::numNewRequests() const {
    replica::Lock lock(_mtx, _context(__func__));
    return _newRequests.size();
}

size_t WorkerHttpProcessor::numInProgressRequests() const {
    replica::Lock lock(_mtx, _context(__func__));
    return _inProgressRequests.size();
}

size_t WorkerHttpProcessor::numFinishedRequests() const {
    replica::Lock lock(_mtx, _context(__func__));
    return _finishedRequests.size();
}

json WorkerHttpProcessor::toJson(protocol::Status status, bool includeRequests) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context);

    replica::Lock lock(_mtx, context);

    json response;
    response["status"] = status;
    response["status_str"] = protocol::toString(status);
    response["status_ext"] = protocol::StatusExt::NONE;
    response["status_ext_str"] = protocol::toString(protocol::StatusExt::NONE);
    response["service_state"] = state();
    response["service_state_str"] = protocol::toString(state());
    response["num_new_requests"] = _newRequests.size();
    response["num_in_progress_requests"] = _inProgressRequests.size();
    response["num_finished_requests"] = _finishedRequests.size();
    response["new_requests"] = json::array();
    response["in_progress_requests"] = json::array();
    response["finished_requests"] = json::array();

    if (includeRequests) {
        for (auto const& request : _newRequests) {
            response["new_requests"].push_back(request->toJson());
        }
        for (auto const& entry : _inProgressRequests) {
            response["in_progress_requests"].push_back(entry.second->toJson());
        }
        for (auto const& entry : _finishedRequests) {
            response["finished_requests"].push_back(entry.second->toJson());
        }
    }
    return response;
}

string WorkerHttpProcessor::_classMethodContext(string const& func) { return "WorkerHttpProcessor::" + func; }

void WorkerHttpProcessor::_logError(string const& context, string const& message) const {
    LOGS(_log, LOG_LVL_ERROR, context << "  " << message);
}

shared_ptr<WorkerHttpRequest> WorkerHttpProcessor::_stopRequestImpl(replica::Lock const& lock,
                                                                    string const& id) {
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
                case protocol::Status::CANCELLED: {
                    _newRequests.remove(id);
                    _finishedRequests[ptr->id()] = ptr;
                    return ptr;
                }
                default:
                    throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                      protocol::toString(ptr->status()) + " in new requests");
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
            case protocol::Status::CANCELLED:
            case protocol::Status::IS_CANCELLING:

            // The following two states are also allowed here because
            // in-progress requests are still allowed to progress to the completed
            // states before reporting their new state via method:
            //    WorkerHttpProcessor::_processingFinished()
            // Sometimes, the request just can't finish this in time due to
            // replica::Lock lock(_mtx) held by the current method. We shouldn't worry
            // about this situation here. The request will be moved into the next
            // queue as soon as replica::Lock lock(_mtx) will be released.
            case protocol::Status::SUCCESS:
            case protocol::Status::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  protocol::toString(ptr->status()) + " in in-progress requests");
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
            case protocol::Status::CANCELLED:
            case protocol::Status::SUCCESS:
            case protocol::Status::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  protocol::toString(ptr->status()) + " in finished requests");
        }
    }

    // No request found!
    return nullptr;
}

shared_ptr<WorkerHttpRequest> WorkerHttpProcessor::_trackRequestImpl(replica::Lock const& lock,
                                                                     string const& id) {
    LOGS(_log, LOG_LVL_DEBUG, _context(__func__) << "  id: " << id);

    // Still waiting in the queue?
    for (auto&& ptr : _newRequests) {
        if (ptr->id() == id) {
            switch (ptr->status()) {
                // This state requirement is strict for the non-active requests
                case protocol::Status::CREATED:
                    return ptr;
                default:
                    throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                      protocol::toString(ptr->status()) + " in new requests");
            }
        }
    }

    // Is it already being processed?
    auto itrInProgress = _inProgressRequests.find(id);
    if (itrInProgress != _inProgressRequests.end()) {
        auto ptr = itrInProgress->second;
        switch (ptr->status()) {
            // These are the most typical states for request in this queue
            case protocol::Status::IS_CANCELLING:
            case protocol::Status::IN_PROGRESS:

            // The following three states are also allowed here because
            // in-progress requests are still allowed to progress to the completed
            // states before reporting their new state via method:
            //    WorkerHttpProcessor::_processingFinished()
            // Sometimes, the request just can't finish this in time due to
            // replica::Lock lock(_mtx) held by the current method. We shouldn't worry
            // about this situation here. The request will be moved into the next
            // queue as soon as replica::Lock lock(_mtx) will be released.
            case protocol::Status::CANCELLED:
            case protocol::Status::SUCCESS:
            case protocol::Status::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  protocol::toString(ptr->status()) + " in in-progress requests");
        }
    }

    // Has it finished?
    auto itrFinished = _finishedRequests.find(id);
    if (itrFinished != _finishedRequests.end()) {
        auto ptr = itrFinished->second;
        switch (ptr->status()) {
            // This state requirement is strict for the completed requests
            case protocol::Status::CANCELLED:
            case protocol::Status::SUCCESS:
            case protocol::Status::FAILED:
                return ptr;
            default:
                throw logic_error(_classMethodContext(__func__) + "  unexpected request status " +
                                  protocol::toString(ptr->status()) + " in finished requests");
        }
    }

    // No request found!
    return nullptr;
}

shared_ptr<WorkerHttpRequest> WorkerHttpProcessor::_fetchNextForProcessing(
        shared_ptr<WorkerHttpProcessorThread> const& processorThread, unsigned int timeoutMilliseconds) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_TRACE,
         context << "  thread: " << processorThread->id() << "  timeout: " << timeoutMilliseconds);

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
            replica::Lock lock(_mtx, context);
            if (!_newRequests.empty()) {
                shared_ptr<WorkerHttpRequest> request = _newRequests.top();
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
    return nullptr;
}

void WorkerHttpProcessor::_processingRefused(shared_ptr<WorkerHttpRequest> const& request) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  id: " << request->id());

    replica::Lock lock(_mtx, context);

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

void WorkerHttpProcessor::_processingFinished(shared_ptr<WorkerHttpRequest> const& request) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG,
         context << "  id: " << request->id() << "  status: " << protocol::toString(request->status()));

    replica::Lock lock(_mtx, context);

    // Note that disposed requests won't be found in any queue.
    auto itr = _inProgressRequests.find(request->id());
    if (itr != _inProgressRequests.end()) {
        _finishedRequests[itr->first] = itr->second;
        _inProgressRequests.erase(itr);
    }
}

void WorkerHttpProcessor::_processorThreadStopped(
        shared_ptr<WorkerHttpProcessorThread> const& processorThread) {
    string const context = _context(__func__);
    LOGS(_log, LOG_LVL_DEBUG, context << "  thread: " << processorThread->id());

    replica::Lock lock(_mtx, context);

    if (_state == protocol::ServiceState::SUSPEND_IN_PROGRESS) {
        // Complete state transition if all threads are stopped
        for (auto&& t : _threads) {
            if (t->isRunning()) return;
        }
        _state = protocol::ServiceState::SUSPENDED;
    }
}

}  // namespace lsst::qserv::replica
