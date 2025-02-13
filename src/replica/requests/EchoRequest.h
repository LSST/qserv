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
#ifndef LSST_QSERV_REPLICA_ECHOREQUEST_H
#define LSST_QSERV_REPLICA_ECHOREQUEST_H

// System headers
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
 * Class EchoRequest represents Controller-side requests for testing
 * the controller-worker protocol and the worker-side framework.
 * These requests have no side effects.
 */
class EchoRequest : public Request {
public:
    typedef std::shared_ptr<EchoRequest> Ptr;

    /// The function type for notifications on the completion of the request
    typedef std::function<void(Ptr)> CallbackType;

    EchoRequest() = delete;
    EchoRequest(EchoRequest const&) = delete;
    EchoRequest& operator=(EchoRequest const&) = delete;

    ~EchoRequest() final = default;

    std::string const& data() const { return _data; }
    uint64_t delay() const { return _delay; }

    /// @return target request specific parameters
    EchoRequestParams const& targetRequestParams() const { return _targetRequestParams; }

    /**
     * @note This operation will return a sensible result only if the operation
     *   finishes with status FINISHED::SUCCESS
     * @return a reference to a result obtained from a remote service.
     */
    std::string const& responseData() const;

    /**
     * Create a new request with specified parameters.
     *
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * Class-specific parameters are documented below:
     * @param data The data string to be echoed back by a worker.
     * @param delay The execution time (milliseconds) of the request at worker.
     *
     * @see The very base class Request for the description of the common parameters
     *   of all subclasses.
     *
     * @return A pointer to the created object.
     */
    static Ptr createAndStart(std::shared_ptr<Controller> const& controller, std::string const& workerName,
                              std::string const& data, uint64_t delay, CallbackType const& onFinish = nullptr,
                              int priority = PRIORITY_NORMAL, bool keepTracking = true,
                              std::string const& jobId = "", unsigned int requestExpirationIvalSec = 0);

    /// @see Request::extendedPersistentState()
    std::list<std::pair<std::string, std::string>> extendedPersistentState() const override;

protected:
    void startImpl(replica::Lock const& lock) final;
    void notify(replica::Lock const& lock) final;
    void savePersistentState(replica::Lock const& lock) final;
    void awaken(boost::system::error_code const& ec) final;

private:
    EchoRequest(std::shared_ptr<Controller> const& controller, std::string const& workerName,
                std::string const& data, uint64_t delay, CallbackType const& onFinish, int priority,
                bool keepTracking);

    /**
     * Send the serialized content of the buffer to a worker
     * @param lock a lock on Request::_mtx must be acquired before calling this method
     */
    void _send(replica::Lock const& lock);

    /**
     * Process the completion of the requested operation
     * @param success 'true' indicates a successful response from a worker
     * @param message response from a worker (if success)
     */
    void _analyze(bool success, ProtocolResponseEcho const& message);

    // Input parameters

    std::string const _data;
    uint64_t const _delay;
    CallbackType _onFinish;  ///< @note is reset when the request finishes

    /// Request-specific parameters of the target request
    EchoRequestParams _targetRequestParams;

    /// The results reported by a worker service
    std::string _responseData;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_ECHOREQUEST_H
