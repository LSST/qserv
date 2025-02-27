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

// Class header
#include "replica/worker/WorkerServer.h"

// System headers
#include <functional>
#include <thread>
#include <vector>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/services/ServiceProvider.h"
#include "replica/worker/WorkerProcessor.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerServer");

}  // namespace

namespace lsst::qserv::replica {

WorkerServer::Ptr WorkerServer::create(ServiceProvider::Ptr const& serviceProvider,
                                       string const& workerName) {
    return WorkerServer::Ptr(new WorkerServer(serviceProvider, workerName));
}

WorkerServer::WorkerServer(ServiceProvider::Ptr const& serviceProvider, string const& workerName)
        : _serviceProvider(serviceProvider),
          _workerName(workerName),
          _processor(WorkerProcessor::create(serviceProvider, workerName)),
          _acceptor(_io_service, boost::asio::ip::tcp::endpoint(
                                         boost::asio::ip::tcp::v4(),
                                         serviceProvider->config()->get<uint16_t>("worker", "svc-port"))) {
    // Set the socket reuse option to allow recycling ports after catastrophic
    // failures.
    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));
}

void WorkerServer::run() {
    // Start the processor to allow processing requests.
    _processor->run();

    // Begin accepting connections before running the service to allow
    // asynchronous operations. Otherwise the service will finish right
    // away.
    _beginAccept();

    // Launch the pool of the BOOST ASIO threads
    vector<shared_ptr<thread>> threads(_serviceProvider->config()->get<size_t>("worker", "num-threads"));
    for (auto&& ptr : threads) {
        ptr = shared_ptr<thread>(new thread([&]() { _io_service.run(); }));
    }

    // Wait for all threads in the pool to exit.
    for (auto&& ptr : threads) {
        ptr->join();
    }
}

void WorkerServer::_beginAccept() {
    auto const connection = WorkerServerConnection::create(_serviceProvider, _processor, _io_service);

    _acceptor.async_accept(connection->socket(),
                           bind(&WorkerServer::_handleAccept, shared_from_this(), connection, _1));
}

void WorkerServer::_handleAccept(WorkerServerConnection::Ptr const& connection,
                                 boost::system::error_code const& ec) {
    if (ec.value() == 0) {
        connection->beginProtocol();
    } else {
        // TODO: Consider throwing an exception instead. Another option
        //       would be to log the message via the standard log file
        //       mechanism since it's safe to ignore problems with
        //       incoming connections due a lack of side effects.
        LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  ec:" << ec);
    }
    _beginAccept();
}

}  // namespace lsst::qserv::replica
