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

#ifndef LSST_QSERV_WBASE_SENDCHANNELSHARED_H
#define LSST_QSERV_WBASE_SENDCHANNELSHARED_H

// System headers
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <stdexcept>
#include <thread>

// Qserv headers
#include "global/Bug.h"
#include "wbase/SendChannel.h"
#include "wbase/TransmitData.h"
#include "wcontrol/TransmitMgr.h"

namespace lsst {
namespace qserv {
namespace wbase {

/// A class that provides a SendChannel object with synchronization so it can be
/// shared by across multiple threads. Due to what may be sent, the synchronization locking
/// is needs to be available outside of the class.
/// Note: Tasks on a SendChannelShared cannot start processing until the total number of
///       Tasks using the SendChannelShared is know. Otherwise, there is a race condition
///       which could close the channel too soon.
class SendChannelShared {
public:
    using Ptr = std::shared_ptr<SendChannelShared>;

    /// To help ensure that _streamMutex is locked before calling,
    /// many member functions require a StreamGuard argument.
    using StreamGuard = std::lock_guard<std::mutex> const&;

    SendChannelShared()=delete;
    SendChannelShared(SendChannelShared const&) = delete;
    SendChannelShared& operator=(SendChannelShared const&) = delete;

    static Ptr create(SendChannel::Ptr const& sendChannel, wcontrol::TransmitMgr::Ptr const& transmitMgr);

    ~SendChannelShared();

    /// Wrappers for SendChannel public functions that may need to be used
    /// by threads.
    /// @see SendChannel::send
    bool send(StreamGuard sLock, char const* buf, int bufLen) {
        return _sendChannel->send(buf, bufLen);
    }

    /// @see SendChannel::sendError
    bool sendError(StreamGuard sLock, std::string const& msg, int code) {
        return _sendChannel->sendError(msg, code);
    }

    /// @see SendChannel::sendFile
    bool sendFile(StreamGuard sLock, int fd, SendChannel::Size fSize) {
        return _sendChannel->sendFile(fd, fSize);
    }

    /// @see SendChannel::sendStream
    bool sendStream(StreamGuard sLock, xrdsvc::StreamBuffer::Ptr const& sBuf, bool last) {
        return _sendChannel->sendStream(sBuf, last);
    }

    /// @see SendChannel::kill
    bool kill(StreamGuard sLock, std::string const& note);

    /// @see SendChannel::isDead
    bool isDead() {
        if (_sendChannel == nullptr) return true;
        return _sendChannel->isDead();
    }

    /// All of the tasks that use this SendChannel must be added
    /// to the scheduler queue at the same time or it risks a race condition.
    void incrTaskCountBy(int subCount);

    /// Try to transmit the data in tData.
    /// If the queue already has at least 2 TransmitData objects, addTransmit
    /// may wait before returning. Result rows are read from the
    /// database until there are no more rows or the buffer is
    /// sufficiently full. addTransmit waits until that buffer has been
    /// sent to the czar before reading more rows. Without the wait,
    /// the worker may read in too many result rows, run out of memory,
    /// and crash.
    bool addTransmit(bool cancelled, bool erred, bool last, bool largeResult,
                     TransmitData::Ptr const& tData, int qId, int jId);

    ///
    /// @return true if inLast is true and this is the last task to call this
    ///              with inLast == true.
    bool transmitTaskLast(bool inLast);

    /// streamMutex is used to protect _lastCount and messages that are sent
    /// using SendChannelShared.
    std::mutex streamMutex;

    /// Return a normalized id string.
    std::string makeIdStr(int qId, int jId);

    uint64_t getId() const { return _id; }
    int getTaskCount() const { return _taskCount; }
    int getLastCount() const { return _lastCount; }

    uint64_t getSeq() const { return _sendChannel->getSeq(); }

private:
    /// Private constructor to protect shared pointer integrity.
    SendChannelShared(uint64_t id, SendChannel::Ptr const& sendChannel, wcontrol::TransmitMgr::Ptr const& transmitMgr)
            : _id(id), _transmitMgr(transmitMgr), _sendChannel(sendChannel) {
        if (_sendChannel == nullptr) {
            throw Bug("SendChannelShared constructor given nullptr");
        }
    }

    /// Encode TransmitData items from _transmitQueue and pass them to XrdSsi
    /// to be sent to the czar.
    /// The header for the 'nextTransmit' item is appended to the result of
    /// 'thisTransmit', with a specially constructed header appended for the
    /// 'reallyLast' transmit.
    /// _queueMtx must be held before calling this.
    bool _transmit(bool erred, bool scanInteractive, bool largeResult, qmeta::CzarId czarId);

    /// Send the buffer 'streamBuffer' using xrdssi. L
    /// 'last' should only be true if this is the last buffer to be sent with this _sendChannel.
    /// 'note' is just a log note about what/who is sending the buffer.
    /// @return true if the buffer was sent.
    bool _sendBuf(std::lock_guard<std::mutex> const& streamLock,
                  xrdsvc::StreamBuffer::Ptr& streamBuf, bool last,
                  std::string const& note);

    uint64_t const _id; ///< id number for this send channel shared.

    std::queue<TransmitData::Ptr> _transmitQueue; ///< Queue of data to be encoded and sent.
    std::mutex _queueMtx; ///< protects _transmitQueue, _taskCount, _lastCount

    /// metadata buffer. Once set, it cannot change until after Finish() has been called.
    std::string _metadataBuf;

    /// The number of tasks to be sent over this SendChannel. This must be set to the final value
    /// before any tasks are processed to avoid race conditions.
    std::atomic<int> _taskCount{0}; ///< The number of tasks to be sent over this SendChannel.
    std::atomic<int> _lastCount{0}; ///< Then number of 'last' buffers received.
    std::mutex _lastCMtx;         ///< Protects _lastCount and the validity of transmitTaskLast(bool inLast).

    std::atomic<bool> _lastRecvd{false}; ///< The truly 'last' transmit message is in the queue.
    std::atomic<bool> _firstTransmit{true}; ///< True until the first transmit has been sent.

    /// Used to limit the number of transmits being sent to czars.
    wcontrol::TransmitMgr::Ptr const _transmitMgr;

    SendChannel::Ptr _sendChannel; ///< Used to pass encoded information to XrdSsi.
};

}}} // namespace lsst::qserv::wbase

#endif // LSST_QSERV_WBASE_SENDCHANNELSHARED_H
