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
#ifndef LSST_QSERV_REPLICA_WORKERHTTPPROCESSOR_H
#define LSST_QSERV_REPLICA_WORKERHTTPPROCESSOR_H

// System headers
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

// Qserv headers
#include "replica/proto/Protocol.h"
#include "replica/util/Mutex.h"
#include "replica/worker/WorkerHttpRequest.h"

// Third party headers
#include "nlohmann/json.hpp"

// Forward declarations

namespace lsst::qserv::replica {
class ServiceProvider;
class WorkerHttpProcessorThread;
}  // namespace lsst::qserv::replica

namespace lsst::qserv::replica::database::mysql {
class ConnectionPool;
}  // namespace lsst::qserv::replica::database::mysql

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerHttpProcessor is a front-end interface for processing
 * requests from remote clients within worker-side services.
 */
class WorkerHttpProcessor : public std::enable_shared_from_this<WorkerHttpProcessor> {
public:
    // The thread-based processor class is allowed to access the internal API
    friend class WorkerHttpProcessorThread;

    /**
     * Structure PriorityQueueType extends the standard priority queue for pointers
     * to the new (unprocessed) requests.
     *
     * Its design relies upon the inheritance to get access to the protected
     * data members 'c' representing the internal container of the base queue
     * in order to implement the iterator protocol.
     */
    struct PriorityQueueType
            : std::priority_queue<std::shared_ptr<WorkerHttpRequest>,
                                  std::vector<std::shared_ptr<WorkerHttpRequest>>, WorkerHttpRequestCompare> {
        /// @return iterator to the beginning of the container
        decltype(c.begin()) begin() { return c.begin(); }

        /// @return iterator to the end of the container
        decltype(c.end()) end() { return c.end(); }

        /**
         * Remove a request from the queue by its identifier
         * @param id an identifier of a request
         * @return 'true' if the object was actually removed
         */
        bool remove(std::string const& id);
    };

    /**
     * The factory method for objects of the class
     *
     * @param serviceProvider provider is needed to access the Configuration of
     *   a setup in order to get a number of the processing threads to be launched
     *   by the processor.
     * @param worker the name of a worker
     * @return a pointer to the created object
     */
    static std::shared_ptr<WorkerHttpProcessor> create(
            std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker);

    WorkerHttpProcessor() = delete;
    WorkerHttpProcessor(WorkerHttpProcessor const&) = delete;
    WorkerHttpProcessor& operator=(WorkerHttpProcessor const&) = delete;

    ~WorkerHttpProcessor() = default;

    /// @return the state of the processor
    protocol::ServiceState state() const { return _state; }

    /// Begin processing requests
    void run();

    /// Stop processing all requests, and stop all threads
    void stop();

    /// Drain (cancel) all queued and in-progress requests
    void drain();

    /// Reload Configuration
    void reconfig();

    /**
     * Enqueue the replica creation request for processing
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json createReplica(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Enqueue the replica deletion request for processing
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json deleteReplica(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Enqueue the replica lookup request for processing
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json findReplica(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Enqueue the multi-replica lookup request for processing
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json findAllReplicas(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Enqueue the worker-side testing request for processing
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json echo(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Enqueue a request for querying the worker database
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json sql(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Enqueue a request for extracting the "director" index data from
     * the director tables.
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    nlohmann::json index(protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req);

    /**
     * Get a status of the request
     * @param id an identifier of a request affected by the operation
     * @return the response object to be sent back to a client
     */
    nlohmann::json requestStatus(std::string const& id);

    /**
     * Dequeue replication request
     * @note If the request is not being processed yet then it will be simply removed
     *   from the ready-to-be-processed queue. If it's being processed an attempt
     *   to cancel processing will be made. If it has already processed this will
     *   be reported.
     * @param id an identifier of a request affected by the operation
     * @return the response object to be sent back to a client
     */
    nlohmann::json stopRequest(std::string const& id);

    /**
     * Return the tracking info on the on-going request
     * @param id an identifier of a request affected by the operation
     * @return the response object to be sent back to a client
     */
    nlohmann::json trackRequest(std::string const& id);

    /**
     * Find the request in any queue, and "garbage collect" it to release resources
     * associated with the request. If the request is still in the "in-progress"
     * state then it will be "drained" before disposing. If the request isn't found
     * in any queue then nothing will happen (no exception thrown, no side effects).
     *
     * @param id an identifier of a request affected by the operation
     * @return 'true' if the request was found and actually removed from any queue
     */
    bool disposeRequest(std::string const& id);

    size_t numNewRequests() const;
    size_t numInProgressRequests() const;
    size_t numFinishedRequests() const;

    /**
     * Capture the processor's state and counters.
     * @param status desired status to set in the response objet
     * @param includeRequests (optional) flag to return detailed info on all known requests
     * @return the response object to be sent back to a client
     */
    nlohmann::json toJson(protocol::Status status, bool includeRequests = false);

private:
    WorkerHttpProcessor(std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker);

    static std::string _classMethodContext(std::string const& func);

    /**
     * Submit a request for processing
     * @param lock a lock on _mtx to be acquired before calling this method
     * @param context the logging context (including the name of a function/method)
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @return the response object to be sent back to a client
     */
    template <typename REQUEST_TYPE, typename... Args>
    nlohmann::json _submit(replica::Lock const& lock, std::string const& context,
                           protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req, Args... args) {
        try {
            auto const ptr = REQUEST_TYPE::create(
                    _serviceProvider, _worker, hdr, req,
                    [self = shared_from_this()](std::string const& id) { self->disposeRequest(id); },
                    args...);
            _newRequests.push(ptr);
            return ptr->toJson();
        } catch (std::exception const& ec) {
            _logError(context, ec.what());
            return nlohmann::json::object(
                    {{"status", protocol::Status::BAD},
                     {"status_str", protocol::toString(protocol::Status::BAD)},
                     {"status_ext", protocol::StatusExt::INVALID_PARAM},
                     {"status_ext_str", protocol::toString(protocol::StatusExt::INVALID_PARAM)}});
        }
    }

    /**
     * Log the error message.
     * @param context the logging context (including the name of a function/method)
     * @param message the error message to be reported
     */
    void _logError(std::string const& context, std::string const& message) const;

    /**
     * Return the next request which is ready to be processed
     * and if then one found assign it to the specified thread. The request
     * will be removed from the ready-to-be-processed queue.
     *
     * If the one is available within the specified timeout then such request
     * will be moved into the in-progress queue, assigned to the processor thread
     * and returned to a caller. Otherwise an empty pointer (pointing to nullptr)
     * will be returned.
     *
     * This method is supposed to be called by one of the processing threads
     * when it becomes available.
     *
     * @note this method will block for a duration of time not exceeding
     *   the client-specified timeout unless it's set to 0. In the later
     *   case the method will block indefinitely.
     * @param processorThread reference to a thread which fetches the next request
     * @param timeoutMilliseconds (optional) amount of time to wait before to finish if
     *   no suitable requests are available for processing
     */
    std::shared_ptr<WorkerHttpRequest> _fetchNextForProcessing(
            std::shared_ptr<WorkerHttpProcessorThread> const& processorThread,
            unsigned int timeoutMilliseconds = 0);

    /**
     * Implement the operation for the specified identifier if such request
     * is still known to the Processor. Return a reference to the request object
     * whose state will be properly updated.
     * @param lock а lock on _mtx to be acquired before calling this method
     * @param id an identifier of a request
     * @return the request object (if found) or nullptr otherwise
     */
    std::shared_ptr<WorkerHttpRequest> _stopRequestImpl(replica::Lock const& lock, std::string const& id);

    /**
     * Find and return a reference to the request object.
     * @param lock а lock on _mtx to be acquired before calling this method
     * @param id an identifier of a request
     * @return the request object (if found) or nullptr otherwise
     */
    std::shared_ptr<WorkerHttpRequest> _trackRequestImpl(replica::Lock const& lock, std::string const& id);

    /**
     * Report a decision not to process a request
     *
     * This method is supposed to be called by one of the processing threads
     * after it fetches the next ready-to-process request and then decided
     * not to proceed with processing. Normally this should happen when
     * the thread was asked to stop. In that case the request will be put
     * back into the ready-to-be processed request and be picked up later
     * by some other thread.
     *
     * @param request a pointer to the request
     */
    void _processingRefused(std::shared_ptr<WorkerHttpRequest> const& request);

    /**
     * Report a request which has been processed or cancelled.
     *
     * The method is called by a thread which was processing the request.
     * The request will be moved into the corresponding queue. A proper
     * completion status is expected be stored within the request.
     *
     * @param request a pointer to the request
     */
    void _processingFinished(std::shared_ptr<WorkerHttpRequest> const& request);

    /**
     * For threads reporting their completion
     *
     * This method is used by threads to report a change in their state.
     * It's meant to be used during the gradual and asynchronous state transition
     * of this processor from the combined State::STATE_IS_STOPPING to
     * State::STATE_IS_STOPPED. The later is achieved when all threads are stopped.
     *
     * @param processorThread reference to the processing thread which finished
     */
    void _processorThreadStopped(std::shared_ptr<WorkerHttpProcessorThread> const& processorThread);

    std::string _context(std::string const& func = std::string()) const { return "PROCESSOR  " + func; }

    std::shared_ptr<ServiceProvider> const _serviceProvider;
    std::string const _worker;
    std::shared_ptr<database::mysql::ConnectionPool> const _connectionPool;

    protocol::ServiceState _state;
    uint64_t _startTime;  /// When the processor started (milliseconds since UNIX Epoch)

    std::vector<std::shared_ptr<WorkerHttpProcessorThread>> _threads;

    mutable replica::Mutex _mtx;  /// Mutex guarding the queues

    PriorityQueueType _newRequests;
    std::map<std::string, std::shared_ptr<WorkerHttpRequest>> _inProgressRequests;
    std::map<std::string, std::shared_ptr<WorkerHttpRequest>> _finishedRequests;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERHTTPPROCESSOR_H
