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

/**
  * @file
  *
  * @brief QueryRequest. XrdSsiRequest impl for czar query dispatch
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "qdisp/QdispPool.h"
#include "qdisp/QueryRequest.h"

// System headers
#include <cstddef>
#include <iostream>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "czar/Czar.h"
#include "global/LogContext.h"
#include "proto/ProtoHeaderWrap.h"
#include "proto/ScanTableInfo.h"
#include "qdisp/JobBase.h"
#include "qdisp/JobQuery.h"
#include "qdisp/JobStatus.h"
#include "qdisp/UberJob.h"
#include "qdisp/ResponseHandler.h"
#include "util/common.h"
#include "util/InstanceCount.h"
#include "util/Timer.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.QueryRequest");
}

namespace lsst {
namespace qserv {
namespace qdisp {


// Run action() when the system expects to have time to accept data.
class QueryRequest::AskForResponseDataCmd : public PriorityCommand {
public:
    typedef shared_ptr<AskForResponseDataCmd> Ptr;
    enum class State { STARTED0, DATAREADY1, DONE2 };
    AskForResponseDataCmd(QueryRequest::Ptr const& qr, JobBase::Ptr const& jq, size_t bufferSize)
        : _qRequest(qr), _jBase(jq), _qid(jq->getQueryId()), _jobid(jq->getIdInt()),
          _bufPtr(new vector<char>(bufferSize)) {
    }


    void action(util::CmdData *data) override {
        // If everything is ok, call GetResponseData to have XrdSsi ask the worker for the data.
        QSERV_LOGCONTEXT_QUERY_JOB(_qid, _jobid);
        //util::InstanceCount ica("AskFor_action&&&");
        util::Timer tWaiting;
        util::Timer tTotal;
        //LOGS(_log, LOG_LVL_INFO, "&&& AskForResponse start");
        {
            util::InstanceCount icb("AskFor_A&&&");
            tTotal.start();
            auto jq = _jBase.lock();
            auto qr = _qRequest.lock();
            if (jq == nullptr || qr == nullptr) {
                LOGS(_log, LOG_LVL_WARN, "AskForResp null before GetResponseData");
                // No way to call _errorFinish().
                _setState(State::DONE2);
                return;
            }

            if (qr->isQueryCancelled()) {
                LOGS(_log, LOG_LVL_DEBUG, "AskForResp query was cancelled");
                qr->_errorFinish(true);
                _setState(State::DONE2);
                return;
            }
            vector<char>& buffer = *_bufPtr;
            LOGS(_log, LOG_LVL_INFO, "AskForResp GetResponseData size=" << buffer.size()); // &&& revert to debug
            tWaiting.start();
            qr->GetResponseData(&buffer[0], buffer.size());
            LOGS(_log, LOG_LVL_INFO, "&&& AskForResp GetResponseData called");
        }

        // Wait for XrdSsi to call ProcessResponseData with the data,
        // which will notify this wait with a call to receivedProcessResponseDataParameters.
        {
            //util::InstanceCount icc("AskFor_B&&&");
            unique_lock<mutex> uLock(_mtx);
            // TODO: make timed wait, check for wedged, if weak pointers dead, log and give up.
            // Hoping for  _state == DATAREADY1, waiting for call to QueryRequest::ProcessResponseData
            util::InstanceCount icc1("AskFor_B1&&&");
            _cv.wait(uLock, [this](){ return _state != State::STARTED0; });
            util::InstanceCount icc2("AskFor_B2&&&");
            tWaiting.stop();
            // _mtx is locked at this point.
            LOGS(_log, LOG_LVL_INFO, "AskForResp should be DATAREADY1 " << (int)_state); // &&& revert to TRACE
            if (_state == State::DONE2) {
                // There was a problem. End the stream associated
                auto qr = _qRequest.lock();
                if (qr != nullptr) {
                    qr->_errorFinish();
                }
                LOGS(_log, LOG_LVL_INFO, "AskForResp returning early");
                return;
            }
        }

        // Actually process the data.
        // If more data needs to be sent, _processData will make a new AskForResponseDataCmd
        // object and queue it.
        {
            //util::InstanceCount icd("AskFor_C&&&");
            auto jq = _jBase.lock();
            auto qr = _qRequest.lock();
            if (jq == nullptr || qr == nullptr) {
                _setState(State::DONE2);
                LOGS(_log, LOG_LVL_WARN, "AskForResp null before processData");
                return;
            }
            LOGS(_log, LOG_LVL_INFO, "&&& AskForResp _processData a");
            qr->_processData(jq, _blen, _last);
            // _processData will have created another AskForResponseDataCmd object if was needed.
            LOGS(_log, LOG_LVL_INFO, "&&& AskForResp _processData b");
            tTotal.stop();
        }
        _setState(State::DONE2);
        LOGS(_log, LOG_LVL_INFO, "Ask data is done wait=" << tWaiting.getElapsed() << // &&& revert to debug
                " total=" << tTotal.getElapsed());
    }

    void notifyDataSuccess(int blen, bool last) {
        {
            lock_guard<mutex> lg(_mtx);
            _blen = blen;
            _last = last;
            _state = State::DATAREADY1;
        }
        LOGS(_log, LOG_LVL_INFO, "&&& notifyDataSuccess blen=" << blen << " last=" << last);
        _cv.notify_all();
    }

    void notifyFailed() {
        LOGS(_log, LOG_LVL_INFO, "notifyFailed");
        _setState(State::DONE2);
        _cv.notify_all();
    }

    State getState() const {
        lock_guard<mutex> lg(_mtx);
        return _state;
    }

    ResponseHandler::BufPtr getBufPtr() { return _bufPtr; }

private:
    void _setState(State const state) {
        lock_guard<mutex> lg(_mtx);
        _state = State::DONE2;
    }


    weak_ptr<QueryRequest> _qRequest;
    weak_ptr<JobBase> _jBase;
    QueryId _qid;
    int _jobid;
    mutable mutex _mtx;
    condition_variable _cv;
    State _state = State::STARTED0;

    ResponseHandler::BufPtr _bufPtr;

    int _blen = -1;
    bool _last = true;
    //&&&util::InstanceCount _instCount{"AskForResponseDataCmd"};
};


////////////////////////////////////////////////////////////////////////
// QueryRequest
////////////////////////////////////////////////////////////////////////
QueryRequest::QueryRequest(JobBase::Ptr const& job) :
  _job(job),
  _qid(job->getQueryId()),
  _jobid(job->getIdInt()),
  _jobIdStr(job->getIdStr()),
  _qdispPool(_job->getQdispPool()){
    QSERV_LOGCONTEXT_QUERY_JOB(_qid, _jobid);
    LOGS(_log, LOG_LVL_TRACE, "New QueryRequest");
}

QueryRequest::~QueryRequest() {
    QSERV_LOGCONTEXT_QUERY_JOB(_qid, _jobid);
    LOGS(_log, LOG_LVL_TRACE, "~QueryRequest");
    if (_askForResponseDataCmd != nullptr) {
        // This shouldn't really happen, but we really don't want to leave this blocking the pool.
        LOGS(_log, LOG_LVL_WARN, "~QueryRequest cleaning up _askForResponseDataCmd");
        _askForResponseDataCmd->notifyFailed();
    }
    if (!_finishedCalled) {
        LOGS(_log, LOG_LVL_WARN, "~QueryRequest cleaning up calling Finished");
        Finished(true);
    }
}

// content of request data
char* QueryRequest::GetRequest(int& requestLength) {
    QSERV_LOGCONTEXT_QUERY_JOB(_qid, _jobid);
    lock_guard<mutex> lock(_finishStatusMutex);
    auto jq = _job;
    if (_finishStatus != ACTIVE || jq == nullptr) {
        LOGS(_log, LOG_LVL_DEBUG, "QueryRequest::GetRequest called after job finished (cancelled?)");
        requestLength = 0;
        return const_cast<char*>("");
    }
    requestLength = jq->getPayload().size();
    LOGS(_log, LOG_LVL_DEBUG, "Requesting, payload size: " << requestLength);
    // Andy promises that his code won't corrupt it.
    return const_cast<char*>(jq->getPayload().data());
}

// precondition: rInfo.rType != isNone
// Must not throw exceptions: calling thread cannot trap them.
// Callback function for XrdSsiRequest.
//
bool QueryRequest::ProcessResponse(XrdSsiErrInfo  const& eInfo, XrdSsiRespInfo const& rInfo) {
    QSERV_LOGCONTEXT_QUERY_JOB(_qid, _jobid);
    LOGS(_log, LOG_LVL_DEBUG, "workerName=" << GetEndPoint() << " ProcessResponse");
    string errorDesc = _jobIdStr + " ";
    if (isQueryCancelled()) {
        LOGS(_log, LOG_LVL_WARN, "QueryRequest::ProcessResponse job already cancelled");
        cancel(); // calls _errorFinish()
        return true;
    }

    // Make a copy of the _jobQuery shared_ptr in case _jobQuery gets reset by a call to  cancel()
    auto jq = _job;
    {
        lock_guard<mutex> lock(_finishStatusMutex);
        if ((_finishStatus != ACTIVE) || (jq == nullptr)) {
            LOGS(_log, LOG_LVL_WARN,
                 "QueryRequest::GetRequest called after job finished (cancelled?)");
            return true;
        }
    }
    if (eInfo.hasError()) {
        ostringstream os;
        os << _jobIdStr << "ProcessResponse request failed "
           << getSsiErr(eInfo, nullptr) << " " << GetEndPoint();
        jq->getRespHandler()->errorFlush(os.str(), -1);
        jq->getStatus()->updateInfo(_jobIdStr, JobStatus::RESPONSE_ERROR);
        _errorFinish();
        return true;
    }

    switch(rInfo.rType) {
    case XrdSsiRespInfo::isNone: // All responses are non-null right now
        errorDesc += "Unexpected XrdSsiRespInfo.rType == isNone";
        break;
    case XrdSsiRespInfo::isData: // Local-only for Mock tests!
        if (string(rInfo.buff, rInfo.blen) == "MockResponse") {
           jq->getStatus()->updateInfo(_jobIdStr, JobStatus::COMPLETE);
           _finish();
           return true;
        }
        errorDesc += "Unexpected XrdSsiRespInfo.rType == isData";
        break;
    case XrdSsiRespInfo::isError:
        jq->getStatus()->updateInfo(_jobIdStr, JobStatus::RESPONSE_ERROR,
                                    rInfo.eNum, string(rInfo.eMsg));
        return _importError(string(rInfo.eMsg), rInfo.eNum);
    case XrdSsiRespInfo::isFile: // Local-only
        errorDesc += "Unexpected XrdSsiRespInfo.rType == isFile";
        break;
    case XrdSsiRespInfo::isStream: // All remote requests
        jq->getStatus()->updateInfo(_jobIdStr, JobStatus::RESPONSE_READY);
        return _importStream(jq);
    default:
        errorDesc += "Out of range XrdSsiRespInfo.rType";
    }
    return _importError(errorDesc, -1);
}


/// Retrieve and process results in using the XrdSsi stream mechanism
/// Uses a copy of JobQuery::Ptr instead of _jobQuery as a call to cancel() would reset _jobQuery.
bool QueryRequest::_importStream(JobBase::Ptr const& jq) {
    //util::InstanceCount ic("QR:_importStream&&&");
    if (_askForResponseDataCmd != nullptr) {
        LOGS(_log, LOG_LVL_ERROR, "_importStream There's already an _askForResponseDataCmd object!!");
        // Keep the previous object from wedging the pool.
        _askForResponseDataCmd->notifyFailed();
    }

    // Get the first header from metadata.
    int expectedLen = proto::ProtoHeaderWrap::getProtoHeaderSize();
    int len = expectedLen;
    const char* buff = GetMetadata(len);
    if (len != expectedLen) {
        throw Bug("_importStream wrong header size=" + to_string(len) + " expected=" + to_string(expectedLen));
    }
    ResponseHandler::BufPtr bufPtr = make_shared<vector<char>>(len);
    memcpy(&(*bufPtr)[0], buff, len);

    // Use flush to read the buffer and extract the header.
    bool largeResult = false;
    int nextBufSize = 0;
    bool last = false;
    //LOGS(_log, LOG_LVL_INFO, "&&& flush meta start");
    bool flushOk = jq->getRespHandler()->flush(len, bufPtr, last, largeResult, nextBufSize);
    //LOGS(_log, LOG_LVL_INFO, "&&& flush meta end");

    if (!flushOk) {
        LOGS(_log, LOG_LVL_ERROR, "_importStream not flushOk");
        return false;
    }

    if (!last) {
        LOGS(_log, LOG_LVL_INFO, "&&& _askForResponseDataCmd nextBufSize=" << nextBufSize);
        _askForResponseDataCmd = make_shared<AskForResponseDataCmd>(shared_from_this(), jq, nextBufSize);
        _queueAskForResponse(_askForResponseDataCmd, jq, true);
    } else {
        // This really shouldn't happen with the first header, even errors should have one result.
        LOGS(_log, LOG_LVL_ERROR, "last true for metadata");
        return false;
    }
    return true;
}


void QueryRequest::_queueAskForResponse(AskForResponseDataCmd::Ptr const& cmd, JobBase::Ptr const& jq, bool initialRequest) {
    // Interactive queries have highest priority.
    if (jq->getScanInteractive()) {
        _qdispPool->queCmd(cmd, 0);
    } else {
        // Trying to get existing requests done before doing new ones.
        if (initialRequest) {
            _qdispPool->queCmd(cmd, 3);
        } else {
            _qdispPool->queCmd(cmd, 2);
        }
    }
}

/// Process an incoming error.
bool QueryRequest::_importError(string const& msg, int code) {
    auto jq = _job;
    {
        lock_guard<mutex> lock(_finishStatusMutex);
        if (_finishStatus != ACTIVE || jq == nullptr) {
            LOGS(_log, LOG_LVL_WARN, "QueryRequest::_importError code=" << code
                      << " msg=" << msg << " not passed");
            return false;
        }
        jq->getRespHandler()->errorFlush(msg, code);
    }
    _errorFinish();
    return true;
}


void QueryRequest::_setHoldState(HoldState state) {
    if (state != _holdState) {
        LOGS(_log, LOG_LVL_DEBUG, "holdState changed from " << _holdState << " to " << state);
    }
    _holdState = state;
}


void QueryRequest::ProcessResponseData(XrdSsiErrInfo const& eInfo,
                                       char *buff, int blen, bool last) { // Step 7
    QSERV_LOGCONTEXT_QUERY_JOB(_qid, _jobid);
    util::InstanceCount ic("QR::ProcessResponseData&&&");
    // buff is ignored here. It points to jq->getDescription()->respHandler()->_mBuf, which
    // is accessed directly by the respHandler. _mBuf is a member of MergingHandler.
    LOGS(_log, LOG_LVL_DEBUG, "ProcessResponseData with buflen=" << blen
                              << " " << (last ? "(last)" : "(more)"));
    if (_askForResponseDataCmd == nullptr) {
        LOGS(_log, LOG_LVL_ERROR,
             "ProcessResponseData called with invalid _askForResponseDataCmd!!!");
        return;
    }

    // Work with a copy of _jobQuery so it doesn't get reset underneath us by a call to cancel().
    auto jq = _job;
    bool jobFinished = false;
    {
        lock_guard<mutex> lock(_finishStatusMutex);
        jobFinished = _finishStatus != ACTIVE || jq == nullptr;
    }
    if (jobFinished) {
        LOGS(_log, LOG_LVL_INFO, "ProcessResponseData job is inactive.");
        // This job is already dead.
        _errorFinish();
        return;
    }
    if (jq->isQueryCancelled()) {
        LOGS(_log, LOG_LVL_INFO, "ProcessResponseData job is cancelled.");
        _errorFinish(true);
        return;
    }

    // If there's an error, it makes sense to handle it immediately.
    if (blen < 0) { // error, check errinfo object.
        int eCode;
        string reason(getSsiErr(eInfo, &eCode) + " " + GetEndPoint());
        jq->getStatus()->updateInfo(_jobIdStr, JobStatus::RESPONSE_DATA_NACK, eCode, reason);
        LOGS(_log, LOG_LVL_ERROR, "ProcessResponse[data] error(" << eCode
             << " " << reason << ")");
        jq->getRespHandler()->errorFlush(
            "Couldn't retrieve response data:" + reason + " " + _jobIdStr, eCode);

        // Let the AskForResponseDataCmd end.
        _askForResponseDataCmd->notifyFailed();
        _errorFinish();
        // An error occurred, let processing continue so it can be cleaned up soon.
        return;
    }

    jq->getStatus()->updateInfo(_jobIdStr, JobStatus::RESPONSE_DATA);

    // Handle the response in a separate thread so we can give this one back to XrdSsi.
    // _askForResponseDataCmd should call QueryRequest::_processData() next.
    _askForResponseDataCmd->notifyDataSuccess(blen, last);
}


void QueryRequest::_processData(JobBase::Ptr const& jq, int blen, bool xrdLast) {
    //util::InstanceCount ic("QR::_processData&&&");
    // It's possible jq and _jobQuery differ, so need to use jq.
    if (jq->isQueryCancelled()) {
        LOGS(_log, LOG_LVL_WARN, "QueryRequest::_processData job was cancelled.");
        _errorFinish(true);
        return;
    }

    // Get a copy of the shared buffer pointer so askForResponseDataCmd can be deleted.
    ResponseHandler::BufPtr bufPtr = _askForResponseDataCmd->getBufPtr();
    _askForResponseDataCmd.reset(); // No longer need it, and don't want the destructor calling _errorFinish().

    int const protoHeaderSize = proto::ProtoHeaderWrap::getProtoHeaderSize();
    ResponseHandler::BufPtr nextHeaderBufPtr;

    // Values for these variables to be filled in by flush() calls.
    bool largeResult = false;
    int nextBufSize = 0;
    bool last = false;

    bool flushOk = false;

    // The buffer has 2 parts.
    // - The first (bytes = blen - ProtoHeaderWrap::getProtheaderSize())
    //   is the result associated with the previously received header.
    // - The second is the header for the next message.
    int respSize = blen - protoHeaderSize;
    nextHeaderBufPtr = make_shared<vector<char>>(bufPtr->begin() + respSize, bufPtr->end());
    // Read the result
    // Values for last, largeResult, and nextBufSize filled in by flush
    //LOGS(_log, LOG_LVL_INFO, "&&& flush a start");
    flushOk = jq->getRespHandler()->flush(respSize, bufPtr, last, largeResult, nextBufSize);
    //LOGS(_log, LOG_LVL_INFO, "&&& flush a end");
    if (last) {
        // Last should only be true when the header is read, not the result.
        throw Bug("_processData result had 'last' true, which cannot be allowed.");
    }

    bufPtr.reset(); // don't need the buffer anymore and it could be big.
    if (nextBufSize != protoHeaderSize) {
        throw Bug("Unexpected header size from flush(result) call QID="
                + to_string(_qid) + "#" + to_string(_jobid));
    }
    if (!flushOk) {
        _flushError(jq);
        return;
    }

    // Read the next header
    // Values for last, largeResult, and nextBufSize filled in by flush
    //LOGS(_log, LOG_LVL_INFO, "&&& flush b start");
    flushOk = jq->getRespHandler()->flush(protoHeaderSize, nextHeaderBufPtr, last, largeResult, nextBufSize);
    LOGS(_log, LOG_LVL_INFO, "&&& flush b end");
    if (largeResult) {
        if (!_largeResult) LOGS(_log, LOG_LVL_DEBUG, "holdState largeResult set to true");
        _largeResult = true; // Once the worker indicates it's a large result, it stays that way.
    }

    if (flushOk) {
        if (last != xrdLast) {
            LOGS(_log, LOG_LVL_DEBUG, "processData disagreement between last=" << last
                                     << " and xrdLast=" << xrdLast);
        }
        if (last) {
            jq->getStatus()->updateInfo(_jobIdStr, JobStatus::COMPLETE);
            _finish();
            // At this point all blocks for this job have been read, there's no point in
            // having XrdSsi wait for anything.
            return;
        } else {
            _askForResponseDataCmd = make_shared<AskForResponseDataCmd>(shared_from_this(), jq, nextBufSize);
            LOGS(_log, LOG_LVL_DEBUG, "queuing askForResponseDataCmd bufSize=" << nextBufSize);
            _queueAskForResponse(_askForResponseDataCmd, jq, false);
        }
    } else {
        LOGS(_log, LOG_LVL_WARN, "flushOk = false");
        _flushError(jq);
        return;
    }
    return;
}


void QueryRequest::_flushError(JobBase::Ptr const& jq) {
    ResponseHandler::Error err = jq->getRespHandler()->getError();
    jq->getStatus()->updateInfo(_jobIdStr, JobStatus::MERGE_ERROR, err.getCode(), err.getMsg());
    // This error can be caused by errors in the SQL
    _retried = true; // Do not retry
    _errorFinish(true);
}


/// @return true if QueryRequest cancelled successfully.
bool QueryRequest::cancel() {
    LOGS(_log, LOG_LVL_DEBUG, "QueryRequest::cancel");
    {
        lock_guard<mutex> lock(_finishStatusMutex);
        if (_cancelled) {
            LOGS(_log, LOG_LVL_DEBUG, "QueryRequest::cancel already cancelled, ignoring");
            return false; // Don't do anything if already cancelled.
        }
        _cancelled = true;
        _retried.store(true); // Prevent retries.
        // Only call the following if the job is NOT already done.
        if (_finishStatus == ACTIVE) {
            auto jq = _job;
            if (jq != nullptr) jq->getStatus()->updateInfo(_jobIdStr, JobStatus::CANCEL);
        }
    }
    return _errorFinish(true); // return true if errorFinish cancelled
}


/// @return true if this object's JobQuery, or its Executive has been cancelled.
/// It takes time for the Executive to flag all jobs as being cancelled
bool QueryRequest::isQueryCancelled() {
    auto jq = _job;
    if (jq == nullptr) {
        // Need to check if _jobQuery is null due to cancellation.
        return isQueryRequestCancelled();
    }
    return jq->isQueryCancelled();
}


/// @return true if QueryRequest::cancel() has been called.
/// QueryRequest::isQueryCancelled() is a much better indicator of user query cancellation.
bool QueryRequest::isQueryRequestCancelled() {
    lock_guard<mutex> lock(_finishStatusMutex);
    return _cancelled;
}


/// Cleanup pointers so this class can be deleted.
/// This should only be called by _finish or _errorFinish.
void QueryRequest::cleanup() {
    LOGS(_log, LOG_LVL_TRACE, "QueryRequest::cleanup()");
    {
        lock_guard<mutex> lock(_finishStatusMutex);
        if (_finishStatus == ACTIVE) {
            LOGS(_log, LOG_LVL_ERROR, "QueryRequest::cleanup called before _finish or _errorFinish");
            return;
        }
    }

    // These need to be outside the mutex lock, or you could delete
    // _finishStatusMutex before it is unlocked.
    // This should reset _jobquery and _keepAlive without risk of either being deleted
    // before being reset.
    shared_ptr<JobBase> jq(move(_job));
    shared_ptr<QueryRequest> keep(move(_keepAlive));
}


/// Finalize under error conditions and retry or report completion
/// THIS FUNCTION WILL RESULT IN THIS OBJECT BEING DESTROYED, UNLESS there is
/// a local shared pointer for this QueryRequest and/or its owner JobQuery.
/// See QueryRequest::cleanup()
/// @return true if this QueryRequest object had the authority to make changes.
bool QueryRequest::_errorFinish(bool shouldCancel) {
    LOGS(_log, LOG_LVL_DEBUG, "_errorFinish() shouldCancel=" << shouldCancel);

    auto jbase = _job;
    JobQuery::Ptr jq = dynamic_pointer_cast<JobQuery>(jbase);
    if (jq == nullptr) {
        //&&& TODO:UJ THIS NEEDS WORK - UberJob failures are different than JobQuery failures.
        UberJob::Ptr uberJob = dynamic_pointer_cast<UberJob>(jbase);
        if (uberJob != nullptr) {
            throw Bug("&&&NEED_CODE for _errorFinish to work correctly with UberJob");
            // UberJobs breakup into their JobQueries when they fail and run the jobs directly.
        }
        return false;
    }

    // Normal JobQuery error handling.
    {
        // Running _errorFinish more than once could cause errors.
        lock_guard<mutex> lock(_finishStatusMutex);
        if (_finishStatus != ACTIVE || jq == nullptr) {
            // Either _finish or _errorFinish has already been called.
            LOGS_DEBUG("_errorFinish() job no longer ACTIVE, ignoring "
                       << " _finishStatus=" << _finishStatus
                       << " ACTIVE=" << ACTIVE << " jq=" << jq);
            return false;
        }
        _finishStatus = ERROR;
    }

    // Make the calls outside of the mutex lock.
    LOGS(_log, LOG_LVL_DEBUG, "calling Finished(shouldCancel=" << shouldCancel << ")");
    bool ok = Finished(shouldCancel);
    _finishedCalled = true;
    if (!ok) {
        LOGS(_log, LOG_LVL_ERROR,  "QueryRequest::_errorFinish !ok ");
    } else {
        LOGS(_log, LOG_LVL_DEBUG, "QueryRequest::_errorFinish ok");
    }

    if (!_retried.exchange(true) && !shouldCancel) {
        // There's a slight race condition here. _jobQuery::runJob() creates a
        // new QueryRequest object which will replace this one in _jobQuery.
        // The replacement could show up before this one's cleanup() is called,
        // so this will keep this alive.
        LOGS(_log, LOG_LVL_DEBUG, "QueryRequest::_errorFinish retrying");
        _keepAlive = jq->getQueryRequest(); // shared pointer to this
        if (!jq->runJob()) {
            // Retry failed, nothing left to try.
            LOGS(_log, LOG_LVL_DEBUG, "errorFinish retry failed");
            _callMarkComplete(false);
        }
    } else {
        _callMarkComplete(false);
    }
    cleanup(); // Reset smart pointers so this object can be deleted.
    return true;
}

/// Finalize under success conditions and report completion.
/// THIS FUNCTION WILL RESULT IN THIS OBJECT BEING DESTROYED, UNLESS there is
/// a local shared pointer for this QueryRequest and/or its owner JobQuery.
/// See QueryRequest::cleanup()
void QueryRequest::_finish() {
    LOGS(_log, LOG_LVL_TRACE, "QueryRequest::_finish");
    {
        // Running _finish more than once would cause errors.
        lock_guard<mutex> lock(_finishStatusMutex);
        if (_finishStatus != ACTIVE) {
            // Either _finish or _errorFinish has already been called.
            LOGS(_log, LOG_LVL_WARN, "QueryRequest::_finish called when not ACTIVE, ignoring");
            return;
        }
        _finishStatus = FINISHED;
    }

    bool ok = Finished();
    _finishedCalled = true;
    if (!ok) {
        LOGS(_log, LOG_LVL_ERROR, "QueryRequest::finish Finished() !ok ");
    } else {
        LOGS(_log, LOG_LVL_DEBUG, "QueryRequest::finish Finished() ok.");
    }
    _callMarkComplete(true);
    cleanup();
}

/// Inform the Executive that this query completed, and
// Call MarkCompleteFunc only once, it should only be called from _finish() or _errorFinish.
void QueryRequest::_callMarkComplete(bool success) {
    if (!_calledMarkComplete.exchange(true)) {
        auto jq = _job;
        if (jq != nullptr) {
            jq->callMarkCompleteFunc(success);
        }
    }
}

ostream& operator<<(ostream& os, QueryRequest const& qr) {
    os << "QueryRequest " << qr._jobIdStr;
    return os;
}


/// @return The error text and code that SSI set.
/// if eCode != nullptr, it is set to the error code set by SSI.
string QueryRequest::getSsiErr(XrdSsiErrInfo const& eInfo, int* eCode) {
    int errNum;
    string errText = eInfo.Get(errNum);
    if (eCode != nullptr) {
        *eCode = errNum;
    }
    ostringstream os;
    os << "SSI_Error(" << errNum << ":" << errText << ")";
    return os.str();
}

}}} // lsst::qserv::qdisp
