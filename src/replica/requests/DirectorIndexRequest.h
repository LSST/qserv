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
#ifndef LSST_QSERV_REPLICA_DIRECTORINDEXREQUEST_H
#define LSST_QSERV_REPLICA_DIRECTORINDEXREQUEST_H

// System headers
#include <fstream>
#include <functional>
#include <memory>
#include <ostream>
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
 *  Structure DirectorIndexRequestInfo represents a result of the requests.
 */
struct DirectorIndexRequestInfo {
    std::string error;         ///< A error (if any) that reported by the worker server.
    std::string fileName;      ///< The file that containes the index data (if success).
    size_t fileSizeBytes = 0;  ///< The number of bytes that were written into the file.
};

std::ostream& operator<<(std::ostream& os, DirectorIndexRequestInfo const& info);

/**
 * Class DirectorIndexRequest extracts and returns data to be loaded into
 * the "director" index.
 */
class DirectorIndexRequest : public Request {
public:
    typedef std::shared_ptr<DirectorIndexRequest> Ptr;

    /// The function type for notifications on the completion of the request
    typedef std::function<void(Ptr)> CallbackType;

    DirectorIndexRequest() = delete;
    DirectorIndexRequest(DirectorIndexRequest const&) = delete;
    DirectorIndexRequest& operator=(DirectorIndexRequest const&) = delete;

    /// Non-trivial destructor is needed to delete the data file that is created
    /// upon successfull completion of the request.
    virtual ~DirectorIndexRequest() final;

    std::string const& database() const { return _database; }
    std::string const& directorTable() const { return _directorTable; }
    unsigned int chunk() const { return _chunk; }
    bool hasTransactions() const { return _hasTransactions; }
    TransactionId transactionId() const { return _transactionId; }

    /// @return target request specific parameters
    DirectorIndexRequestParams const& targetRequestParams() const { return _targetRequestParams; }

    /**
     * @note the method must be called on requests which are in the FINISHED
     *   state only. Otherwise the resulting structure may be in the undefined state.
     * @note the structure returned by this operation may carry a meaningful
     *   MySQL error code if the worker-side data extraction failed.
     * @return a reference to a result of the completed request
     */
    DirectorIndexRequestInfo const& responseData() const;

    /**
     * Create a new request with specified parameters.
     *
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * Class-specific parameters are documented below:
     * @param database The name of a database.
     * @param directorTable The name of the director table.
     * @param chunk The number of a chunk to be inspected.
     * @param hasTransactions If set to 'true' then the result will also include a column which
     *   stores a value of the corresponding super-transaction.
     * @param transactionId The (optional) identifier of a super-transaction. This parameter is used
     *   only if the above defined flag 'hasTransactions' is set.
     *
     * @see The very base class Request for the description of the common parameters
     *   of all subclasses.
     *
     * @return A pointer to the created object.
     */
    static Ptr createAndStart(std::shared_ptr<Controller> const& controller, std::string const& workerName,
                              std::string const& database, std::string const& directorTable,
                              unsigned int chunk, bool hasTransactions, TransactionId transactionId,
                              CallbackType const& onFinish = nullptr, int priority = PRIORITY_NORMAL,
                              bool keepTracking = true, std::string const& jobId = "",
                              unsigned int requestExpirationIvalSec = 0);

    std::list<std::pair<std::string, std::string>> extendedPersistentState() const final;

protected:
    void startImpl(replica::Lock const& lock) final;
    void notify(replica::Lock const& lock) final;
    void savePersistentState(replica::Lock const& lock) final;
    void awaken(boost::system::error_code const& ec) final;

private:
    DirectorIndexRequest(std::shared_ptr<Controller> const& controller, std::string const& workerName,
                         std::string const& database, std::string const& directorTable, unsigned int chunk,
                         bool hasTransactions, TransactionId transactionId, CallbackType const& onFinish,
                         int priority, bool keepTracking);

    /**
     * Send the initial request for pulling data from the server.
     * @param lock a lock on Request::_mtx must be acquired before calling this method
     */
    void _sendInitialRequest(replica::Lock const& lock);

    /**
     * Send the status inquery request to the server.
     * @param lock a lock on Request::_mtx must be acquired before calling this method
     */
    void _sendStatusRequest(replica::Lock const& lock);

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
    void _analyze(bool success, ProtocolResponseDirectorIndex const& message);

    /**
     * Process the completion of the request disposal operation.
     * @param success 'true' indicates a successful response from a worker
     * @param message response from a worker (if success)
     */
    void _disposed(bool success, ProtocolResponseDispose const& message);

    /**
     * Open the input file and write the data into the file.
     * @note The method may throw exceptions in case if any problems will be encountered
     *  with opening the output file or writing data into the file.
     * @param lock A lock on the mutex _mtx acquired before calling the method.
     * @param data The data to be writrten onto the file.
     */
    void _writeInfoFile(replica::Lock const& lock, std::string const& data);

    // Input parameters

    std::string const _database;
    std::string const _directorTable;
    unsigned int const _chunk;
    bool const _hasTransactions;
    TransactionId const _transactionId;
    CallbackType _onFinish;

    /// Request-specific parameters of the target request
    DirectorIndexRequestParams _targetRequestParams;

    /// Result of the operation
    DirectorIndexRequestInfo _responseData;

    /// The file open for writing data read from the input stream. The file is open
    /// at a time when the first batch of data is received. And it gets closed after
    /// writing the last batch of data or in case of any failure.
    std::ofstream _file;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_DIRECTORINDEXREQUEST_H
