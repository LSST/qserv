// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
#include "ccontrol/MergingHandler.h"

// System headers
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <vector>

// Third-party headers
#include "curl/curl.h"
#include "XrdCl/XrdClFile.hh"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "ccontrol/msgCode.h"
#include "global/clock_defs.h"
#include "global/debugUtil.h"
#include "http/Client.h"
#include "http/ClientConnPool.h"
#include "http/Method.h"
#include "proto/ProtoHeaderWrap.h"
#include "proto/worker.pb.h"
#include "qdisp/CzarStats.h"
#include "qdisp/JobQuery.h"
#include "qdisp/UberJob.h"
#include "rproc/InfileMerger.h"
#include "util/Bug.h"
#include "util/common.h"

using lsst::qserv::proto::ProtoHeaderWrap;
using lsst::qserv::proto::ResponseData;
using lsst::qserv::proto::ResponseSummary;
namespace http = lsst::qserv::http;

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.MergingHandler");

/**
 * Instances of this class are used to update statistic counter on starting
 * and finishing operations with the result files.
 */
class ResultFileTracker {
public:
    ResultFileTracker() { lsst::qserv::qdisp::CzarStats::get()->addResultFile(); }
    ~ResultFileTracker() { lsst::qserv::qdisp::CzarStats::get()->deleteResultFile(); }
};

// The logging function employed by the transmit rate tracker to report
// the data transfer rates in a histogram. The histogram is used in
// the performance monitoring of the application.
lsst::qserv::TimeCountTracker<double>::CALLBACKFUNC const reportFileRecvRate =
        [](lsst::qserv::TIMEPOINT start, lsst::qserv::TIMEPOINT end, double bytes, bool success) {
            if (!success) return;
            if (chrono::duration<double> const seconds = end - start; seconds.count() > 0) {
                lsst::qserv::qdisp::CzarStats::get()->addFileReadRate(bytes / seconds.count());
            }
        };

/// If success, then everything is fine.
/// If not success, and not mergeHappened, the user query can be saved by abandoning
/// this UberJob. If mergeHappened, the result table is fouled and the user query is ruined.
/// @return bool success - true if operation was successful.
/// @return bool mergeHappened - true if merging was started.
std::tuple<bool, bool> readHttpFileAndMergeHttp(
        lsst::qserv::qdisp::UberJob::Ptr const& uberJob, string const& httpUrl,
        function<bool(char const*, uint32_t, bool&)> const& messageIsReady,
        shared_ptr<http::ClientConnPool> const& httpConnPool) {
    string const context = "MergingHandler::" + string(__func__) + " " + " qid=" + uberJob->getIdStr() + " ";

    LOGS(_log, LOG_LVL_DEBUG, context << "httpUrl=" << httpUrl);

    // Track the file while the control flow is staying within the function.
    ResultFileTracker const resultFileTracker;

    // The data transmit rate tracker is set up before reading each data message.
    unique_ptr<lsst::qserv::TimeCountTracker<double>> transmitRateTracker;

    // A location of the next byte to be read from the input file. The variable
    // is used for error reporting.
    uint64_t offset = 0;

    // Temporary buffer for messages read from the file. The buffer gets automatically
    // resized to fit the largest message.
    unique_ptr<char[]> msgBuf;
    size_t msgBufSize = 0;
    size_t msgBufNext = 0;  // An index of the next character in the buffer.

    // Fixed-size buffer to store the message size.
    string msgSizeBuf(sizeof(uint32_t), '\0');
    size_t msgSizeBufNext = 0;  // An index of the next character in the buffer.

    // The size of the next/current message. The variable is set after succesfully parsing
    // the message length header and is reset back to 0 after parsing the message body.
    // The value is stays 0 while reading the frame header.
    uint32_t msgSizeBytes = 0;
    bool success = true;
    bool mergeHappened = false;
    int headerCount = 0;
    uint64_t totalBytesRead = 0;
    try {
        auto exec = uberJob->getExecutive();
        if (exec == nullptr || exec->getCancelled()) {
            throw runtime_error(context + " query was cancelled");
        }
        string const noClientData;
        vector<string> const noClientHeaders;
        http::ClientConfig clientConfig;
        clientConfig.httpVersion = CURL_HTTP_VERSION_1_1;  // same as in qhttp
        clientConfig.bufferSize = CURL_MAX_READ_SIZE;      // 10 MB in the current version of libcurl
        clientConfig.tcpKeepAlive = true;
        clientConfig.tcpKeepIdle = 5;   // the default is 60 sec
        clientConfig.tcpKeepIntvl = 5;  // the default is 60 sec
        http::Client reader(http::Method::GET, httpUrl, noClientData, noClientHeaders, clientConfig,
                            httpConnPool);
        reader.read([&](char const* inBuf, size_t inBufSize) {
            // A value of the flag is set by the message processor when it's time to finish
            // or abort reading  the file.
            bool last = false;
            char const* next = inBuf;
            char const* const end = inBuf + inBufSize;
            LOGS(_log, LOG_LVL_TRACE, context << " next=" << (uint64_t)next << " end=" << (uint64_t)end);
            while ((next < end) && !last) {
                if (exec->getCancelled()) {
                    throw runtime_error(context + " query was cancelled");
                }
                if (msgSizeBytes == 0) {
                    // Continue or finish reading the frame header.
                    size_t const bytes2read =
                            std::min(sizeof(uint32_t) - msgSizeBufNext, (size_t)(end - next));
                    std::memcpy(msgSizeBuf.data() + msgSizeBufNext, next, bytes2read);
                    next += bytes2read;
                    offset += bytes2read;
                    msgSizeBufNext += bytes2read;
                    if (msgSizeBufNext == sizeof(uint32_t)) {
                        ++headerCount;
                        // Done reading the frame header.
                        msgSizeBufNext = 0;
                        // Parse and evaluate the message length.
                        msgSizeBytes = *(reinterpret_cast<uint32_t*>(msgSizeBuf.data()));
                        if (msgSizeBytes == 0) {
                            throw runtime_error("message size is 0 at offset " +
                                                to_string(offset - sizeof(uint32_t)) + ", file: " + httpUrl);
                        }
                        if (msgSizeBytes > ProtoHeaderWrap::PROTOBUFFER_HARD_LIMIT) {
                            throw runtime_error("message size " + to_string(msgSizeBytes) + " at offset " +
                                                to_string(offset - sizeof(uint32_t)) +
                                                " exceeds the hard limit of " +
                                                to_string(ProtoHeaderWrap::PROTOBUFFER_HARD_LIMIT) +
                                                ", file: " + httpUrl);
                        }
                        // Extend the message buffer (if needed). Note that buffer never gets
                        // truncated to avoid excessive memory deallocations/allocations.
                        if (msgBufSize < msgSizeBytes) {
                            msgBufSize = msgSizeBytes;
                            msgBuf.reset(new char[msgBufSize]);
                        }
                        // Starts the tracker to measure the performance of the network I/O.
                        transmitRateTracker =
                                make_unique<lsst::qserv::TimeCountTracker<double>>(reportFileRecvRate);
                    }
                } else {
                    // Continue or finish reading the message body.
                    size_t const bytes2read =
                            std::min((size_t)msgSizeBytes - msgBufNext, (size_t)(end - next));
                    std::memcpy(msgBuf.get() + msgBufNext, next, bytes2read);
                    next += bytes2read;
                    offset += bytes2read;
                    msgBufNext += bytes2read;
                    if (msgBufNext == msgSizeBytes) {
                        // Done reading message body.
                        msgBufNext = 0;

                        // Destroying the tracker will result in stopping the tracker's timer and
                        // reporting the file read rate before proceeding to the merge.
                        if (transmitRateTracker != nullptr) {
                            transmitRateTracker->addToValue(msgSizeBytes);
                            transmitRateTracker->setSuccess();
                            transmitRateTracker.reset();
                        }

                        // Parse and evaluate the message.
                        mergeHappened = true;
                        bool messageReadyResult = messageIsReady(msgBuf.get(), msgSizeBytes, last);
                        totalBytesRead += msgSizeBytes;
                        if (!messageReadyResult) {
                            success = false;
                            throw runtime_error("message processing failed at offset " +
                                                to_string(offset - msgSizeBytes) + ", file: " + httpUrl);
                        }
                        // Reset the variable to prepare for reading the next header & message (if any).
                        msgSizeBytes = 0;
                    } else {
                        LOGS(_log, LOG_LVL_TRACE,
                             context << " headerCount=" << headerCount
                                     << " incomplete read diff=" << (msgSizeBytes - msgBufNext));
                    }
                }
            }
        });
        LOGS(_log, LOG_LVL_TRACE,
             context << " headerCount=" << headerCount << " msgSizeBytes=" << msgSizeBytes
                     << " totalBytesRead=" << totalBytesRead);
        if (msgSizeBufNext != 0) {
            throw runtime_error("short read of the message header at offset " +
                                to_string(offset - msgSizeBytes) + ", file: " + httpUrl);
        }
        if (msgBufNext != 0) {
            throw runtime_error("short read of the message body at offset " +
                                to_string(offset - msgSizeBytes) + ", file: " + httpUrl);
        }
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR, context + " " + ex.what());
        success = false;
    }

    // Remove the file from the worker if it still exists. Report and ignore errors.
    // The files will be garbage-collected by workers.
    try {
        http::Client remover(http::Method::DELETE, httpUrl);
        remover.read([](char const* inBuf, size_t inBufSize) {});
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_WARN, context << "failed to remove " << httpUrl << ", ex: " << ex.what());
    }
    // If the merge failed, that indicates something went wrong in the local database table,
    // is likely this user query is doomed and should be cancelled.
    LOGS(_log, LOG_LVL_DEBUG, context << " end succes=" << success << " mergeSuccess=" << mergeHappened);
    return {success, mergeHappened};
}

}  // namespace

namespace lsst::qserv::ccontrol {

shared_ptr<http::ClientConnPool> MergingHandler::_httpConnPool;
mutex MergingHandler::_httpConnPoolMutex;

shared_ptr<http::ClientConnPool> const& MergingHandler::_getHttpConnPool() {
    lock_guard<mutex> const lock(_httpConnPoolMutex);
    if (nullptr == _httpConnPool) {
        _httpConnPool = make_shared<http::ClientConnPool>(
                cconfig::CzarConfig::instance()->getResultMaxHttpConnections());
    }
    return _httpConnPool;
}

MergingHandler::MergingHandler(std::shared_ptr<rproc::InfileMerger> merger, std::string const& tableName)
        : _infileMerger{merger}, _tableName{tableName} {}

MergingHandler::~MergingHandler() { LOGS(_log, LOG_LVL_TRACE, __func__ << " " << _tableName); }

void MergingHandler::errorFlush(std::string const& msg, int code) {
    _setError(code, msg);
    // Might want more info from result service.
    // Do something about the error. FIXME.
    LOGS(_log, LOG_LVL_ERROR, "Error receiving result.");
}

// Note that generally we always have an _infileMerger object except during
// a unit test. I suppose we could try to figure out how to create one.
//
void MergingHandler::prepScrubResults(int jobId, int attemptCount) {
    if (_infileMerger) _infileMerger->prepScrub(jobId, attemptCount);
}

std::ostream& MergingHandler::print(std::ostream& os) const {
    return os << "MergingRequester(" << _tableName << ", flushed=" << (_flushed ? "true)" : "false)");
}

bool MergingHandler::_mergeHttp(shared_ptr<qdisp::UberJob> const& uberJob,
                                proto::ResponseData const& responseData) {
    if (_flushed) {
        throw util::Bug(ERR_LOC, "already flushed");
    }
    bool const success = _infileMerger->mergeHttp(uberJob, responseData);
    if (!success) {
        LOGS(_log, LOG_LVL_WARN, __func__ << " failed");
        util::Error const& err = _infileMerger->getError();
        _setError(ccontrol::MSG_RESULT_ERROR, err.getMsg());
    }
    return success;
}

void MergingHandler::_setError(int code, std::string const& msg) {
    LOGS(_log, LOG_LVL_DEBUG, "_setError: code: " << code << ", message: " << msg);
    std::lock_guard<std::mutex> lock(_errorMutex);
    _error = Error(code, msg);
}

tuple<bool, bool> MergingHandler::flushHttp(string const& fileUrl, uint64_t expectedRows,
                                            uint64_t& resultRows) {
    bool success = false;
    bool shouldCancel = false;

    // This is needed to ensure the job query would be staying alive for the duration
    // of the operation to prevent inconsistency within the application.
    auto const uberJob = getUberJob().lock();
    if (uberJob == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, __func__ << " failed, uberJob was NULL");
        return {success, shouldCancel};  // both should still be false
    }

    LOGS(_log, LOG_LVL_TRACE,
         "MergingHandler::" << __func__ << " uberJob=" << uberJob->getIdStr() << " fileUrl=" << fileUrl);

    // Dispatch result processing to the corresponidng method which depends on
    // the result delivery protocol configured at the worker.
    // Notify the file reader when all rows have been read by setting 'last = true'.
    auto const dataMergerHttp = [&](char const* buf, uint32_t bufSize, bool& last) {
        LOGS(_log, LOG_LVL_TRACE, "dataMergerHttp");
        last = true;
        proto::ResponseData responseData;
        if (responseData.ParseFromArray(buf, bufSize) && responseData.IsInitialized()) {
            bool const mergeSuccess = _mergeHttp(uberJob, responseData);
            if (mergeSuccess) {
                resultRows += responseData.row_size();
                last = resultRows >= expectedRows;
            }
            return mergeSuccess;
        }
        throw runtime_error("MergingHandler::flush ** message deserialization failed **");
    };

    tie(success, shouldCancel) =
            ::readHttpFileAndMergeHttp(uberJob, fileUrl, dataMergerHttp, MergingHandler::_getHttpConnPool());

    if (!success || shouldCancel) {
        LOGS(_log, LOG_LVL_WARN, __func__ << " success=" << success << " shouldCancel=" << shouldCancel);
    }

    if (success) {
        _infileMerger->mergeCompleteFor(uberJob->getUjId());
    }
    return {success, shouldCancel};
}

void MergingHandler::flushHttpError(int errorCode, std::string const& errorMsg, int status) {
    if (!_errorSet.exchange(true)) {
        _error = util::Error(errorCode, errorMsg, util::ErrorCode::MYSQLEXEC);
        _setError(ccontrol::MSG_RESULT_ERROR, _error.getMsg());
    }
}

}  // namespace lsst::qserv::ccontrol
