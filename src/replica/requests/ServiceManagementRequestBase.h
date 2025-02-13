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
#ifndef LSST_QSERV_REPLICA_SERVICEMANAGEMENTREQUESTBASE_H
#define LSST_QSERV_REPLICA_SERVICEMANAGEMENTREQUESTBASE_H

// System headers
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "replica/requests/Request.h"
#include "replica/util/Common.h"

// Forward declarations
namespace lsst::qserv::replica {
class Controller;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Structure ServiceState encapsulates various parameters representing the state
 * of the remote request processing service. The parameters are available
 * upon the completion of the request.
 */
struct ServiceState {
    // Its state
    enum State { SUSPEND_IN_PROGRESS = 0, SUSPENDED = 1, RUNNING = 2 };
    State state;

    /// When the service started (milliseconds since UNIX Epoch)
    uint64_t startTime;

    // Counters for requests known to the service since its last start

    uint32_t numNewRequests;
    uint32_t numInProgressRequests;
    uint32_t numFinishedRequests;

    std::vector<ProtocolServiceResponseInfo> newRequests;
    std::vector<ProtocolServiceResponseInfo> inProgressRequests;
    std::vector<ProtocolServiceResponseInfo> finishedRequests;

    /// @return string representation of the state
    std::string state2string() const;

    /// Set parameter values from a protobuf object
    void set(const ProtocolServiceResponse& message);
};

/// Overloaded streaming operator for type ServiceState
std::ostream& operator<<(std::ostream& os, const ServiceState& ss);

/**
 * Class ServiceManagementRequestBase is the base class for a family of requests
 * managing worker-side replication service. The only variable parameter of this
 * class is a specific type of the management request.
 *
 * @note  That this class can't be instantiated directly. It serves as an implementation
 *   of the protocol. All final customizations and type-specific operations are
 *   provided via a generic subclass.
 */
class ServiceManagementRequestBase : public Request {
public:
    typedef std::shared_ptr<ServiceManagementRequestBase> Ptr;

    ServiceManagementRequestBase() = delete;
    ServiceManagementRequestBase(ServiceManagementRequestBase const&) = delete;
    ServiceManagementRequestBase& operator=(ServiceManagementRequestBase const&) = delete;

    ~ServiceManagementRequestBase() override = default;

    /**
     * @throw std::logic_error If the request's primary state is not 'FINISHED' and its
     *   extended state is neither 'SUCCESS' or 'SERVER_ERROR'.
     * @return  The state of the worker-side service.
     */
    ServiceState const& getServiceState() const;

    /**
     * Make an extended print of the request which would include a result set.
     * The method will also make a call to Request::defaultPrinter().
     * @param ptr  An object to be printed.
     */
    static void extendedPrinter(Ptr const& ptr);

protected:
    /**
     * Construct the request with the pointer to the services provider.
     * @param controller the Controller associated with the request
     * @param requestName The name of a request.
     * @param workerName The name of a worker.
     * @param requestType A type of a request.
     * @param priority A priority level of a request.
     */
    ServiceManagementRequestBase(std::shared_ptr<Controller> const& controller, char const* requestName,
                                 std::string const& workerName, ProtocolServiceRequestType requestType,
                                 int priority);

    void startImpl(replica::Lock const& lock) final;
    void savePersistentState(replica::Lock const& lock) final;

private:
    /**
     * Process the worker response to the requested operation.
     * @param success 'true' indicates a successful response from a worker.
     * @param message  A response message from the worker service (if success is 'true').
     */
    void _analyze(bool success, ProtocolServiceResponse const& message);

    /// Request type
    ProtocolServiceRequestType const _requestType;

    /// Detailed status of the worker-side service obtained upon completion of
    /// the management request.
    ServiceState _serviceState;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_SERVICEMANAGEMENTREQUESTBASE_H
