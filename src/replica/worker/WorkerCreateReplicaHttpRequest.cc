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
#include "replica/worker/WorkerCreateReplicaHttpRequest.h"

// System headers
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/proto/Protocol.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/FileUtils.h"
#include "replica/worker/FileClient.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

#define CONTEXT context("WorkerCreateReplicaHttpRequest", __func__)

using namespace std;
namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerCreateReplicaHttpRequest");

}  // namespace

namespace lsst::qserv::replica {

shared_ptr<WorkerCreateReplicaHttpRequest> WorkerCreateReplicaHttpRequest::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired) {
    auto ptr = shared_ptr<WorkerCreateReplicaHttpRequest>(
            new WorkerCreateReplicaHttpRequest(serviceProvider, worker, hdr, req, onExpired));
    ptr->init();
    return ptr;
}

WorkerCreateReplicaHttpRequest::WorkerCreateReplicaHttpRequest(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired)
        : WorkerHttpRequest(serviceProvider, worker, "REPLICATE", hdr, req, onExpired),
          _databaseInfo(serviceProvider->config()->databaseInfo(req.at("database"))),
          _chunk(req.at("chunk")),
          _sourceWorker(req.at("worker")),
          _sourceWorkerHost(req.at("worker_host")),
          _sourceWorkerPort(req.at("worker_port")),
          _sourceWorkerHostPort(_sourceWorkerHost + ":" + to_string(_sourceWorkerPort)),
          _initialized(false),
          _files(FileUtils::partitionedFiles(_databaseInfo, _chunk)),
          _tmpFilePtr(nullptr),
          _buf(0),
          _bufSize(serviceProvider->config()->get<size_t>("worker", "fs-buf-size-bytes")) {
    if (worker == _sourceWorker) {
        throw invalid_argument(CONTEXT + " workers are the same in the request.");
    }
    if (_sourceWorkerHost.empty()) {
        throw invalid_argument(CONTEXT + " the DNS name or an IP address of the worker not provided.");
    }
}

WorkerCreateReplicaHttpRequest::~WorkerCreateReplicaHttpRequest() {
    replica::Lock lock(_mtx, CONTEXT);
    _releaseResources(lock);
}

void WorkerCreateReplicaHttpRequest::getResult(json& result) const {
    // No locking is needed here since the method is called only after
    // the request is completed.
    result["replica_info"] = _replicaInfo.toJson();
}

bool WorkerCreateReplicaHttpRequest::execute() {
    LOGS(_log, LOG_LVL_DEBUG,
         CONTEXT << " sourceWorkerHostPort: " << _sourceWorkerHostPort << " database: " << _databaseInfo.name
                 << " chunk: " << _chunk);

    replica::Lock lock(_mtx, CONTEXT);
    checkIfCancelling(lock, CONTEXT);

    // Obtain the list of files to be migrated
    //
    // IMPLEMENTATION NOTES:
    //
    // - Note using the overloaded operator '/' which is used to form
    //   folders and files path names below. The operator will concatenate
    //   names and also insert a file separator for an operating system
    //   on which this code will get compiled.
    //
    // - Temporary file names at a destination folders are prepended with
    //   prefix '_' to prevent colliding with the canonical names. They will
    //   be renamed in the last step.
    //
    // - All operations with the file system namespace (creating new non-temporary
    //   files, checking for folders and files, renaming files, creating folders, etc.)
    //   are guarded by acquiring replica::Lock lock(_mtxDataFolderOperations) where it's needed.

    WorkerHttpRequest::ErrorContext errorContext;

    ///////////////////////////////////////////////////////
    //       Initialization phase (runs only once)       //
    ///////////////////////////////////////////////////////

    if (!_initialized) {
        _initialized = true;

        fs::path const outDir =
                fs::path(serviceProvider()->config()->get<string>("worker", "data-dir")) / _databaseInfo.name;

        vector<fs::path> tmpFiles;
        vector<fs::path> outFiles;
        for (auto&& file : _files) {
            fs::path const tmpFile = outDir / ("_" + file);
            tmpFiles.push_back(tmpFile);

            fs::path const outFile = outDir / file;
            outFiles.push_back(outFile);

            _file2descr[file].inSizeBytes = 0;
            _file2descr[file].outSizeBytes = 0;
            _file2descr[file].mtime = 0;
            _file2descr[file].cs = 0;
            _file2descr[file].tmpFile = tmpFile;
            _file2descr[file].outFile = outFile;
            _file2descr[file].beginTransferTime = 0;
            _file2descr[file].endTransferTime = 0;
        }

        // Check input files, check and sanitize the destination folder

        boost::system::error_code ec;
        {
            replica::Lock dataFolderLock(_mtxDataFolderOperations, CONTEXT);

            // Check for a presence of input files and calculate space requirement

            uintmax_t totalBytes = 0;          // the total number of bytes in all input files to be moved
            map<string, uintmax_t> file2size;  // the number of bytes in each file

            for (auto&& file : _files) {
                // Open the file on the remote server in the no-content-read mode
                auto const inFilePtr = FileClient::stat(_serviceProvider, _sourceWorkerHost,
                                                        _sourceWorkerPort, _databaseInfo.name, file);
                errorContext =
                        errorContext or
                        reportErrorIf(inFilePtr == nullptr, protocol::StatusExt::FILE_ROPEN,
                                      "failed to open input file on remote worker: " + _sourceWorker + " (" +
                                              _sourceWorkerHostPort + "), database: " + _databaseInfo.name +
                                              ", file: " + file);
                if (errorContext.failed) {
                    setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
                    return true;
                }
                file2size[file] = inFilePtr->size();
                totalBytes += inFilePtr->size();
                _file2descr[file].inSizeBytes = inFilePtr->size();
                _file2descr[file].mtime = inFilePtr->mtime();
            }

            // Check and sanitize the output directory

            bool const outDirExists = fs::exists(outDir, ec);
            errorContext =
                    errorContext or
                    reportErrorIf(ec.value() != 0, protocol::StatusExt::FOLDER_STAT,
                                  "failed to check the status of output directory: " + outDir.string()) or
                    reportErrorIf(!outDirExists, protocol::StatusExt::NO_FOLDER,
                                  "the output directory doesn't exist: " + outDir.string());

            // The files with canonical(!) names should NOT exist at the destination
            // folder.
            for (auto&& file : outFiles) {
                fs::file_status const stat = fs::status(file, ec);
                errorContext = errorContext or
                               reportErrorIf(stat.type() == fs::status_error, protocol::StatusExt::FILE_STAT,
                                             "failed to check the status of output file: " + file.string()) or
                               reportErrorIf(fs::exists(stat), protocol::StatusExt::FILE_EXISTS,
                                             "the output file already exists: " + file.string());
            }

            // Check if there are any files with the temporary names at the destination
            // folder and if so then get rid of them.
            for (auto&& file : tmpFiles) {
                fs::file_status const stat = fs::status(file, ec);
                errorContext =
                        errorContext or
                        reportErrorIf(stat.type() == fs::status_error, protocol::StatusExt::FILE_STAT,
                                      "failed to check the status of temporary file: " + file.string());
                if (fs::exists(stat)) {
                    fs::remove(file, ec);
                    errorContext = errorContext or
                                   reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_DELETE,
                                                 "failed to remove temporary file: " + file.string());
                }
            }

            // Make sure a file system at the destination has enough space
            // to accommodate new files
            //
            // NOTE: this operation runs after cleaning up temporary files
            fs::space_info const space = fs::space(outDir, ec);
            errorContext =
                    errorContext or
                    reportErrorIf(
                            ec.value() != 0, protocol::StatusExt::SPACE_REQ,
                            "failed to obtaine space information at output folder: " + outDir.string()) or
                    reportErrorIf(space.available < totalBytes, protocol::StatusExt::NO_SPACE,
                                  "not enough free space availble at output folder: " + outDir.string());

            // Pre-create temporary files with the final size to assert disk space
            // availability before filling these files with the actual payload.
            for (auto&& file : _files) {
                fs::path const tmpFile = _file2descr[file].tmpFile;

                // Create a file of size 0
                FILE* tmpFilePtr = fopen(tmpFile.string().c_str(), "wb");
                errorContext = errorContext or
                               reportErrorIf(tmpFilePtr == nullptr, protocol::StatusExt::FILE_CREATE,
                                             "failed to open/create temporary file: " + tmpFile.string() +
                                                     ", error: " + strerror(errno));
                if (tmpFilePtr) {
                    fflush(tmpFilePtr);
                    fclose(tmpFilePtr);
                }

                // Resize the file (will be filled with \0)
                fs::resize_file(tmpFile, file2size[file], ec);
                errorContext = errorContext or
                               reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_RESIZE,
                                             "failed to resize the temporary file: " + tmpFile.string());
            }
        }
        if (errorContext.failed) {
            setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
            return true;
        }

        // Allocate the record buffer
        _buf = new uint8_t[_bufSize];
        if (_buf == nullptr) throw runtime_error(CONTEXT + " buffer allocation failed");

        // Setup the iterator for the name of the very first file to be copied
        _fileItr = _files.begin();
        if (!_openFiles(lock)) return true;
    }

    // Copy the next record from the currently open remote file
    // into the corresponding temporary files at the destination folder
    // w/o acquiring the directory lock.
    //
    // NOTE: the while loop below is meant to skip files which are empty
    while (_files.end() != _fileItr) {
        // Copy the next record if any is available
        size_t num = 0;
        try {
            num = _inFilePtr->read(_buf, _bufSize);
            if (num) {
                if (num == fwrite(_buf, sizeof(uint8_t), num, _tmpFilePtr)) {
                    // Update the descriptor (the number of bytes copied so far
                    // and the control sum)
                    _file2descr[*_fileItr].outSizeBytes += num;
                    uint64_t& cs = _file2descr[*_fileItr].cs;
                    for (uint8_t *ptr = _buf, *end = _buf + num; ptr != end; ++ptr) {
                        cs += *ptr;
                    }

                    // Keep updating this stats while copying the files
                    _file2descr[*_fileItr].endTransferTime = util::TimeUtils::now();
                    _updateInfo(lock);

                    // Keep copying the same file
                    return false;
                }
                errorContext = errorContext or reportErrorIf(true, protocol::StatusExt::FILE_WRITE,
                                                             "failed to write into temporary file: " +
                                                                     _file2descr[*_fileItr].tmpFile.string() +
                                                                     ", error: " + strerror(errno));
            }
        } catch (FileClientError const& ex) {
            errorContext =
                    errorContext or
                    reportErrorIf(true, protocol::StatusExt::FILE_READ,
                                  "failed to read input file from remote worker: " + _sourceWorker + " (" +
                                          _sourceWorkerHostPort + "), database: " + _databaseInfo.name +
                                          ", file: " + *_fileItr);
        }

        // Make sure the number of bytes copied from the remote server
        // matches expectations.
        errorContext =
                errorContext or
                reportErrorIf(_file2descr[*_fileItr].inSizeBytes != _file2descr[*_fileItr].outSizeBytes,
                              protocol::StatusExt::FILE_READ,
                              "short read of the input file from remote worker: " + _sourceWorker + " (" +
                                      _sourceWorkerHostPort + "), database: " + _databaseInfo.name +
                                      ", file: " + *_fileItr);
        if (errorContext.failed) {
            setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
            _releaseResources(lock);
            return true;
        }

        // Flush and close the current file
        fflush(_tmpFilePtr);
        fclose(_tmpFilePtr);
        _tmpFilePtr = 0;

        // Keep updating this stats after finishing to copy each file
        _file2descr[*_fileItr].endTransferTime = util::TimeUtils::now();
        _updateInfo(lock);

        // Move the iterator to the name of the next file to be copied
        ++_fileItr;
        if (_files.end() != _fileItr) {
            if (!_openFiles(lock)) {
                _releaseResources(lock);
                return true;
            }
        }
    }

    // Finalize the operation, de-allocate resources, etc.
    return _finalize(lock);
}

bool WorkerCreateReplicaHttpRequest::_openFiles(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG,
         CONTEXT << " sourceWorkerHostPort: " << _sourceWorkerHostPort << " database: " << _databaseInfo.name
                 << " chunk: " << _chunk << " file: " << *_fileItr);

    WorkerHttpRequest::ErrorContext errorContext;

    // Open the input file on the remote server
    _inFilePtr = FileClient::open(_serviceProvider, _sourceWorkerHost, _sourceWorkerPort, _databaseInfo.name,
                                  *_fileItr);
    errorContext = errorContext or
                   reportErrorIf(_inFilePtr == nullptr, protocol::StatusExt::FILE_ROPEN,
                                 "failed to open input file on remote worker: " + _sourceWorker + " (" +
                                         _sourceWorkerHostPort + "), database: " + _databaseInfo.name +
                                         ", file: " + *_fileItr);
    if (errorContext.failed) {
        setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
        return false;
    }

    // Reopen a temporary output file locally in the 'append binary mode'
    // then 'rewind' to the beginning of the file before writing into it.
    fs::path const tmpFile = _file2descr[*_fileItr].tmpFile;

    _tmpFilePtr = fopen(tmpFile.string().c_str(), "wb");
    errorContext = errorContext or reportErrorIf(_tmpFilePtr == nullptr, protocol::StatusExt::FILE_OPEN,
                                                 "failed to open temporary file: " + tmpFile.string() +
                                                         ", error: " + strerror(errno));
    if (errorContext.failed) {
        setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
        return false;
    }
    rewind(_tmpFilePtr);
    _file2descr[*_fileItr].beginTransferTime = util::TimeUtils::now();
    return true;
}

bool WorkerCreateReplicaHttpRequest::_finalize(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG,
         CONTEXT << " sourceWorkerHostPort: " << _sourceWorkerHostPort << " database: " << _databaseInfo.name
                 << " chunk: " << _chunk);

    // Unconditionally regardless of the completion of the file renaming attempt
    _releaseResources(lock);

    // Rename temporary files into the canonical ones
    // Note that this operation changes the directory namespace in a way
    // which may affect other users (like replica lookup operations, etc.). Hence we're
    // acquiring the directory lock to guarantee a consistent view onto the folder.
    replica::Lock dataFolderLock(_mtxDataFolderOperations, CONTEXT);

    // ATTENTION: as per ISO/IEC 9945 the file rename operation will
    //            remove empty files. Not sure if this should be treated
    //            in a special way?
    WorkerHttpRequest::ErrorContext errorContext;
    boost::system::error_code ec;
    for (auto&& file : _files) {
        fs::path const tmpFile = _file2descr[file].tmpFile;
        fs::path const outFile = _file2descr[file].outFile;

        fs::rename(tmpFile, outFile, ec);
        errorContext = errorContext or reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_RENAME,
                                                     "failed to rename file: " + tmpFile.string());
        fs::last_write_time(outFile, _file2descr[file].mtime, ec);
        errorContext = errorContext or reportErrorIf(ec.value() != 0, protocol::StatusExt::FILE_MTIME,
                                                     "failed to change 'mtime' of file: " + tmpFile.string());
    }
    if (errorContext.failed) {
        setStatus(lock, protocol::Status::FAILED, errorContext.extendedStatus);
        return true;
    }
    setStatus(lock, protocol::Status::SUCCESS);
    return true;
}

void WorkerCreateReplicaHttpRequest::_updateInfo(replica::Lock const& lock) {
    size_t totalInSizeBytes = 0;
    size_t totalOutSizeBytes = 0;
    ReplicaInfo::FileInfoCollection fileInfoCollection;
    for (auto&& file : _files) {
        fileInfoCollection.emplace_back(
                ReplicaInfo::FileInfo({file, _file2descr[file].outSizeBytes, _file2descr[file].mtime,
                                       to_string(_file2descr[file].cs), _file2descr[file].beginTransferTime,
                                       _file2descr[file].endTransferTime, _file2descr[file].inSizeBytes}));
        totalInSizeBytes += _file2descr[file].inSizeBytes;
        totalOutSizeBytes += _file2descr[file].outSizeBytes;
    }
    ReplicaInfo::Status const status =
            (_files.size() == fileInfoCollection.size()) and (totalInSizeBytes == totalOutSizeBytes)
                    ? ReplicaInfo::Status::COMPLETE
                    : ReplicaInfo::Status::INCOMPLETE;

    // Fill in the info on the chunk before finishing the operation
    WorkerCreateReplicaHttpRequest::_replicaInfo = ReplicaInfo(status, worker(), _databaseInfo.name, _chunk,
                                                               util::TimeUtils::now(), fileInfoCollection);
}

void WorkerCreateReplicaHttpRequest::_releaseResources(replica::Lock const& lock) {
    // Drop a connection to the remote server
    _inFilePtr.reset();

    // Close the output file
    if (_tmpFilePtr) {
        fflush(_tmpFilePtr);
        fclose(_tmpFilePtr);
        _tmpFilePtr = nullptr;
    }

    // Release the record buffer
    if (_buf) {
        delete[] _buf;
        _buf = nullptr;
    }
}

}  // namespace lsst::qserv::replica
