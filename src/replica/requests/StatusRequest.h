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
#ifndef LSST_QSERV_REPLICA_STATUSREQUEST_H
#define LSST_QSERV_REPLICA_STATUSREQUEST_H

// System headers
#include <functional>
#include <iostream>
#include <memory>
#include <string>

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "replica/requests/Request.h"
#include "replica/util/Common.h"
#include "replica/util/Performance.h"

// Forward declarations
namespace lsst::qserv::replica {
class Controller;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class StatusRequest is used for checking a status of the previously submitted requests.
 */
class StatusRequest : public Request {
public:
    typedef std::shared_ptr<StatusRequest> Ptr;

    /// The function type for notifications on the completion of the request
    typedef std::function<void(Ptr)> CallbackType;

    /**
     * Create a new request with specified parameters.
     *
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * Class-specific parameters are documented below:
     * @param targetRequestId An identifier of the target request whose remote status
     *   is going to be inspected.
     *
     * @see The very base class Request for the description of the common parameters
     *   of all subclasses.
     *
     * @return A pointer to the created object.
     */
    static Ptr createAndStart(std::shared_ptr<Controller> const& controller, std::string const& workerName,
                              std::string const& targetRequestId, CallbackType const& onFinish = nullptr,
                              int priority = PRIORITY_NORMAL, bool keepTracking = true,
                              std::string const& jobId = "", unsigned int requestExpirationIvalSec = 0);

    StatusRequest() = delete;
    StatusRequest(StatusRequest const&) = delete;
    StatusRequest& operator=(StatusRequest const&) = delete;

    ~StatusRequest() final = default;

    std::string const& targetRequestId() const { return _targetRequestId; }

    // Status info on the target request (as reported by the worker) is only available after
    // the status request is successfully completed. Otherwise std::logic_error is thrown.

    ProtocolStatus targetRequestStatus() const;
    ProtocolStatusExt targetRequestExtendedStatus() const;
    Performance const& targetRequestPerformance() const;

    /// @see Request::toString()
    std::string toString(bool extended = false) const override;

protected:
    void startImpl(replica::Lock const& lock) final;
    void notify(replica::Lock const& lock) final;
    void awaken(boost::system::error_code const& ec) final;

private:
    StatusRequest(std::shared_ptr<Controller> const& controller, std::string const& workerName,
                  std::string const& targetRequestId, CallbackType const& onFinish, int priority,
                  bool keepTracking);
    /**
     * Send the serialized content of the buffer to a worker
     * @param lock a lock on Request::_mtx must be acquired before calling
     *   this method
     */
    void _send(replica::Lock const& lock);

    /**
     * Process the worker response to the requested operation.
     * @param success 'true' indicates a successful response from a worker
     * @param message worker response (if success)
     */
    void _analyze(bool success, ProtocolResponseStatus message);

    /// Ensure the request is in a successful state.
    /// @throw std::logic_error if the request is not in a successful state.
    void _assertIsSucceeded() const;

    // Input parameters
    std::string const _targetRequestId;
    CallbackType _onFinish;

    // Status of of the target request
    ProtocolStatus _targetStatus;
    ProtocolStatusExt _targetExtendedServerStatus;
    Performance _targetPerformance;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_STATUSREQUEST_H
