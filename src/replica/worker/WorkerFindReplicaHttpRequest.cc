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
#include "replica/worker/WorkerFindReplicaHttpRequest.h"

// System headers

// Third party headers
#include "boost/filesystem.hpp"

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/FileUtils.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

#define CONTEXT context("WorkerFindReplicaHttpRequest", __func__)

using namespace std;
namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerFindReplicaHttpRequest");

}  // namespace

namespace lsst::qserv::replica {

shared_ptr<WorkerFindReplicaHttpRequest> WorkerFindReplicaHttpRequest::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired) {
    auto ptr = shared_ptr<WorkerFindReplicaHttpRequest>(
            new WorkerFindReplicaHttpRequest(serviceProvider, worker, hdr, req, onExpired));
    ptr->init();
    return ptr;
}

WorkerFindReplicaHttpRequest::WorkerFindReplicaHttpRequest(shared_ptr<ServiceProvider> const& serviceProvider,
                                                           string const& worker,
                                                           protocol::QueuedRequestHdr const& hdr,
                                                           json const& req,
                                                           ExpirationCallbackType const& onExpired)
        : WorkerHttpRequest(serviceProvider, worker, "FIND", hdr, req, onExpired),
          _databaseInfo(serviceProvider->config()->databaseInfo(req.at("database"))),
          _chunk(req.at("chunk")),
          _computeCheckSum(req.at("compute_cs")) {}

void WorkerFindReplicaHttpRequest::getResult(json& result) const {
    // No locking is needed here since the method is called only after
    // the request is completed.
    result["replica_info"] = _replicaInfo.toJson();
}

bool WorkerFindReplicaHttpRequest::execute() {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT << " database: " << _databaseInfo.name << " chunk: " << _chunk);

    replica::Lock lock(_mtx, CONTEXT);
    checkIfCancelling(lock, CONTEXT);

    // There are two modes of operation of the code which would depend
    // on a presence (or a lack of that) to calculate control/check sums
    // for the found files.
    //
    // - if the control/check sum is NOT requested then the request will
    //   be executed immediately within this call.
    //
    // - otherwise the incremental approach will be used (which will require
    //   setting up the incremental engine if this is the first call to the method)
    //
    // Both methods are combined within the same code block to avoid
    // code duplication.
    WorkerHttpRequest::ErrorContext errorContext;
    boost::system::error_code ec;

    if (!_computeCheckSum or (_csComputeEnginePtr == nullptr)) {
        // Check if the data directory exists and it can be read
        replica::Lock dataFolderLock(_mtxDataFolderOperations, CONTEXT);
        fs::path const dataDir =
                fs::path(_serviceProvider->config()->get<string>("worker", "data-dir")) / _databaseInfo.name;
        fs::file_status const stat = fs::status(dataDir, ec);
        errorContext = errorContext or
                       reportErrorIf(stat.type() == fs::status_error, protocol::StatusExt::FOLDER_STAT,
                                     "failed to check the status of directory: " + dataDir.string()) or
                       reportErrorIf(!fs::exists(stat), protocol::StatusExt::NO_FOLDER,
                                     "the directory does not exists: " + dataDir.string());
        if (errorContext.failed) {
            setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
            return true;
        }

        // For each file associated with the chunk check if the file is present in
        // the data directory.
        //
        // - not finding a file is not a failure for this operation. Just reporting
        //   those files which are present.
        //
        // - assume the request failure for any file system operation failure
        //
        // - assume the successful completion otherwise and adjust the replica
        //   information record accordingly, depending on the findings.
        ReplicaInfo::FileInfoCollection
                fileInfoCollection;  // file info if not using the incremental processing
        vector<string> files;        // file paths registered for the incremental processing

        for (auto&& file : FileUtils::partitionedFiles(_databaseInfo, _chunk)) {
            fs::path const path = dataDir / file;
            fs::file_status const stat = fs::status(path, ec);
            errorContext = errorContext or
                           reportErrorIf(stat.type() == fs::status_error, protocol::StatusExt::FILE_STAT,
                                         "failed to check the status of file: " + path.string());
            if (fs::exists(stat)) {
                if (!_computeCheckSum) {
                    // Get file size & mtime right away
                    uint64_t const size = fs::file_size(path, ec);
                    errorContext =
                            errorContext or reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_SIZE,
                                                          "failed to read file size: " + path.string());
                    const time_t mtime = fs::last_write_time(path, ec);
                    errorContext =
                            errorContext or reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_MTIME,
                                                          "failed to read file mtime: " + path.string());
                    fileInfoCollection.emplace_back(ReplicaInfo::FileInfo({
                            file, size, mtime, "", /* cs */
                            0,                     /* beginTransferTime */
                            0,                     /* endTransferTime */
                            size                   /* inSize */
                    }));
                } else {
                    // Register this file for the incremental processing
                    files.push_back(path.string());
                }
            }
        }
        if (errorContext.failed) {
            setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
            return true;
        }

        // If that's so then finalize the operation right away
        if (!_computeCheckSum) {
            ReplicaInfo::Status status = ReplicaInfo::Status::NOT_FOUND;
            if (fileInfoCollection.size())
                status =
                        FileUtils::partitionedFiles(_databaseInfo, _chunk).size() == fileInfoCollection.size()
                                ? ReplicaInfo::Status::COMPLETE
                                : ReplicaInfo::Status::INCOMPLETE;

            // Fill in the info on the chunk before finishing the operation
            _replicaInfo = ReplicaInfo(status, worker(), _databaseInfo.name, _chunk, util::TimeUtils::now(),
                                       fileInfoCollection);
            setStatus(lock, protocol::Status::SUCCESS);
            return true;
        }

        // Otherwise proceed with the incremental approach
        _csComputeEnginePtr.reset(new MultiFileCsComputeEngine(files));
    }

    // Next (or the first) iteration in the incremental approach
    bool finished = true;
    try {
        finished = _csComputeEnginePtr->execute();
        if (finished) {
            // Extract statistics
            ReplicaInfo::FileInfoCollection fileInfoCollection;
            auto const fileNames = _csComputeEnginePtr->fileNames();
            for (auto&& file : fileNames) {
                const fs::path path(file);
                uint64_t const size = _csComputeEnginePtr->bytes(file);
                time_t const mtime = fs::last_write_time(path, ec);
                errorContext = errorContext or reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_MTIME,
                                                             "failed to read file mtime: " + path.string());
                fileInfoCollection.emplace_back(ReplicaInfo::FileInfo({
                        path.filename().string(), size, mtime, to_string(_csComputeEnginePtr->cs(file)),
                        0,   /* beginTransferTime */
                        0,   /* endTransferTime */
                        size /* inSize */
                }));
            }
            if (errorContext.failed) {
                setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
                return true;
            }

            // Fnalize the operation
            ReplicaInfo::Status status = ReplicaInfo::Status::NOT_FOUND;
            if (fileInfoCollection.size())
                status = FileUtils::partitionedFiles(_databaseInfo, _chunk).size() == fileNames.size()
                                 ? ReplicaInfo::Status::COMPLETE
                                 : ReplicaInfo::Status::INCOMPLETE;

            // Fill in the info on the chunk before finishing the operation
            _replicaInfo = ReplicaInfo(status, worker(), _databaseInfo.name, _chunk, util::TimeUtils::now(),
                                       fileInfoCollection);
            setStatus(lock, protocol::Status::SUCCESS);
        }
    } catch (exception const& ex) {
        WorkerHttpRequest::ErrorContext errorContext;
        errorContext = errorContext or reportErrorIf(true, protocol::StatusExt::FILE_READ, ex.what());
        setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
    }

    // If done (either way) then get rid of the engine right away because
    // it may still have allocated buffers
    if (finished) _csComputeEnginePtr.reset();
    return finished;
}

}  // namespace lsst::qserv::replica
