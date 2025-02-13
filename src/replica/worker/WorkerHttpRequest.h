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
#ifndef LSST_QSERV_REPLICA_WORKERHTTPREQUEST_H
#define LSST_QSERV_REPLICA_WORKERHTTPREQUEST_H

// System headers
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>

// Third party headers
#include "boost/asio.hpp"

// Qserv headers
#include "replica/proto/Protocol.h"
#include "replica/util/Common.h"
#include "replica/util/Mutex.h"
#include "replica/util/Performance.h"

// Forward declarations
namespace lsst::qserv::replica {
class ServiceProvider;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Structure WorkerHttpRequestCancelled represent an exception thrown when
 * a replication request is cancelled
 */
class WorkerHttpRequestCancelled : public std::exception {
public:
    /// @return a short description of the exception
    char const* what() const noexcept override { return "cancelled"; }
};

/**
 * Class WorkerHttpRequest is the base class for a family of the worker-side
 * requests which require non-deterministic interactions with the server's
 * environment (network, disk I/O, etc.). Generally speaking, all requests
 * which can't be implemented instantaneously fall into this category.
 */
class WorkerHttpRequest : public std::enable_shared_from_this<WorkerHttpRequest> {
public:
    /// The function type for notifications on the expiration of the request
    /// given its unique identifier.
    typedef std::function<void(std::string const&)> ExpirationCallbackType;

    WorkerHttpRequest() = delete;
    WorkerHttpRequest(WorkerHttpRequest const&) = delete;
    WorkerHttpRequest& operator=(WorkerHttpRequest const&) = delete;

    /// Destructor (can't 'override' because the base class's one is not virtual)
    /// Also, non-trivial destructor is needed to stop the request expiration
    /// timer (if any was started by the constructor).
    virtual ~WorkerHttpRequest();

    std::shared_ptr<ServiceProvider> const& serviceProvider() const { return _serviceProvider; }
    std::string const& worker() const { return _worker; }
    std::string const& type() const { return _type; }
    std::string const& id() const { return _hdr.id; }
    int priority() const { return _hdr.priority; }
    nlohmann::json const& req() const { return _req; }
    protocol::Status status() const { return _status; }
    protocol::StatusExt extendedStatus() const { return _extendedStatus; }

    WorkerPerformance const& performance() const { return _performance; }

    /**
     * This method is called from the initial state protocol::Status::CREATED in order
     * to start the request expiration timer. It's safe to call this operation
     * multiple times. Each invocation of the method will result in cancelling
     * the previously set timer (if any) and starting a new one.
     */
    void init();

    /**
     * This method is called from the initial state protocol::Status::CREATED in order
     * to prepare the request for processing (to respond to methods 'execute',
     * 'cancel', 'rollback' or 'reset'. The final state upon the completion
     * of the method should be protocol::Status::IN_PROGRESS.
     */
    void start();

    /**
     * This method should be invoked (repeatedly) to execute the request until
     * it returns 'true' or throws an exception. Note that returning 'true'
     * may mean both success or failure, depending on the completion status
     * of the request.
     *
     * This method is required to be called while the request state is protocol::Status::IN_PROGRESS.
     * The method will throw custom exception WorkerHttpRequestCancelled when it detects a cancellation
     * request.
     *
     * @return result of the operation as explained above
     */
    virtual bool execute() = 0;

    /**
     * Cancel execution of the request.
     *
     * The effect of the operation varies depending on the current state of
     * the request. The default (the base class's implementation) assumes
     * the following transitions:
     *
     * {protocol::Status::CREATED,protocol::Status::CANCELLED} -> protocol::Status::CANCELLED
     * {protocol::Status::IN_PROGRESS,protocol::Status::IS_CANCELLING} -> protocol::Status::IS_CANCELLING
     * {*} -> throw std::logic_error
     */
    virtual void cancel();

    /**
     * Roll back the request into its initial state and cleanup partial results
     * if possible.
     *
     * The effect of the operation varies depending on the current state of
     * the request. The default (the base class's implementation) assumes
     * the following transitions:
     *
     * {protocol::Status::CREATED, protocol::Status::IN_PROGRESS} -> protocol::Status::CREATED
     * {protocol::Status::IS_CANCELLING} -> protocol::Status::CANCELLED -> throw WorkerHttpRequestCancelled
     * {*} -> throw std::logic_error
     */
    virtual void rollback();

    /**
     * This method is called from *ANY* initial state in order to turn
     * the request back into the initial protocol::Status::CREATED.
     */
    void stop();

    /**
     * This method should be used to cancel the request expiration timer.
     * Normally this method is initiated during the external "garbage collection"
     * of requests to ensure all resources (including a copy of a smart pointer onto
     * objects of the request classes) held by timers get released.
     *
     * @note this method won't throw any exceptions so that it could
     *   be invoked from the destructor. All exceptions (should they
     *   occur during an execution of the method) will be intersected
     *   and reported as errors to the message logger.
     */
    void dispose() noexcept;

    /**
     * Extract the extra data from the request and put it into the response object.
     * @param includeResultIfFinished (optional) flag to include results if the request has finished
     */
    nlohmann::json toJson(bool includeResultIfFinished = false) const;

    /// @return the context string
    std::string context(std::string const& className, std::string const& func) const;

protected:
    /**
     * The normal constructor of the class
     *
     * @param serviceProvider provider is needed to access the Configuration of
     *   a setup and for validating the input parameters
     * @param worker the name of a worker. It must be the same worker as the one
     *   where the request is going to be processed.
     * @param type the type name of a request
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @param onExpired request expiration callback function
     * @throws std::invalid_argument if the worker is unknown
     */
    WorkerHttpRequest(std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker,
                      std::string const& type, protocol::QueuedRequestHdr const& hdr,
                      nlohmann::json const& req, ExpirationCallbackType const& onExpired);

    /**
     * The method is used to check if the request is entered the cancellation state.
     * The implementation assumes the following transitions:
     *
     * {protocol::Status::IN_PROGRESS} -> protocol::Status::IN_PROGRESS
     * {protocol::Status::IS_CANCELLING} -> protocol::Status::CANCELLED -> throw WorkerHttpRequestCancelled
     * {*} -> throw std::logic_error
     *
     * @param lock a lock on _mtx which acquired before calling this method
     * @param context_ a scope class/method from where the method was called
     * @throws WorkerHttpRequestCancelled if the request is being cancelled.
     * @throws std::logic_error if the state is not as expected.
     */
    void checkIfCancelling(replica::Lock const& lock, std::string const& context_);

    /** Set the status
     *
     * @note this method needs to be called within a thread-safe context
     *   when moving requests between different queues.
     *
     * @param lock a lock which acquired before calling this method
     * @param status primary status to be set
     * @param extendedStatus secondary status to be set
     */
    void setStatus(replica::Lock const& lock, protocol::Status status,
                   protocol::StatusExt extendedStatus = protocol::StatusExt::NONE);

    /**
     * Fill in the information object for the specified request based on its
     * actual type.
     * @param result an object to be filled
     */
    virtual void getResult(nlohmann::json& result) const = 0;

    /**
     * Structure ErrorContext is used for tracking errors reported by
     * method 'reportErrorIf
     */
    struct ErrorContext {
        // State of the object
        bool failed;
        protocol::StatusExt extendedStatus;

        ErrorContext() : failed(false), extendedStatus(protocol::StatusExt::NONE) {}

        /**
         * Merge the context of another object into the current one.
         *
         * @note Only the first error code will be stored when a error condition
         *  is detected. An assumption is that the first error would usually cause
         *  a "chain reaction", hence only the first one typically matters.
         *  Other details could be found in the log files if needed.
         * @param ErrorContext input context to be merged with the current state
         */
        ErrorContext& operator||(const ErrorContext& rhs) {
            if (&rhs != this) {
                if (rhs.failed and not failed) {
                    failed = true;
                    extendedStatus = rhs.extendedStatus;
                }
            }
            return *this;
        }
    };

    /**
     * Check if the error condition is set and report the error.
     * The error message will be sent to the corresponding logging
     * stream.
     *
     * @param condition if set to 'true' then there is a error condition
     * @param extendedStatus extended status corresponding to the condition
     *   (will be ignored if no error condition is present)
     * @param errorMsg a message to be reported into the log stream
     * @return the context object encapsulating values passed in parameters
     *   'condition' and 'extendedStatus'
     */
    ErrorContext reportErrorIf(bool condition, protocol::StatusExt extendedStatus,
                               std::string const& errorMsg);

    /// Return shared pointer of the desired subclass (no dynamic type checking)
    template <class T>
    std::shared_ptr<T> shared_from_base() {
        return std::static_pointer_cast<T>(shared_from_this());
    }

    // Input parameters

    std::shared_ptr<ServiceProvider> const _serviceProvider;

    std::string const _worker;
    std::string const _type;
    protocol::QueuedRequestHdr const _hdr;
    nlohmann::json const _req;

    ExpirationCallbackType _onExpired;  ///< The callback is reset when the request gets expired
                                        /// or explicitly disposed.
    unsigned int const _expirationTimeoutSec;

    /// This timer is used (if configured) to limit the total duration of time
    /// a request could exist from its creation till termination. The timer
    /// starts when the request gets created. And it's explicitly finished when
    /// a request object gets destroyed.
    ///
    /// If the time has a chance to expire then the request expiration callback
    /// (if any) passed into the constructor will be invoked to notify WorkerProcessor
    /// on the expiration event.
    boost::asio::deadline_timer _expirationTimer;

    // 2-layer state of a request

    std::atomic<protocol::Status> _status;
    std::atomic<protocol::StatusExt> _extendedStatus;

    /// Performance counters
    WorkerPerformance _performance;

    /// Mutex guarding API calls where it's needed
    mutable replica::Mutex _mtx;

    /// Mutex guarding operations with the worker's data folder
    static replica::Mutex _mtxDataFolderOperations;

private:
    /**
     * Request expiration timer's handler. The expiration interval (if any)
     * is obtained from the Controller-side requests or obtained from
     * the configuration service. When the request expires (and if the timer
     * is not aborted due to request disposal) then an upstream callback
     * is invoked.
     *
     * @param ec error code to be checked to see if the time was aborted
     *   by the explicit request disposal operation.
     */
    void _expired(boost::system::error_code const& ec);

    // For memory usage monitoring and memory leak diagnostic.
    static std::atomic<size_t> _numInstances;
};

/**
 * Structure WorkerHttpRequestCompare is a functor representing a comparison type
 * for strict weak ordering required by std::priority_queue
 */
struct WorkerHttpRequestCompare {
    /**
     * Sort requests by their priorities
     * @param lhs pointer to a request on the left side of a logical comparison
     * @param rhs pointer to a request on the right side of a logical comparison
     * @return 'true' if the priority of 'lhs' is strictly less than the one of 'rhs'
     */
    bool operator()(std::shared_ptr<WorkerHttpRequest> const& lhs,
                    std::shared_ptr<WorkerHttpRequest> const& rhs) const {
        return lhs->priority() < rhs->priority();
    }
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERHTTPREQUEST_H
