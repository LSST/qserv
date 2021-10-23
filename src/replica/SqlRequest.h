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
#ifndef LSST_QSERV_REPLICA_SQLREQUEST_H
#define LSST_QSERV_REPLICA_SQLREQUEST_H

// System headers
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <tuple>

// Qserv headers
#include "replica/Common.h"
#include "replica/RequestMessenger.h"
#include "replica/SqlResultSet.h"
#include "replica/protocol.pb.h"

// Forward declarations
namespace lsst {
namespace qserv {
namespace replica {
    class Messenger;
}}}  // Forward declarations

// This header declarations
namespace lsst {
namespace qserv {
namespace replica {

/**
 * Abstract class SqlRequest is a base class for a family of the Controller-side
 * requests launching queries against a MySQL/MariaDB database of Qserv workers
 * via the corresponding Replication workers. The requests are sent over
 * the controller-worker protocol and they are executed by the the worker-side
 * framework. 
 *
 * In case of a successful completion of a request an object of this request class
 * will receive a result set (if any) of the query.
 */
class SqlRequest: public RequestMessenger  {
public:
    typedef std::shared_ptr<SqlRequest> Ptr;

    SqlRequest() = delete;
    SqlRequest(SqlRequest const&) = delete;
    SqlRequest& operator=(SqlRequest const&) = delete;

    ~SqlRequest() override = default;

    uint64_t maxRows() const { return requestBody.max_rows(); }

    /// @return target request specific parameters
    SqlRequestParams const& targetRequestParams() const { return _targetRequestParams; }

    /**
     * @note This operation will return a sensible result only if the operation
     *   finishes with status FINISHED::SUCCESS
     * @return a reference to a result obtained from a remote service.
     */
    SqlResultSet const& responseData() const;

    /// @see Request::extendedPersistentState()
    std::list<std::pair<std::string,std::string>> extendedPersistentState() const final;

    /**
     * Make an extended print of the request which would include a result set.
     * The method will also make a call to Request::defaultPrinter().
     * @param ptr  an object to be printed
     */
    static void extendedPrinter(Ptr const& ptr);

protected:
    /**
     * Create a new request with specified parameters.
     * @param serviceProvider is needed to access the Configuration and the Controller
     *   for communicating with the worker.
     * @param io_service  a communication end-point
     * @param worker identifier of a worker node
     * @param maxRows (optional) limit for the maximum number of rows to be returned with the request.
     *   Leaving the default value of the parameter to 0 will result in not imposing any
     *   explicit restrictions on a size of the result set. Note that other, resource-defined
     *   restrictions will still apply. The later includes the maximum size of the Google Protobuf
     *   objects, the amount of available memory, etc.
     * @param priority priority level of the request
     * @param keepTracking keep tracking the request before it finishes or fails
     * @param messenger interface for communicating with workers
     */
    SqlRequest(ServiceProvider::Ptr const& serviceProvider,
               boost::asio::io_service& io_service,
               std::string const& requestName,
               std::string const& worker,
               uint64_t maxRows,
               int priority,
               bool keepTracking,
               std::shared_ptr<Messenger> const& messenger);

    /// @see Request::startImpl()
    void startImpl(util::Lock const& lock) final;

    /// @see Request::savePersistentState()
    void savePersistentState(util::Lock const& lock) final;

    /// @see Request::awaken()
    void awaken(boost::system::error_code const& ec) final;

    /**
     * Request body to be sent to the worker. The content of the request is partially
     * set by this class's constructor, and it's fully initialized by the constructor
     * of a subclass depending on a type of request.
     */
    ProtocolRequestSql requestBody;

private:
    /**
     * Send the serialized content of the buffer to a worker
     * @param lock a lock on Request::_mtx must be acquired before calling this method
     */
    void _send(util::Lock const& lock);

    /**
     * Analyze the completion status of the requested operation
     * @param success 'true' indicates a successful response from a worker
     * @param response response from a worker (if success)
     */
    void _analyze(bool success, ProtocolResponseSql const& response);

    /// Request-specific parameters of the target request
    SqlRequestParams _targetRequestParams;

    /// The results reported by a worker service
    SqlResultSet _responseData;
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_REPLICA_SQLREQUEST_H
