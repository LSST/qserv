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
#ifndef LSST_QSERV_WORKERHTTPSVCMOD_H
#define LSST_QSERV_WORKERHTTPSVCMOD_H

// System headers
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "http/ChttpModule.h"

// Forward declarations

namespace lsst::qserv::replica {
class ServiceProvider;
class WorkerHttpProcessor;
}  // namespace lsst::qserv::replica

namespace lsst::qserv::replica::protocol {
struct QueuedRequestHdr;
}  // namespace lsst::qserv::replica::protocol

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerHttpSvcMod processes the Replication Controller's requests.
 * The class is used by the HTTP server built into the worker Replication service.
 */
class WorkerHttpSvcMod : public http::ChttpModule {
public:
    WorkerHttpSvcMod() = delete;
    WorkerHttpSvcMod(WorkerHttpSvcMod const&) = delete;
    WorkerHttpSvcMod& operator=(WorkerHttpSvcMod const&) = delete;

    virtual ~WorkerHttpSvcMod() = default;

    /**
     * Process a request.
     *
     * Supported values for parameter 'subModuleName':
     *
     *  ECHO                for testing the worker-side framework
     *  REPLICA-CREATE      for creating a replica of a chunk
     *  REPLICA-DELETE      for deleting an existing replica of a chunk
     *  REPLICA-FIND        for finding out if a replica is present, and reporting its state
     *  REPLICA-FIND-ALL    for finding all replicas and reporting their states
     *  INDEX               for extracting and returning a collection of the "director" index data
     *  SQL                 for executing various SQL statements against the worker's database
     *  REQUEST-TRACK       for tracking status and retreiving results of the previously submitted request
     *  REQUEST-STATUS      for checking the status of the previously submitted request
     *  REQUEST-STOP        for stopping the previously submitted request
     *  REQUEST-DISPOSE     for garbage collecting the request
     *  SERVICE-STATUS      for checking the status of the worker replication service
     *  SERVICE-SUSPEND     for suspending the worker replication service
     *  SERVICE-RESUME      for resuming the worker replication service
     *  SERVICE-REQUESTS    for listing the outstanding requests
     *  SERVICE-DRAIN       for draining the worker replication service
     *  SERVICE-RECONFIG    for reconfiguring the worker replication service
     *
     * @param serviceProvider The provider of services is needed to access
     *   the configuration and the database services.
     * @param workerName The name of a worker this service is acting upon (used to pull
     *   worker-specific configuration options for the service).
     * @param processor Request processor.
     * @param req The HTTP request.
     * @param resp The HTTP response channel.
     * @param subModuleName The name of a submodule to be called.
     * @param authType The authorization requirements for the module
     * @throws std::invalid_argument for unknown values of parameter 'subModuleName'
     */
    static void process(std::shared_ptr<ServiceProvider> const& serviceProvider,
                        std::shared_ptr<WorkerHttpProcessor> const& processor, std::string const& workerName,
                        httplib::Request const& req, httplib::Response& resp,
                        std::string const& subModuleName,
                        http::AuthType const authType = http::AuthType::NONE);

protected:
    virtual std::string context() const final;
    virtual nlohmann::json executeImpl(std::string const& subModuleName) final;

private:
    WorkerHttpSvcMod(std::shared_ptr<ServiceProvider> const& serviceProvider,
                     std::shared_ptr<WorkerHttpProcessor> const& processor, std::string const& workerName,
                     httplib::Request const& req, httplib::Response& resp);

    /// Parse common parameters of the queued requests
    /// @param func The name of the function to be used in the log messages
    /// @return The parsed header
    protocol::QueuedRequestHdr _parseHdr(std::string const& func) const;

    /// Process the ECHO request
    nlohmann::json _echo() const;

    /// Process the REPLICA-CREATE request
    nlohmann::json _replicaCreate();

    /// Process the REPLICA-DELETE request
    nlohmann::json _replicaDelete();

    /// Process the REPLICA-FIND request
    nlohmann::json _replicaFind();

    /// Process the REPLICA-FIND-ALL request
    nlohmann::json _replicaFindAll();

    /// Process the INDEX request
    nlohmann::json _index();

    /// Process the SQL request
    nlohmann::json _sql();

    /// Process the REQUEST-TRACK request
    nlohmann::json _requestTrack();

    /// Process the REQUEST-STATUS request
    nlohmann::json _requestStatus();

    /// Process the REQUEST-STOP request
    nlohmann::json _requestStop();

    /// Process the REQUEST-DISPOSE request
    nlohmann::json _requestDispose();

    /// Process the SERVICE-SUSPEND request
    nlohmann::json _serviceSuspend();

    /// Process the SERVICE-RESUME request
    nlohmann::json _serviceResume();

    /// Process the SERVICE-STATUS request
    nlohmann::json _serviceStatus();

    /// Process the SERVICE-REQUESTS request
    nlohmann::json _serviceRequests();

    /// Process the SERVICE-DRAIN request
    nlohmann::json _serviceDrain();

    /// Process the SERVICE-RECONFIG request
    nlohmann::json _serviceReconfig();

    // Input parameters
    std::shared_ptr<ServiceProvider> const _serviceProvider;
    std::shared_ptr<WorkerHttpProcessor> _processor;
    std::string const _workerName;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_WORKERHTTPSVCMOD_H
