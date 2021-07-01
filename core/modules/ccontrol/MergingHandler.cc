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
    case MsgState::INVALID:          return "INVALID";
    case MsgState::HEADER_SIZE_WAIT: return "HEADER_SIZE_WAIT";
    case MsgState::RESULT_WAIT:      return "RESULT_WAIT";
    case MsgState::RESULT_RECV:      return "RESULT_RECV";
    case MsgState::RESULT_EXTRA:     return "RESULT_EXTRA";
    case MsgState::HEADER_ERR:       return "HEADER_ERR";
    case MsgState::RESULT_ERR:       return "RESULT_ERR";
    }
    return "unknown";
}

bool MergingHandler::flush(int bLen, BufPtr const& bufPtr, bool& last, bool& largeResult, int& nextBufSize, int& resultRows) {
    resultRows = 0;
    LOGS(_log, LOG_LVL_DEBUG, "From:" << _wName << " flush state="
         << getStateStr(_state) << " blen=" << bLen << " last=" << last);
    if ((bLen < 0) || (bLen != (int)bufPtr->size())) {
        if (_state != MsgState::RESULT_EXTRA) {
            LOGS(_log, LOG_LVL_ERROR, "MergingRequester size mismatch: expected " <<
                 bufPtr->size() << " got " << bLen);
            // Worker sent corrupted data, or there is some other error.
        }
    }
    switch(_state) {
    case MsgState::HEADER_SIZE_WAIT:
        _response->headerSize = static_cast<unsigned char>((*bufPtr)[0]);
        if (!proto::ProtoHeaderWrap::unwrap(_response, *bufPtr)) {
            std::string s = "From:" + _wName + "Error decoding proto header for " + getStateStr(_state);
            _setError(ccontrol::MSG_RESULT_DECODE, s);
            _state = MsgState::HEADER_ERR;
            return false;
        }
        if (_wName == "~") {
            _wName = _response->protoHeader.wname();
        }

        LOGS(_log, LOG_LVL_DEBUG, "HEADER_SIZE_WAIT: From:" << _wName
             << "Resizing buffer to " <<  _response->protoHeader.size());
        nextBufSize = _response->protoHeader.size();
        largeResult = _response->protoHeader.largeresult();
        _state = MsgState::RESULT_WAIT;

        return true;
    case MsgState::RESULT_WAIT:
        {
            auto jobQuery = getJobQuery().lock();
            if (!_verifyResult(bufPtr)) { return false; }
            if (!_setResult(bufPtr)) {
                LOGS(_log, LOG_LVL_WARN, "setResult failure " << _wName);
                return false;
            } // set _response->result
            largeResult = _response->result.largeresult();
            LOGS(_log, LOG_LVL_DEBUG, "From:" << _wName << " _mBuf " << util::prettyCharList(*bufPtr, 5));
            bool msgContinues = _response->result.continues();
            resultRows = _response->result.row_size();
            _state = MsgState::RESULT_RECV;
            if (msgContinues) {
                LOGS(_log, LOG_LVL_TRACE, "Message continues, waiting for next header.");
                _state = MsgState::RESULT_EXTRA;
                nextBufSize = proto::ProtoHeaderWrap::PROTO_HEADER_SIZE;
            } else {
                LOGS(_log, LOG_LVL_TRACE, "Message ends, setting last=true");
                last = true;
            }
            LOGS(_log, LOG_LVL_DEBUG, "Flushed msgContinues=" << msgContinues
                 << " last=" << last << " for tableName=" << _tableName);

            auto success = _merge(last);
            if (msgContinues) {
                _response.reset(new WorkerResponse());
            }
            return success;
        }
    case MsgState::RESULT_EXTRA:
        if (!proto::ProtoHeaderWrap::unwrap(_response, *bufPtr)) {
            _setError(ccontrol::MSG_RESULT_DECODE,
                      std::string("Error decoding proto header for ") + getStateStr(_state));
            _state = MsgState::HEADER_ERR;
            return false;
        }
        largeResult = _response->protoHeader.largeresult();
        LOGS(_log, LOG_LVL_DEBUG, "RESULT_EXTRA: Resizing buffer to "
             << _response->protoHeader.size() << " largeResult=" << largeResult);
        nextBufSize = _response->protoHeader.size();
        _state = MsgState::RESULT_WAIT;
        return true;
    case MsgState::RESULT_RECV:
        // We shouldn't wind up here. _buffer.size(0) and last=true should end communication.
        // fall-through
    case MsgState::HEADER_ERR:
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
    _state = MsgState::HEADER_SIZE_WAIT;
    _setError(0, "");
}

bool MergingHandler::_merge(bool last) {
    if (auto job = getJobQuery().lock()) {
        if (_flushed) {
            throw Bug("MergingRequester::_merge : already flushed");
        }
        /* &&&
        if (exec->isLimitRowComplete()) {
            LOGS(_log, LOG_LVL_INFO, "skipping merge, LIMIT result already ready");
            return true;
        }
        */
        bool success = _infileMerger->merge(_response, last);
        if (!success) {
            LOGS(_log, LOG_LVL_WARN, "_merge() failed");
            rproc::InfileMergerError const& err = _infileMerger->getError();
            _setError(ccontrol::MSG_RESULT_ERROR, err.getMsg());
            _state = MsgState::RESULT_ERR;
        }
        _response.reset();

        /* &&&
        // &&& the 'last' check may not be needed, but partial query
        // results could cause issues.
        if (last && success && _infileMerger->limitRowComplete()) {
            throw Bug("&&& NEED CODE to call Executive::setLimitRowComplete() and then squash()");
        }
        */
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

bool MergingHandler::_setResult(BufPtr const& bufPtr) {
    auto start = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lg(_setResultMtx);
    auto& buf = *bufPtr;
    if (!ProtoImporter<proto::Result>::setMsgFrom(_response->result, &(buf[0]), buf.size())) {
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

bool MergingHandler::_verifyResult(BufPtr const& bufPtr) {
    auto& buf = *bufPtr;
    if (_response->protoHeader.md5() != util::StringHash::getMd5(&(buf[0]), bufPtr->size())) {
        LOGS(_log, LOG_LVL_ERROR, "_verifyResult MD5 mismatch");
        _setError(ccontrol::MSG_RESULT_MD5, "Result message MD5 mismatch");
        _state = MsgState::RESULT_ERR;
        return false;
    }
    return true;
}


}}} // lsst::qserv::ccontrol
