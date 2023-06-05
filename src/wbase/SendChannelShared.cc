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
#include "wbase/SendChannelShared.h"

// System headers

// Qserv headers
#include "global/LogContext.h"
#include "proto/ProtoHeaderWrap.h"
#include "util/Bug.h"
#include "util/Error.h"
#include "util/HoldTrack.h"
#include "util/MultiError.h"
#include "util/Timer.h"
#include "wbase/Task.h"
#include "wcontrol/TransmitMgr.h"
#include "wpublish/QueriesAndChunks.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wbase.SendChannelShared");
}

namespace lsst::qserv::wbase {

atomic<uint64_t> SendChannelShared::scsSeqId{0};

SendChannelShared::Ptr SendChannelShared::create(SendChannel::Ptr const& sendChannel,
                                                 wcontrol::TransmitMgr::Ptr const& transmitMgr,
                                                 qmeta::CzarId czarId) {
    auto scs = shared_ptr<SendChannelShared>(new SendChannelShared(sendChannel, transmitMgr, czarId));
    return scs;
}

SendChannelShared::SendChannelShared(SendChannel::Ptr const& sendChannel,
                                     std::shared_ptr<wcontrol::TransmitMgr> const& transmitMgr,
                                     qmeta::CzarId czarId)
        : _sendChannel(sendChannel), _transmitMgr(transmitMgr), _czarId(czarId), _scsId(scsSeqId++) {
    if (_sendChannel == nullptr) {
        throw util::Bug(ERR_LOC, "SendChannelShared constructor given nullptr");
    }
}

SendChannelShared::~SendChannelShared() {
    if (_sendChannel != nullptr) {
        _sendChannel->setDestroying();
        if (!_sendChannel->isDead()) {
            _sendChannel->kill("~SendChannelShared()");
        }
    }
}

void SendChannelShared::setTaskCount(int taskCount) { _taskCount = taskCount; }

bool SendChannelShared::transmitTaskLast(bool inLast) {
    lock_guard<mutex> streamLock(_streamMutex);
    /// _caller must have locked _streamMutex before calling this.
    if (not inLast) return false;  // This wasn't the last message buffer for this task, so it doesn't matter.
    ++_lastCount;
    bool lastTaskDone = _lastCount >= _taskCount;
    return lastTaskDone;
}

bool SendChannelShared::_kill(std::string const& note) {
    LOGS(_log, LOG_LVL_DEBUG, "SendChannelShared::kill() called " << note);
    bool ret = _sendChannel->kill(note);
    _lastRecvd = true;
    return ret;
}

string SendChannelShared::makeIdStr(int qId, int jId) {
    string str("QID" + (qId == 0 ? "" : to_string(qId) + "#" + to_string(jId)));
    return str;
}

void SendChannelShared::_waitTransmitLock(bool interactive, QueryId const& qId) {
    if (_transmitLock != nullptr) {
        return;
    }

    {
        unique_lock<mutex> uLock(_transmitLockMtx);
        bool first = _firstTransmitLock.exchange(false);
        if (first) {
            // This will wait until TransmitMgr has resources available.
            _transmitLock.reset(new wcontrol::TransmitLock(*_transmitMgr, interactive, qId));
        } else {
            _transmitLockCv.wait(uLock, [this]() { return _transmitLock != nullptr; });
        }
    }
    _transmitLockCv.notify_one();
}

bool SendChannelShared::_addTransmit(Task::Ptr const& task, bool cancelled, bool erred, bool lastIn,
                                     TransmitData::Ptr const& tData, int qId, int jId) {
    QSERV_LOGCONTEXT_QUERY_JOB(qId, jId);
    assert(tData != nullptr);

    // This lock may be held for a very long time.
    std::unique_lock<std::mutex> qLock(_queueMtx);
    _transmitQueue.push(tData);

    // If _lastRecvd is true, the last message has already been transmitted and
    // this SendChannel is effectively dead.
    bool reallyLast = _lastRecvd;
    string idStr(makeIdStr(qId, jId));
    if (_icPtr == nullptr) {
        _icPtr = std::make_shared<util::InstanceCount>(std::to_string(qId) + "_SCS_LDB");
    }

    // If something bad already happened, just give up.
    if (reallyLast || isDead()) {
        // If there's been some kind of error, make sure that nothing hangs waiting
        // for this.
        LOGS(_log, LOG_LVL_WARN, "addTransmit getting messages after isDead or reallyLast " << idStr);
        _lastRecvd = true;
        return false;
    }

    // If lastIn is true, all tasks for this job have run to completion and
    // finished building their transmit messages.
    if (lastIn) {
        reallyLast = true;
    }
    if (reallyLast || erred || cancelled) {
        _lastRecvd = true;
        LOGS(_log, LOG_LVL_DEBUG,
             "addTransmit lastRecvd=" << _lastRecvd << " really=" << reallyLast << " erred=" << erred
                                      << " cancelled=" << cancelled);
    }

    return _transmit(erred, task);
}

bool SendChannelShared::_transmit(bool erred, Task::Ptr const& task) {
    string idStr = "QID?";

    // Result data is transmitted in messages containing data and headers.
    // data - is the result data
    // header - contains information about the next chunk of result data,
    //          most importantly the size of the next data message.
    //          The header has a fixed size (about 255 bytes)
    // header_END - indicates there will be no more msg.
    // msg - contains data and header.
    // metadata - special xrootd buffer that can only be set once per SendChannelShared
    //            instance. It is used to send the first header.
    // A complete set of results to the czar looks like
    //    metadata[header_A] -> msg_A[data_A, header_END]
    // or
    //    metadata[header_A] -> msg_A[data_A, header_B]
    //          -> msg_B[data_B, header_C] -> ... -> msg_X[data_x, header_END]
    //
    // Since you can't send msg_A until you know the size of data_B, you can't
    // transmit until there are at least 2 msg in the queue, or you know
    // that msg_A is the last msg in the queue.
    // Note that the order of result rows does not matter, but data_B must come after header_B.
    // Keep looping until nothing more can be transmitted.
    while (_transmitQueue.size() >= 2 || _lastRecvd) {
        TransmitData::Ptr thisTransmit = _transmitQueue.front();
        _transmitQueue.pop();
        if (thisTransmit == nullptr) {
            throw util::Bug(ERR_LOC, "_transmitLoop() _transmitQueue had nullptr!");
        }

        auto sz = _transmitQueue.size();
        // Is this really the last message for this SharedSendChannel?
        // _lastRecvd means no more data will be added to the queue and
        // if the queue size is zero, then `thisTransmit` is really the last one.
        bool reallyLast = (_lastRecvd && sz == 0);

        TransmitData::Ptr nextTr;
        if (sz != 0) {
            nextTr = _transmitQueue.front();
            if (nextTr->getResultSize() == 0) {
                LOGS(_log, LOG_LVL_ERROR,
                     "RESULT SIZE IS 0, this should not happen thisTr=" << thisTransmit->dump()
                                                                        << " nextTr=" << nextTr->dump());
            }
        }
        uint32_t seq = _sendChannel->getSeq();
        int scsSeq = ++_scsSeq;
        string seqStr = string("seq=" + to_string(seq) + " scsseq=" + to_string(scsSeq) +
                               " scsId=" + to_string(_scsId));
        thisTransmit->attachNextHeader(nextTr, reallyLast, seq, scsSeq);

        // The first message needs to put its header data in metadata as there's
        // no previous message it could attach its header to.
        {
            lock_guard<mutex> streamLock(_streamMutex);  // Must keep meta and buffer together.
            if (_firstTransmit.exchange(false)) {
                // Put the header for the first message in metadata
                // _metaDataBuf must remain valid until Finished() is called.
                std::string thisHeaderString = thisTransmit->getHeaderString(seq, scsSeq - 1);
                _metadataBuf = proto::ProtoHeaderWrap::wrap(thisHeaderString);
                bool metaSet = _sendChannel->setMetadata(_metadataBuf.data(), _metadataBuf.size());
                if (!metaSet) {
                    LOGS(_log, LOG_LVL_ERROR, "Failed to setMeta " << idStr);
                    _kill("metadata");
                    return false;
                }
            }

            // Put the data for the transmit in a StreamBuffer and send it.
            // Since the StreamBuffer's lifetime is beyond our control, it needs
            // its own Task pointer.
            auto streamBuf = thisTransmit->getStreamBuffer(task);
            streamBuf->startTimer();
            bool sent = _sendBuf(streamLock, streamBuf, reallyLast, "transmitLoop " + idStr + " " + seqStr,
                                 scsSeq);

            if (!sent) {
                LOGS(_log, LOG_LVL_ERROR, "Failed to send " << idStr);
                _kill("SendChannelShared::_transmit b");
                return false;
            }
        }
        // If that was the last message, break the loop.
        if (reallyLast) return true;
    }
    return true;
}

bool SendChannelShared::_sendBuf(lock_guard<mutex> const& streamLock, xrdsvc::StreamBuffer::Ptr& streamBuf,
                                 bool last, string const& note, int scsSeq) {
    bool sent = _sendChannel->sendStream(streamBuf, last, scsSeq);
    if (!sent) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to transmit " << note << "!");
        return false;
    } else {
        // Only wait if this is really the last message to finish transmitting for this SendChannelShared.
        // Note: If this does speed up transmission of data to the czar then it is probably worth it to
        //       add code to wait if more than X buffers are in use at the same time, which will likely
        //       make a big performance crater, but better than crashing due to out of memory.
        //       It may be best to reduce the number of threads per scheduler to 1 until it clears up instead
        //       of waiting for every send.
        //       If this doesn't improve transmission performance, just change it back to waiting all of the
        //       time.
        if (last) {
            LOGS(_log, LOG_LVL_INFO, "_sendbuf wait start " << note);
            streamBuf->waitForDoneWithThis();  // Block until this buffer has been sent.
        }
    }
    return sent;
}

bool SendChannelShared::buildAndTransmitError(util::MultiError& multiErr, Task::Ptr const& task,
                                              bool cancelled) {
    util::HoldTrack::Mark mark(ERR_LOC, "SCS buildAndTransmitError");
    auto qId = task->getQueryId();
    bool scanInteractive = true;
    _waitTransmitLock(scanInteractive, qId);
    lock_guard<mutex> lock(_tMtx);
    // Ignore the existing _transmitData object as it is irrelevant now
    // that there's an error. Create a new one to send the error.
    TransmitData::Ptr tData = _createTransmit(*task);
    _transmitData = tData;
    bool largeResult = false;
    _transmitData->buildDataMsg(*task, largeResult, multiErr);
    LOGS(_log, LOG_LVL_DEBUG, "SendChannelShared::buildAndTransmitError " << _dumpTr());
    bool lastIn = true;
    return _prepTransmit(task, cancelled, lastIn);
}

void SendChannelShared::setSchemaCols(Task& task, std::vector<SchemaCol>& schemaCols) {
    // _schemaCols should be identical for all tasks in this send channel.
    if (_schemaColsSet.exchange(true) == false) {
        _schemaCols = schemaCols;
        // If this is the first time _schemaCols has been set, it is missing
        // from the existing _transmitData object
        lock_guard<mutex> lock(_tMtx);
        if (_transmitData != nullptr) {
            _transmitData->addSchemaCols(_schemaCols);
        }
    }
}

bool SendChannelShared::buildAndTransmitResult(MYSQL_RES* mResult, int numFields, Task::Ptr const& task,
                                               bool largeResult, util::MultiError& multiErr,
                                               std::atomic<bool>& cancelled, bool& readRowsOk) {
    util::Timer transmitT;
    transmitT.start();
    double bufferFillSecs = 0.0;

    // 'cancelled' is passed as a reference so that if its value is
    // changed externally, it will break the while loop below.
    // Wait until the transmit Manager says it is ok to send data to the czar.
    auto qId = task->getQueryId();
    bool scanInteractive = task->getScanInteractive();
    _waitTransmitLock(scanInteractive, qId);

    // Lock the transmit mutex until this is done.
    lock_guard<mutex> lock(_tMtx);
    // Initialize _transmitData, if needed.
    _initTransmit(*task);

    numFields = mysql_num_fields(mResult);
    bool erred = false;
    size_t tSize = 0;

    int bytesTransmitted = 0;
    int rowsTransmitted = 0;

    // If fillRows returns false, _transmitData is full and needs to be transmitted
    // fillRows returns true when there are no more rows in mResult to add.
    // tSize is set by fillRows.
    bool more = true;
    while (more && !cancelled) {
        util::Timer bufferFillT;
        bufferFillT.start();
        more = !_transmitData->fillRows(mResult, numFields, tSize);
        if (tSize > proto::ProtoHeaderWrap::PROTOBUFFER_HARD_LIMIT) {
            LOGS_ERROR("Message single row too large to send using protobuffer");
            erred = true;
            util::Error worker_err(util::ErrorCode::INTERNAL,
                                   "Message single row too large to send using protobuffer");
            multiErr.push_back(worker_err);
            break;
        }
        bytesTransmitted += _transmitData->getResultSize();
        rowsTransmitted += _transmitData->getResultRowCount();
        _transmitData->buildDataMsg(*task, largeResult, multiErr);
        bufferFillT.stop();
        bufferFillSecs += bufferFillT.getElapsed();
        LOGS(_log, LOG_LVL_TRACE,
             "buildAndTransmitResult() more=" << more << " " << task->getIdStr() << " seq=" << task->getTSeq()
                                              << _dumpTr());

        // This will become true only if this is the last task sending its last transmit.
        // _prepTransmit will add the message to the queue and may try to transmit it now.
        bool lastIn = false;
        if (more) {
            if (readRowsOk && !_prepTransmit(task, cancelled, lastIn)) {
                LOGS(_log, LOG_LVL_ERROR, "Could not transmit intermediate results.");
                readRowsOk = false;  // Empty the fillRows data and then return false.
                erred = true;
                break;
            }
        } else {
            lastIn = transmitTaskLast(true);
            // If 'lastIn', this is the last transmit and it needs to be added.
            // Otherwise, just append the next query result rows to the existing _transmitData
            // and send it later.
            if (lastIn && readRowsOk && !_prepTransmit(task, cancelled, lastIn)) {
                LOGS(_log, LOG_LVL_ERROR, "Could not transmit intermediate results.");
                readRowsOk = false;  // Empty the fillRows data and then return false.
                erred = true;
                break;
            }
        }
    }

    transmitT.stop();
    double timeSeconds = transmitT.getElapsed();
    auto qStats = task->getQueryStats();
    if (qStats == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, "No statistics for " << task->getIdStr());
    } else {
        qStats->addTaskTransmit(timeSeconds, bytesTransmitted, rowsTransmitted, bufferFillSecs);
        LOGS(_log, LOG_LVL_TRACE,
             "TaskTransmit time=" << timeSeconds << " bufferFillSecs=" << bufferFillSecs);
    }

    return erred;
}

void SendChannelShared::_initTransmit(Task& task) {
    LOGS(_log, LOG_LVL_TRACE, "_initTransmit " << task.getIdStr() << " seq=" << task.getTSeq());
    if (_transmitData == nullptr) {
        _transmitData = _createTransmit(task);
    }
}

TransmitData::Ptr SendChannelShared::_createTransmit(Task& task) {
    LOGS(_log, LOG_LVL_TRACE, "_createTransmit " << task.getIdStr() << " seq=" << task.getTSeq());
    auto tData = wbase::TransmitData::createTransmitData(_czarId, task.getIdStr());
    tData->initResult(task, _schemaCols);
    return tData;
}

bool SendChannelShared::_prepTransmit(Task::Ptr const& task, bool cancelled, bool lastIn) {
    auto qId = task->getQueryId();
    int jId = task->getJobId();

    QSERV_LOGCONTEXT_QUERY_JOB(qId, jId);
    LOGS(_log, LOG_LVL_DEBUG, "_transmit lastIn=" << lastIn);
    if (isDead()) {
        LOGS(_log, LOG_LVL_INFO, "aborting transmit since sendChannel is dead.");
        return false;
    }

    // Have all rows already been read, or an error?
    bool erred = _transmitData->hasErrormsg();

    bool success = _addTransmit(task, cancelled, erred, lastIn, _transmitData, qId, jId);

    // Now that _transmitData is on the queue, reset and initialize a new one.
    _transmitData.reset();
    _initTransmit(*task);  // reset _transmitData

    return success;
}

string SendChannelShared::dumpTr() const {
    lock_guard<mutex> lock(_tMtx);
    return _dumpTr();
}

string SendChannelShared::_dumpTr() const {
    string str = "scs::dumpTr ";
    if (_transmitData == nullptr) {
        str += "nullptr";
    } else {
        str += _transmitData->dump();
    }
    return str;
}

}  // namespace lsst::qserv::wbase
