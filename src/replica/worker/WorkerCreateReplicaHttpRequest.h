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
#ifndef LSST_QSERV_REPLICA_WORKERCREATEREPLICAHTTPREQUEST_H
#define LSST_QSERV_REPLICA_WORKERCREATEREPLICAHTTPREQUEST_H

// System headers
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>

// Third party headers
#include "boost/filesystem.hpp"
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/config/ConfigDatabase.h"
#include "replica/util/ReplicaInfo.h"
#include "replica/worker/WorkerHttpRequest.h"

// Forward declarations
namespace lsst::qserv::replica {
class FileClient;
class ServiceProvider;
}  // namespace lsst::qserv::replica

namespace lsst::qserv::replica::protocol {
struct QueuedRequestHdr;
}  // namespace lsst::qserv::replica::protocol

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerCreateReplicaHttpRequest represents a context and a state of replication
 * requests within the worker servers.
 */
class WorkerCreateReplicaHttpRequest : public WorkerHttpRequest {
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
    static std::shared_ptr<WorkerCreateReplicaHttpRequest> create(
            std::shared_ptr<ServiceProvider> const& serviceProvider, std::string const& worker,
            protocol::QueuedRequestHdr const& hdr, nlohmann::json const& req,
            ExpirationCallbackType const& onExpired);

    WorkerCreateReplicaHttpRequest() = delete;
    WorkerCreateReplicaHttpRequest(WorkerCreateReplicaHttpRequest const&) = delete;
    WorkerCreateReplicaHttpRequest& operator=(WorkerCreateReplicaHttpRequest const&) = delete;

    /// Non-trivial destructor is needed to relese resources
    ~WorkerCreateReplicaHttpRequest() override;

    bool execute() override;

protected:
    void getResult(nlohmann::json& result) const override;

private:
    WorkerCreateReplicaHttpRequest(std::shared_ptr<ServiceProvider> const& serviceProvider,
                                   std::string const& worker, protocol::QueuedRequestHdr const& hdr,
                                   nlohmann::json const& req, ExpirationCallbackType const& onExpired);

    /**
     * Open files associated with the current state of iterator _fileItr.
     * @param lock lock which must be acquired before calling this method
     * @return 'false' in case of any error
     */
    bool _openFiles(replica::Lock const& lock);

    /**
     * The final stage to be executed just once after copying the content
     * of the remote files into the local temporary ones. It will rename
     * the temporary files into the standard ones. Resources will also be released.
     * @param lock A lock to be acquired before calling this method
     * @return always 'true'
     */
    bool _finalize(replica::Lock const& lock);

    /**
     * Close connections, de-allocate resources, etc.
     *
     * Any connections and open files will be closed, the buffers will be
     * released to prevent unnecessary resource utilization. Note that
     * request objects can stay in the server's memory for an extended
     * period of time.
     * @param lock A lock to be acquired before calling this method
     */
    void _releaseResources(replica::Lock const& lock);

    /**
     * Update file migration statistics
     * @param lock A lock to be acquired before calling this method
     */
    void _updateInfo(replica::Lock const& lock);

    // Input parameters (extracted from the request object)

    DatabaseInfo const _databaseInfo;  ///< Database descriptor obtained from the Configuration
    unsigned int const _chunk;
    std::string const _sourceWorker;
    std::string const _sourceWorkerHost;
    uint16_t const _sourceWorkerPort;
    std::string const _sourceWorkerHostPort;
    std::string const _sourceWorkerDataDir;

    /// Result of the operation
    ReplicaInfo _replicaInfo;

    /// The flag indicating if the initialization phase of the operation
    /// has already completed
    bool _initialized;

    std::vector<std::string> const _files;  ///< Short names of files to be copied

    /// The iterator pointing to the currently processed file.
    /// If it's set to _files.end() then it means the operation
    /// has finished.
    std::vector<std::string>::const_iterator _fileItr;

    /// This object represents the currently open (if any) input file
    /// on the source worker node
    std::shared_ptr<FileClient> _inFilePtr;

    std::FILE* _tmpFilePtr;  ///< The file pointer for the temporary output file

    /// The FileDescr structure encapsulates various parameters of a file
    struct FileDescr {
        size_t inSizeBytes = 0;   ///< The input file size as reported by a remote server
        size_t outSizeBytes = 0;  ///< Num. bytes read so far (changes during processing)
        std::time_t mtime = 0;    ///< The last modification time of the file (sec, UNIX Epoch)
        uint64_t cs = 0;          ///< Control sum computed locally while copying the file

        boost::filesystem::path tmpFile;  /// The absolute path to the temporary file

        /// The final (canonic) file name the temporary file will be renamed as
        /// upon a successful completion of the operation.
        boost::filesystem::path outFile;

        uint64_t beginTransferTime = 0;  ///< When the file transfer started
        uint64_t endTransferTime = 0;    ///< When the file transfer ended
    };

    /// Cached file descriptions mapping from short file names into
    /// the corresponding parameters.
    std::map<std::string, FileDescr> _file2descr;

    uint8_t* _buf;    ///< The buffer for storing file payload read from the remote service
    size_t _bufSize;  ///< The size of the buffer
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERCREATEREPLICAHTTPREQUEST_H
