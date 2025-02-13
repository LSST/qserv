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
#ifndef LSST_QSERV_REPLICA_WORKERHTTPSVC_H
#define LSST_QSERV_REPLICA_WORKERHTTPSVC_H

// System headers
#include <memory>
#include <string>

// Qserv headers
#include "replica/util/ChttpSvc.h"

// Forward declarations
namespace lsst::qserv::replica {
class ServiceProvider;
class WorkerHttpProcessor;
}  // namespace lsst::qserv::replica

namespace httplib {
class Server;
}  // namespace httplib

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerHttpSvc is the HTTP frontend to the Replication Worker Service.
 * Each instance of this class will be running in its own thread.
 */
class WorkerHttpSvc : public ChttpSvc {
public:
    /**
     * Create an instance of the service.
     *
     * @param serviceProvider For configuration, etc. services.
     * @param workerName The name of a worker this service is acting upon (used for
     *   checking consistency of the protocol).
     * @return A pointer to the created object.
     */
    static std::shared_ptr<WorkerHttpSvc> create(std::shared_ptr<ServiceProvider> const& serviceProvider,
                                                 std::string const& workerName);

    WorkerHttpSvc() = delete;
    WorkerHttpSvc(WorkerHttpSvc const&) = delete;
    WorkerHttpSvc& operator=(WorkerHttpSvc const&) = delete;

    virtual ~WorkerHttpSvc() = default;

protected:
    /// @see HttpSvc::registerServices()
    virtual void registerServices(std::unique_ptr<httplib::Server> const& server) override;

private:
    /// @see WorkerHttpSvc::create()
    WorkerHttpSvc(std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& workerName);

    // Input parameters
    std::string const _workerName;

    /// The request processor.
    std::shared_ptr<WorkerHttpProcessor> _processor;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERHTTPSVC_H
