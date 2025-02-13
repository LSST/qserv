// -*- LSST-C++ -*-
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
#ifndef LSST_QSERV_REPLICA_WORKERDELETEREPLICAHTTPREQUEST_H
#define LSST_QSERV_REPLICA_WORKERDELETEREPLICAHTTPREQUEST_H

// System headers
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/config/ConfigDatabase.h"
#include "replica/util/ReplicaInfo.h"
#include "replica/worker/WorkerHttpRequest.h"

// Forward declarations
namespace lsst::qserv::replica {
class ServiceProvider;
}  // namespace lsst::qserv::replica

namespace lsst::qserv::replica::protocol {
struct QueuedRequestHdr;
}  // namespace lsst::qserv::replica::protocol

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerDeleteReplicaHttpRequest represents a context and a state of replica deletion
 * requests within the worker servers.
 */
class WorkerDeleteReplicaHttpRequest : public WorkerHttpRequest {
public:
    /**
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * @param serviceProvider provider is needed to access the Configuration
     *   of a setup and for validating the input parameters
     * @param worker the name of a worker. The name must match the worker which
     *   is going to execute the request.
     * @param hdr request header (common parameters of the queued request)
     * @param req the request object received from a client (request-specific parameters)
     * @param onExpired request expiration callback function
     * @return pointer to the created object
     */
    static std::shared_ptr<WorkerDeleteReplicaHttpRequest> create(
            std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker,
            protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req,
            ExpirationCallbackType const& onExpired);

    WorkerDeleteReplicaHttpRequest() = delete;
    WorkerDeleteReplicaHttpRequest(WorkerDeleteReplicaHttpRequest const&) = delete;
    WorkerDeleteReplicaHttpRequest& operator=(WorkerDeleteReplicaHttpRequest const&) = delete;

    ~WorkerDeleteReplicaHttpRequest() override = default;

    bool execute() override;

protected:
    void getResult(nlohmann::json& result) const override;

private:
    WorkerDeleteReplicaHttpRequest(std::shared_ptr<ServiceProvider> const& serviceProvider,
                                   std::string const& worker, protocol::QueuedRequestHdr const& hdr,
                                   nlohmann::json const& req, ExpirationCallbackType const& onExpired);

    // Input parameters
    DatabaseInfo const _databaseInfo;  ///< Database descriptor obtained from the Configuration
    unsigned int _chunk;

    /// Extended status of the replica deletion request
    ReplicaInfo _replicaInfo;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERDELETEREPLICAHTTPREQUEST_H
