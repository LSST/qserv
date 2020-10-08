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
#ifndef LSST_QSERV_HTTPMODULE_H
#define LSST_QSERV_HTTPMODULE_H

// System headers
#include <memory>
#include <string>

// Qserv headers
#include "qhttp/Request.h"
#include "qhttp/Response.h"
#include "replica/EventLogger.h"
#include "replica/HttpModuleBase.h"
#include "replica/HttpProcessorConfig.h"

// Forward declarations
namespace lsst {
namespace qserv {
namespace replica {
namespace database {
namespace mysql {
    class Connection;
}}}}} // Forward declarations

// This header declarations
namespace lsst {
namespace qserv {
namespace replica {

/**
 * Class HttpModule is a base class for requests processing modules
 * of an HTTP server built into the Master Replication Controller.
 */
class HttpModule: public EventLogger,
                  public HttpModuleBase {
public:
    HttpModule() = delete;
    HttpModule(HttpModule const&) = delete;
    HttpModule& operator=(HttpModule const&) = delete;

    virtual ~HttpModule() = default;

protected:
    /**
     * 
     * @param controller       The Controller provides the network I/O services (BOOST ASIO)
     * @param taskName         The name of a task in a context of the Master Replication Controller
     * @param processorConfig  Shared parameters of the HTTP services
     * @param req              The HTTP request
     * @param resp             The HTTP response channel
     */
    HttpModule(Controller::Ptr const& controller,
               std::string const& taskName,
               HttpProcessorConfig const& processorConfig,
               qhttp::Request::Ptr const& req,
               qhttp::Response::Ptr const& resp);

    unsigned int workerResponseTimeoutSec() const { return _processorConfig.workerResponseTimeoutSec; }
    unsigned int qservSyncTimeoutSec() const { return _processorConfig.qservSyncTimeoutSec; }
    unsigned int workerReconfigTimeoutSec() const { return _processorConfig.workerReconfigTimeoutSec; }

    /// @see HttpModuleBase::context()
    virtual std::string context() const final;

    /// @param database The name of a database to connect to.
    /// @return A connection object for the Qserv Master Database server.
    std::shared_ptr<database::mysql::Connection> qservMasterDbConnection(std::string const& database) const;

private:
    HttpProcessorConfig const _processorConfig;
};
    
}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_HTTPMODULE_H
