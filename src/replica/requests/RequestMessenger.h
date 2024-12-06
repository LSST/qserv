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
#ifndef LSST_QSERV_REPLICA_REQUESTMESSENGER_H
#define LSST_QSERV_REPLICA_REQUESTMESSENGER_H

// System headers
#include <functional>
#include <memory>
#include <string>

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "replica/requests/Request.h"
#include "replica/util/Common.h"
#include "replica/util/Mutex.h"

// Forward declarations
namespace lsst::qserv::replica {
class Controller;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class RequestMessenger is a base class for a family of requests within
 * the replication Controller server.
 */
class RequestMessenger : public Request {
public:
    /// The pointer type for instances of the class
    typedef std::shared_ptr<RequestMessenger> Ptr;

    /// The callaback type for notifications on completion of the request
    /// disposal operation. The first parameter (std::string const&) of the callback
    /// is the unique identifier of a request, the second parameter (bool) is a flag
    /// indicating a success or a failure of the operation, and the last parameter
    /// (ProtocolResponseDispose const&) represents a result of the operation reported
    /// by the worker service.
    typedef std::function<void(std::string const&, bool, ProtocolResponseDispose const&)>
            OnDisposeCallbackType;

    RequestMessenger() = delete;
    RequestMessenger(RequestMessenger const&) = delete;
    RequestMessenger& operator=(RequestMessenger const&) = delete;

    ~RequestMessenger() override = default;

protected:
    /**
     * Construct the request with the pointer to the services provider.
     * @return A pointer to the created object.
     */
    RequestMessenger(std::shared_ptr<Controller> const& controller, std::string const& type,
                     std::string const& workerName, int priority, bool keepTracking, bool disposeRequired);

    /// @see Request::finishImpl()
    void finishImpl(replica::Lock const& lock) override;

    /**
     * Initiate the request disposal at the worker server. This method is automatically
     * called upon succesfull completion of requests for which the flag 'disposeRequired'
     * was set during request object construction. However, the streaming requests
     * that are designed to make more than one trip to the worker under the same request
     * identifier may also explicitly call this method upon completing intermediate
     * requests. That is normally done to expedite the garbage collection of the worker
     * requests and prevent excessive memory build up (or keeping other resources)
     * at the worker.
     * @param lock The lock on Request::_mtx must be acquired before calling this method.
     * @param priority The desired priority level of the operation.
     * @param onFinish The optional callback to be called upon the completion of
     *  the request disposal operation.
     */
    void dispose(replica::Lock const& lock, int priority, OnDisposeCallbackType const& onFinish = nullptr);
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_REQUESTMESSENGER_H
