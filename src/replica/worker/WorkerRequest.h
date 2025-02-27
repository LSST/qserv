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
#ifndef LSST_QSERV_REPLICA_WORKERREQUEST_H
#define LSST_QSERV_REPLICA_WORKERREQUEST_H

// System headers
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <string>

// Third party headers
#include "boost/asio.hpp"

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/Common.h"
#include "replica/util/Mutex.h"
#include "replica/util/Performance.h"

// This header declarations
namespace lsst::qserv::replica {

/**
 * Structure WorkerRequestCancelled represent an exception thrown when
 * a replication request is cancelled
 */
class WorkerRequestCancelled : public std::exception {
public:
    /// @return a short description of the exception
    char const* what() const noexcept override { return "cancelled"; }
};

/**
 * Class WorkerRequest is the base class for a family of the worker-side
 * requests which require non-deterministic interactions with the server's
 * environment (network, disk I/O, etc.). Generally speaking, all requests
 * which can't be implemented instantaneously fall into this category.
 */
class WorkerRequest : public std::enable_shared_from_this<WorkerRequest> {
public:
    typedef std::shared_ptr<WorkerRequest> Ptr;

    /// The function type for notifications on the expiration of the request
    /// given its unique identifier.
    typedef std::function<void(std::string const&)> ExpirationCallbackType;

    /// @return the string representation of the status
    static std::string status2string(ProtocolStatus status);

    /// @return the string representation of the full status
    static std::string status2string(ProtocolStatus status, ProtocolStatusExt extendedStatus);

    WorkerRequest() = delete;
    WorkerRequest(WorkerRequest const&) = delete;
    WorkerRequest& operator=(WorkerRequest const&) = delete;

    /// Destructor (can't 'override' because the base class's one is not virtual)
    /// Also, non-trivial destructor is needed to stop the request expiration
    /// timer (if any was started by the constructor).
    virtual ~WorkerRequest();

    // Trivial getter methods

    ServiceProvider::Ptr const& serviceProvider() const { return _serviceProvider; }

    std::string const& worker() const { return _worker; }
    std::string const& type() const { return _type; }
    std::string const& id() const { return _id; }

    int priority() const { return _priority; }

    ProtocolStatus status() const { return _status; }

    ProtocolStatusExt extendedStatus() const { return _extendedStatus; }

    /// @return the performance info
    const WorkerPerformance& performance() const { return _performance; }

    /**
     * This method is called from the initial state ProtocolStatus::CREATED in order
     * to start the request expiration timer. It's safe to call this operation
     * multiple times. Each invocation of the method will result in cancelling
     * the previously set timer (if any) and starting a new one.
     */
    void init();

    /**
     * This method is called from the initial state ProtocolStatus::CREATED in order
     * to prepare the request for processing (to respond to methods 'execute',
     * 'cancel', 'rollback' or 'reset'. The final state upon the completion
     * of the method should be ProtocolStatus::IN_PROGRESS.
     */
    void start();

    /**
     * This method should be invoked (repeatedly) to execute the request until
     * it returns 'true' or throws an exception. Note that returning 'true'
     * may mean both success or failure, depending on the completion status
     * of the request.
     *
     * This method is required to be called while the request state is ProtocolStatus::IN_PROGRESS.
     * The method will throw custom exception WorkerRequestCancelled when it detects a cancellation request.
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
     * {ProtocolStatus::CREATED,ProtocolStatus::CANCELLED} -> ProtocolStatus::CANCELLED
     * {ProtocolStatus::IN_PROGRESS,ProtocolStatus::IS_CANCELLING} -> ProtocolStatus::IS_CANCELLING
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
     * {ProtocolStatus::CREATED, ProtocolStatus::IN_PROGRESS} -> ProtocolStatus::CREATED
     * {ProtocolStatus::IS_CANCELLING} -> ProtocolStatus::CANCELLED -> throw WorkerRequestCancelled
     * {*} -> throw std::logic_error
     */
    virtual void rollback();

    /**
     * This method is called from *ANY* initial state in order to turn
     * the request back into the initial ProtocolStatus::CREATED.
     * @param func (optional) the name of a function/method which requested the context string
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

    /// @return the context string
    std::string context(std::string const& func = std::string()) const {
        return id() + "  " + type() + "  " + status2string(status()) + "  " + func;
    }

protected:
    /**
     * The normal constructor of the class
     *
     * @param serviceProvider provider is needed to access the Configuration of
     *   a setup and for validating the input parameters
     * @param worker the name of a worker. It must be the same worker as the one
     *   where the request is going to be processed.
     * @param type the type name of a request
     * @param id an identifier of a client request
     * @param priority indicates the importance of the request
     * @param (optional) onExpired request expiration callback function.
     *   If nullptr is passed as a parameter then the request will never expire.
     * @param (optional) requestExpirationIvalSec request expiration interval.
     *   If 0 is passed into the method then a value of the corresponding
     *   parameter for the Controller-side requests will be pulled from
     *   the Configuration.
     * @throws std::invalid_argument if the worker is unknown
     */
    WorkerRequest(ServiceProvider::Ptr const& serviceProvider, std::string const& worker,
                  std::string const& type, std::string const& id, int priority,
                  ExpirationCallbackType const& onExpired = nullptr,
                  unsigned int requestExpirationIvalSec = 0);

    /**
     * The method is used to check if the request is entered the cancellation state.
     * The implementation assumes the following transitions:
     *
     * {ProtocolStatus::IN_PROGRESS} -> ProtocolStatus::IN_PROGRESS
     * {ProtocolStatus::IS_CANCELLING} -> ProtocolStatus::CANCELLED -> throw WorkerRequestCancelled
     * {*} -> throw std::logic_error
     *
     * @param lock a lock on _mtx which acquired before calling this method
     * @param func the name of a function/method which called the method
     * @throws WorkerRequestCancelled if the request is being cancelled.
     * @throws std::logic_error if the state is not as expected.
     */
    void checkIfCancelling(replica::Lock const& lock, std::string const& func);

    /** Set the status
     *
     * @note this method needs to be called within a thread-safe context
     *   when moving requests between different queues.
     *
     * @param lock a lock which acquired before calling this method
     * @param status primary status to be set
     * @param extendedStatus secondary status to be set
     */
    void setStatus(replica::Lock const& lock, ProtocolStatus status,
                   ProtocolStatusExt extendedStatus = ProtocolStatusExt::NONE);

    /**
     * Structure ErrorContext is used for tracking errors reported by
     * method 'reportErrorIf
     */
    struct ErrorContext {
        // State of the object
        bool failed;
        ProtocolStatusExt extendedStatus;

        ErrorContext() : failed(false), extendedStatus(ProtocolStatusExt::NONE) {}

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
    ErrorContext reportErrorIf(bool condition, ProtocolStatusExt extendedStatus, std::string const& errorMsg);

    /// Return shared pointer of the desired subclass (no dynamic type checking)
    template <class T>
    std::shared_ptr<T> shared_from_base() {
        return std::static_pointer_cast<T>(shared_from_this());
    }

    // Input parameters

    ServiceProvider::Ptr const _serviceProvider;

    std::string const _worker;
    std::string const _type;
    std::string const _id;
    int const _priority;

    ExpirationCallbackType _onExpired;  ///< The callback is reset when the request gets expired
                                        /// or explicitly disposed.
    unsigned int const _requestExpirationIvalSec;

    /// This timer is used (if configured) to limit the total duration of time
    /// a request could exist from its creation till termination. The timer
    /// starts when the request gets created. And it's explicitly finished when
    /// a request object gets destroyed.
    ///
    /// If the time has a chance to expire then the request expiration callback
    /// (if any) passed into the constructor will be invoked to notify WorkerProcessor
    /// on the expiration event.
    boost::asio::deadline_timer _requestExpirationTimer;

    // 2-layer state of a request

    std::atomic<ProtocolStatus> _status;
    std::atomic<ProtocolStatusExt> _extendedStatus;

    /// Performance counters
    WorkerPerformance _performance;

    /// The number of milliseconds since the beginning of the request processing.
    /// This members is used by the default implementation of method execute()
    /// to simulate request processing.
    unsigned int _durationMillisec;

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
 * Structure WorkerRequestCompare is a functor representing a comparison type
 * for strict weak ordering required by std::priority_queue
 */
struct WorkerRequestCompare {
    /**
     * Sort requests by their priorities
     * @param lhs pointer to a request on the left side of a logical comparison
     * @param rhs pointer to a request on the right side of a logical comparison
     * @return 'true' if the priority of 'lhs' is strictly less than the one of 'rhs'
     */
    bool operator()(WorkerRequest::Ptr const& lhs, WorkerRequest::Ptr const& rhs) const {
        return lhs->priority() < rhs->priority();
    }
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERREQUEST_H
