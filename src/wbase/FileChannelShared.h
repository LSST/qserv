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

#ifndef LSST_QSERV_WBASE_FILECHANNELSHARED_H
#define LSST_QSERV_WBASE_FILECHANNELSHARED_H

// System headers
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

// Third-party headers
#include <mysql/mysql.h>
#include "nlohmann/json.hpp"

// Qserv headers
#include "global/intTypes.h"
#include "qmeta/types.h"
#include "wbase/SendChannel.h"

// Forward declarations

namespace google::protobuf {
class Arena;
}  // namespace google::protobuf

namespace lsst::qserv::proto {
class ResponseData;
}  // namespace lsst::qserv::proto

namespace lsst::qserv::wbase {
class Task;
}  // namespace lsst::qserv::wbase

namespace lsst::qserv::util {
class MultiError;
}  // namespace lsst::qserv::util

namespace lsst::qserv::wbase {

/// The class is responsible for writing mysql result rows as Protobuf
/// serialized messages into an output file. Once a task (or all sub-chunk
/// tasks) finished writing data a short reply message is sent back to Czar using
/// SSI request's SendChannel that was provided to the factory method
/// of the class. Error messages would be also sent via te same channel.
/// A partially written file will be automatically deleted in case of
/// errors.
///
/// When building messages for result rows, multiple tasks may add to the
/// the output file before it gets closed and a reply is transmitted to the czar.
/// All the tasks adding rows to the file must be operating on
/// the same chunk. This only happens for near-neighbor queries, which
/// have one task per subchunk.
class FileChannelShared {
public:
    using Ptr = std::shared_ptr<FileChannelShared>;

    static std::atomic<uint64_t> scsSeqId;  ///< Source for unique _scsId numbers

    /**
     * This method gets called upon receiving a notification from Czar about
     * the Czar service restart. The method will clean result files corresponding
     * to the older (including the specified one) queries.
     * @note The method may be called 0 or many times during the lifetime of the worker service.
     * @note The Czar identifier is required to avoid possble interferences in the multi-Czar
     *   deployments where different instances of Czars may restart independently.
     * @param czarId The unique identifier of Czar that initiated the query.
     * @param queryId The most recent user query registered before restart.
     */
    static void cleanUpResultsOnCzarRestart(uint32_t czarId, QueryId queryId);

    /**
     * This method gets called exactly one time during the initial startup
     * initialization of the worker service.
     */
    static void cleanUpResultsOnWorkerRestart();

    /**
     * Clean up result files of the specified query.
     * @param czarId The unique identifier of Czar that initiated the query.
     * @param queryId The most recent user query registered before restart.
     */
    static void cleanUpResults(uint32_t czarId, QueryId queryId);

    /// @return Status and statistics on the results folder (capacity, usage, etc.)
    static nlohmann::json statusToJson();

    /**
     * Locate existing result files.
     * @param queryIds The optional selector for queries. If the collection is empty
     *   then all queries will be considered.
     * @param maxFiles The optional limit for maximum number of files to be reported.
     *   If 0 then no limit is set.
     * @return A collection of the results files matching the optional filter.
     */
    static nlohmann::json filesToJson(std::vector<QueryId> const& queryIds, unsigned int maxFiles);

    /// The factory method for the channel class.
    static Ptr create(std::shared_ptr<wbase::SendChannel> const& sendChannel, qmeta::CzarId czarId,
                      std::string const& workerId = std::string());

    FileChannelShared() = delete;
    FileChannelShared(FileChannelShared const&) = delete;
    FileChannelShared& operator=(FileChannelShared const&) = delete;

    /// Non-trivial d-tor is needed to close the channel and to garbage collect
    /// the file after failures.
    ~FileChannelShared();

    /// Set the number of Tasks that will be sent using this wbase::SendChannel.
    /// This should not be changed once set.
    void setTaskCount(int taskCount);
    int getTaskCount() const { return _taskCount; }

    /// @return true if this is the last task to call this
    bool transmitTaskLast();

    /// Return a normalized id string.
    static std::string makeIdStr(int qId, int jId);

    /// @return the sendChannelShared sequence number, which is always valid.
    uint64_t getScsId() const { return _scsId; }

    /// @return the current sql connection count
    int getSqlConnectionCount() { return _sqlConnectionCount; }

    /// @return the sql connection count after incrementing by 1.
    int incrSqlConnectionCount() { return ++_sqlConnectionCount; }

    /// @return true if this is the first time this function has been called.
    bool getFirstChannelSqlConn() { return _firstChannelSqlConn.exchange(false); }

    /// @return a transmit data object indicating the errors in 'multiErr'.
    bool buildAndTransmitError(util::MultiError& multiErr, std::shared_ptr<Task> const& task, bool cancelled);

    /// Extract the SQL results and write them into the file and notify Czar after the last
    /// row of the result result set depending on theis channel has been processed.
    /// @return true if there was an error.
    bool buildAndTransmitResult(MYSQL_RES* mResult, std::shared_ptr<Task> const& task,
                                util::MultiError& multiErr, std::atomic<bool>& cancelled);

    /// @see wbase::SendChannel::kill
    bool kill(std::string const& note);

    /// @see wbase::SendChannel::isDead
    bool isDead();

private:
    /// Private constructor to protect shared pointer integrity.
    FileChannelShared(std::shared_ptr<wbase::SendChannel> const& sendChannel, qmeta::CzarId czarId,
                      std::string const& workerId);

    /// @see wbase::SendChannel::kill
    /// @param streamMutexLock - Lock on mutex _streamMutex to be acquired before calling the method.
    bool _kill(std::lock_guard<std::mutex> const& streamMutexLock, std::string const& note);

    /**
     * Transfer rows of the result set into into the output file.
     * @note The file will be created at the first call to the method.
     * @note The method may not extract all rows if the amount of data found
     *   in the result set exceeded the maximum size allowed by the Google Protobuf
     *   implementation. Also, the iterative approach to the data extraction allows
     *   the driving code to be interrupted should the corresponding query be cancelled
     *   during the lengthy data processing phase.
     * @param responseData - proto buffer to hold the response being constructed.
     * @param protobufArena - proto buffer memory management control.
     * @param task - a task that produced the result set
     * @param mResult - MySQL result to be used as a source
     * @param bytes - the number of bytes in the result message recorded into the file
     * @param rows - the number of rows extracted from th eresult set
     * @param multiErr - a collector of any errors that were captured during result set processing
     * @return 'true' if the result set still has more rows to be extracted.
     * @throws std::runtime_error for problems encountered when attemting to create the file
     *   or write into the file.
     */
    bool _writeToFile(proto::ResponseData* responseData,
                      std::unique_ptr<google::protobuf::Arena> const& protobufArena,
                      std::shared_ptr<Task> const& task, MYSQL_RES* mResult, int& bytes, int& rows,
                      util::MultiError& multiErr);

    /**
     * Extract as many rows as allowed by the Google Protobuf implementation from
     * from the input result set into the output result object.
     * @param responseData - proto buffer to hold the response being constructed.
     * @param protobufArena - proto buffer memory management control.
     * @param mResult - MySQL result to be used as a source
     * @param rows - the number of rows extracted from the result set
     * @param tSize - the approximate amount of data extracted from the result set
     * @return 'true' if there are more rows left in the result set.
     */
    static bool _fillRows(proto::ResponseData* responseData, MYSQL_RES* mResult, int& rows, size_t& tSize);

    /**
     * Unconditionaly close and remove (potentially - the partially written) file.
     * This method gets called in case of any failure detected while processing
     * a query, sending a response back to Czar, or in case of a query cancellation.
     * @note For succesfully completed requests the files are deleted remotely
     *   upon special requests made explicitly by Czar after uploading and consuming
     *   result sets. Unclaimed files that might be still remaining at the results
     *   folder would need to be garbage collected at the startup time of the worker.
     * @param tMtxLock - a lock on the mutex tMtx
     */
    void _removeFile(std::lock_guard<std::mutex> const& tMtxLock);

    /**
     * Send the summary message to Czar upon complation or failure of a request.
     * @param tMtxLock - a lock on the mutex tMtx
     * @param task - a task that produced the result set
     * @param cancelled - request cancellaton flag (if any)
     * @param multiErr - a collector of any errors that were captured during result set processing
     * @return 'true' if the operation was successfull
     */
    bool _sendResponse(std::lock_guard<std::mutex> const& tMtxLock,
                       std::unique_ptr<google::protobuf::Arena> const& protobufArena,
                       std::shared_ptr<Task> const& task, bool cancelled, util::MultiError const& multiErr);

    mutable std::mutex _tMtx;  ///< Protects data recording and Czar notification

    std::shared_ptr<wbase::SendChannel> const _sendChannel;  ///< Used to pass encoded information to XrdSsi.
    qmeta::CzarId const _czarId;                             ///< id of the czar that requested this task(s).
    std::string const _workerId;                             ///< The unique identifier of the worker.

    uint64_t const _scsId;  ///< id number for this FileChannelShared

    /// streamMutex is used to protect _lastCount and messages that are sent
    /// using FileChannelShared.
    std::mutex _streamMutex;

    // Metadata and response buffers should persist for the lifetime of the stream.
    std::string _metadataBuf;
    std::string _responseBuf;

    int _taskCount = 0;  ///< The number of tasks to be sent over this wbase::SendChannel.
    int _lastCount = 0;  ///< The number of 'last' buffers received.

    /// The number of sql connections opened to handle the Tasks using this FileChannelShared.
    /// Once this is greater than 0, this object needs free access to sql connections to avoid
    // system deadlock. @see SqlConnMgr::_take() and SqlConnMgr::_release().
    std::atomic<int> _sqlConnectionCount{0};

    /// true until getFirstChannelSqlConn() is called.
    std::atomic<bool> _firstChannelSqlConn{true};

    /// The mutex is locked by the following static methods which require exclusive
    /// access to the results folder: create(), cleanUpResultsOnCzarRestart(),
    /// cleanUpResultsOnWorkerRestart(), and cleanUpResults().
    static std::mutex _resultsDirCleanupMtx;

    std::string _fileName;  ///< The name is set when opening the file
    std::fstream _file;

    // Counters reported to Czar in the only ("summary") message sent upon the completion
    // of all tasks of a query.

    uint32_t _rowcount = 0;      ///< The total numnber of rows in all result sets of a query.
    uint64_t _transmitsize = 0;  ///< The total amount of data (bytes) in all result sets of a query.

    /// This should be set to true if there were any issues that invalidate the file, such as errors
    /// or cancellation.
    std::atomic<bool> _issueRequiresFileRemoval{false};
};

}  // namespace lsst::qserv::wbase

#endif  // LSST_QSERV_WBASE_FILECHANNELSHARED_H
