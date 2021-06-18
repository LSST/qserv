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
#include <cassert>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/msgCode.h"
#include "global/Bug.h"
#include "global/debugUtil.h"
#include "global/MsgReceiver.h"
#include "proto/ProtoHeaderWrap.h"
#include "proto/ProtoImporter.h"
#include "proto/WorkerResponse.h"
#include "qdisp/JobQuery.h"
#include "rproc/InfileMerger.h"
#include "util/common.h"
#include "util/StringHash.h"

using lsst::qserv::proto::ProtoImporter;
using lsst::qserv::proto::ProtoHeader;
using lsst::qserv::proto::Result;
using lsst::qserv::proto::WorkerResponse;

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.MergingHandler");
}


namespace lsst {
namespace qserv {
namespace ccontrol {


////////////////////////////////////////////////////////////////////////
// MergingHandler public
////////////////////////////////////////////////////////////////////////
MergingHandler::MergingHandler(
    std::shared_ptr<MsgReceiver> msgReceiver,
    std::shared_ptr<rproc::InfileMerger> merger,
    std::string const& tableName)
    : _msgReceiver{msgReceiver}, _infileMerger{merger}, _tableName{tableName},
      _response{new WorkerResponse()} {
    _initState();
}

MergingHandler::~MergingHandler() {
    LOGS(_log, LOG_LVL_DEBUG, "~MergingHandler()");
}

const char* MergingHandler::getStateStr(MsgState const& state) {
    switch(state) {
    case MsgState::HEADER_WAIT: return "HEADER_WAIT";
    case MsgState::RESULT_WAIT: return "RESULT_WAIT";
    case MsgState::RESULT_RECV: return "RESULT_RECV";
    case MsgState::HEADER_ERR:  return "HEADER_ERR";
    case MsgState::RESULT_ERR:  return "RESULT_ERR";
    }
    return "unknown";
}


bool MergingHandler::flush(int bLen, BufPtr const& bufPtr, bool& last, bool& largeResult,
                           int& nextBufSize) {
    LOGS(_log, LOG_LVL_DEBUG, "From:" << _wName << " flush state="
         << getStateStr(_state) << " blen=" << bLen << " last=" << last);

    if (bLen < 0) {
        throw Bug("MergingHandler invalid blen=" + to_string(bLen) + " from " + _wName);
    }

    switch(_state) {
    case MsgState::HEADER_WAIT:
        _response->headerSize = static_cast<unsigned char>((*bufPtr)[0]);
        if (!proto::ProtoHeaderWrap::unwrap(_response, *bufPtr)) {
            std::string sErr = "From:" + _wName + "Error decoding proto header for " + getStateStr(_state);
            _setError(ccontrol::MSG_RESULT_DECODE, sErr);
            _state = MsgState::HEADER_ERR;
            return false;
        }
        if (_wName == "~") {
            _wName = _response->protoHeader.wname();
        }

        {
            nextBufSize    = _response->protoHeader.size();
            largeResult    = _response->protoHeader.largeresult();
            bool endNoData = _response->protoHeader.endnodata();
            LOGS(_log, LOG_LVL_DEBUG, "HEADER_SIZE_WAIT: From:" << _wName
                << " nextBufSize=" << nextBufSize << " largeResult=" << largeResult
                << " endNoData=" << endNoData);

            _state = MsgState::RESULT_WAIT;
            if (endNoData || nextBufSize == 0) {
                if (!endNoData || nextBufSize != 0 ) {
                    throw Bug("inconsistent msg termination endNoData=" + std::to_string(endNoData)
                    + " nextBufSize=" + std::to_string(nextBufSize));
                }
                // Nothing to merge, but some bookkeeping needs to be done.
                _infileMerger->mergeCompleteFor(_jobIds);
                last = true;
                _state = MsgState::RESULT_RECV;
            }
        }
        return true;
    case MsgState::RESULT_WAIT:
        {
            nextBufSize = proto::ProtoHeaderWrap::getProtoHeaderSize();
            auto job = getJobBase().lock();
            if (!_verifyResult(bufPtr, bLen)) { return false; }
            if (!_setResult(bufPtr, bLen)) { // This sets _response->result
                LOGS(_log, LOG_LVL_WARN, "setResult failure " << _wName);
                return false;
            }
            LOGS(_log, LOG_LVL_DEBUG, "From:" << _wName << " _mBuf " << util::prettyCharList(*bufPtr, 5));
            _state = MsgState::HEADER_WAIT;

            int jobId = _response->result.jobid();
            _jobIds.insert(jobId);
            LOGS(_log, LOG_LVL_DEBUG, "Flushed last=" << last << " for tableName=" << _tableName);

            auto success = _merge();
            _response.reset(new WorkerResponse());
            return success;
        }
    case MsgState::RESULT_RECV:
        // We shouldn't wind up here. _buffer.size(0) and last=true should end communication.
        [[fallthrough]];
    case MsgState::HEADER_ERR:
        [[fallthrough]];
    case MsgState::RESULT_ERR:
         {
            std::ostringstream eos;
            eos << "Unexpected message From:" << _wName << " flush state="
                << getStateStr(_state) << " last=" << last;
            LOGS(_log, LOG_LVL_ERROR, eos.str());
            _setError(ccontrol::MSG_RESULT_ERROR, eos.str());
         }
        return false;
    default:
        break;
    }
    _setError(ccontrol::MSG_RESULT_ERROR, "Unexpected message (invalid)");
    return false;
}


void MergingHandler::errorFlush(std::string const& msg, int code) {
    _setError(code, msg);
    // Might want more info from result service.
    // Do something about the error. FIXME.
    LOGS(_log, LOG_LVL_ERROR, "Error receiving result.");
}

bool MergingHandler::finished() const {
    return _flushed;
}

bool MergingHandler::reset() {
    // If we've pushed any bits to the merger successfully, we have to undo them
    // to reset to a fresh state. For now, we will just fail if we've already
    // begun merging. If we implement the ability to retract a partial result
    // merge, then we can use it and do something better.
    if (_flushed) {
        return false; // Can't reset if we have already pushed state.
    }
    _initState();
    return true;
}

// Note that generally we always have an _infileMerger object except during
// a unit test. I suppose we could try to figure out how to create one.
//
void MergingHandler::prepScrubResults(int jobId, int attemptCount) {
    if (_infileMerger) _infileMerger->prepScrub(jobId, attemptCount);
}


std::ostream& MergingHandler::print(std::ostream& os) const {
    return os << "MergingRequester(" << _tableName << ", flushed="
              << (_flushed ? "true)" : "false)") ;
}
////////////////////////////////////////////////////////////////////////
// MergingRequester private
////////////////////////////////////////////////////////////////////////

void MergingHandler::_initState() {
    _state = MsgState::HEADER_WAIT;
    _setError(0, "");
}

bool MergingHandler::_merge() {
    auto job = getJobBase().lock();
    if (job != nullptr) {
        if (_flushed) {
            throw Bug("MergingRequester::_merge : already flushed");
        }
        bool success = _infileMerger->merge(_response);
        if (!success) {
            LOGS(_log, LOG_LVL_WARN, "_merge() failed");
            rproc::InfileMergerError const& err = _infileMerger->getError();
            _setError(ccontrol::MSG_RESULT_ERROR, err.getMsg());
            _state = MsgState::RESULT_ERR;
        }
        _response.reset();
        return success;
    }
    LOGS(_log, LOG_LVL_ERROR, "MergingHandler::_merge() failed, jobQuery was NULL");
    return false;
}

void MergingHandler::_setError(int code, std::string const& msg) {
    LOGS(_log, LOG_LVL_DEBUG, "_setErr: code: " << code << ", message: " << msg);
    std::lock_guard<std::mutex> lock(_errorMutex);
    _error = Error(code, msg);
}

bool MergingHandler::_setResult(BufPtr const& bufPtr, int blen) {
    auto start = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lg(_setResultMtx);
    auto& buf = *bufPtr;
    if (!ProtoImporter<proto::Result>::setMsgFrom(_response->result, &(buf[0]), blen)) {
        LOGS(_log, LOG_LVL_ERROR, "_setResult decoding error");
        _setError(ccontrol::MSG_RESULT_DECODE, "Error decoding result msg");
        _state = MsgState::RESULT_ERR;
        return false;
    }
    auto protoEnd = std::chrono::system_clock::now();
    auto protoDur = std::chrono::duration_cast<std::chrono::milliseconds>(protoEnd - start);
    LOGS(_log, LOG_LVL_DEBUG, "protoDur=" << protoDur.count());
    return true;
}

bool MergingHandler::_verifyResult(BufPtr const& bufPtr, int blen) {
    auto& buf = *bufPtr;
    if (_response->protoHeader.md5() != util::StringHash::getMd5(&(buf[0]), blen)) {
        LOGS(_log, LOG_LVL_ERROR, "_verifyResult MD5 mismatch");
        _setError(ccontrol::MSG_RESULT_MD5, "Result message MD5 mismatch");
        _state = MsgState::RESULT_ERR;
        return false;
    }
    return true;
}


}}} // lsst::qserv::ccontrol
