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
#include "replica/worker/WorkerFindAllReplicasHttpRequest.h"

// System headers
#include <map>

// Third party headers
#include "boost/filesystem.hpp"

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/proto/Protocol.h"
#include "replica/util/FileUtils.h"
#include "replica/services/ServiceProvider.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

#define CONTEXT context("WorkerFindAllReplicasHttpRequest", __func__)

using namespace std;
namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerFindAllReplicasHttpRequest");

}  // namespace

namespace lsst::qserv::replica {

shared_ptr<WorkerFindAllReplicasHttpRequest> WorkerFindAllReplicasHttpRequest::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired) {
    auto ptr = shared_ptr<WorkerFindAllReplicasHttpRequest>(
            new WorkerFindAllReplicasHttpRequest(serviceProvider, worker, hdr, req, onExpired));
    ptr->init();
    return ptr;
}

WorkerFindAllReplicasHttpRequest::WorkerFindAllReplicasHttpRequest(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired)
        : WorkerHttpRequest(serviceProvider, worker, "FIND-ALL", hdr, req, onExpired),
          _database(req.at("database")),
          _databaseInfo(serviceProvider->config()->databaseInfo(_database)) {}

void WorkerFindAllReplicasHttpRequest::getResult(json& result) const {
    // No locking is needed here since the method is called only after
    // the request is completed.
    result["replica_info_many"] = json::array();
    for (auto const& replicaInfo : _replicaInfoCollection) {
        result["replica_info_many"].push_back(replicaInfo.toJson());
    }
}

bool WorkerFindAllReplicasHttpRequest::execute() {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT << " database: " << _databaseInfo.name);

    replica::Lock lock(_mtx, CONTEXT);
    checkIfCancelling(lock, CONTEXT);

    // Scan the data directory to find all files which match the expected pattern(s)
    // and group them by their chunk number
    WorkerHttpRequest::ErrorContext errorContext;
    boost::system::error_code ec;

    map<unsigned int, ReplicaInfo::FileInfoCollection> chunk2fileInfoCollection;
    {
        replica::Lock dataFolderLock(_mtxDataFolderOperations, CONTEXT);
        fs::path const dataDir =
                fs::path(_serviceProvider->config()->get<string>("worker", "data-dir")) / _databaseInfo.name;
        fs::file_status const stat = fs::status(dataDir, ec);
        errorContext = errorContext or
                       reportErrorIf(stat.type() == fs::status_error, protocol::StatusExt::FOLDER_STAT,
                                     "failed to check the status of directory: " + dataDir.string()) or
                       reportErrorIf(!fs::exists(stat), protocol::StatusExt::NO_FOLDER,
                                     "the directory does not exists: " + dataDir.string());
        try {
            for (fs::directory_entry& entry : fs::directory_iterator(dataDir)) {
                tuple<string, unsigned int, string> parsed;
                if (FileUtils::parsePartitionedFile(parsed, entry.path().filename().string(),
                                                    _databaseInfo)) {
                    LOGS(_log, LOG_LVL_DEBUG,
                         CONTEXT << " database: " << _databaseInfo.name
                                 << " file: " << entry.path().filename() << " table: " << get<0>(parsed)
                                 << " chunk: " << get<1>(parsed) << " ext: " << get<2>(parsed));

                    uint64_t const size = fs::file_size(entry.path(), ec);
                    errorContext = errorContext or
                                   reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_SIZE,
                                                 "failed to read file size: " + entry.path().string());

                    time_t const mtime = fs::last_write_time(entry.path(), ec);
                    errorContext = errorContext or
                                   reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_MTIME,
                                                 "failed to read file mtime: " + entry.path().string());

                    unsigned const chunk = get<1>(parsed);
                    chunk2fileInfoCollection[chunk].emplace_back(ReplicaInfo::FileInfo({
                            entry.path().filename().string(), size, mtime,
                            "",  /* cs is never computed for this type of requests */
                            0,   /* beginTransferTime */
                            0,   /* endTransferTime */
                            size /* inSize */
                    }));
                }
            }
        } catch (fs::filesystem_error const& ex) {
            errorContext = errorContext or reportErrorIf(true, protocol::StatusExt::FOLDER_READ,
                                                         "failed to read the directory: " + dataDir.string() +
                                                                 ", error: " + string(ex.what()));
        }
    }
    if (errorContext.failed) {
        setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
        return true;
    }

    // Analyze results to see which chunks are complete using chunk 0 as an example
    // of the total number of files which are normally associated with each chunk.
    size_t const numFilesPerChunkRequired = FileUtils::partitionedFiles(_databaseInfo, 0).size();
    for (auto&& entry : chunk2fileInfoCollection) {
        unsigned int const chunk = entry.first;
        size_t const numFiles = entry.second.size();
        _replicaInfoCollection.emplace_back(
                numFiles < numFilesPerChunkRequired ? ReplicaInfo::INCOMPLETE : ReplicaInfo::COMPLETE,
                worker(), _databaseInfo.name, chunk, util::TimeUtils::now(), chunk2fileInfoCollection[chunk]);
    }
    setStatus(lock, protocol::Status::SUCCESS);
    return true;
}

}  // namespace lsst::qserv::replica
