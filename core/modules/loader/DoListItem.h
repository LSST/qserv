// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
 *
 */
#ifndef LSST_QSERV_LOADER_DOLISTITEM_H
#define LSST_QSERV_LOADER_DOLISTITEM_H

// system headers
#include <chrono>
#include <list>

// Qserv headers
#include "util/ThreadPool.h"

namespace lsst {
namespace qserv {
namespace loader {


class Central;


class TimeOut {
public:
    using TimePoint = std::chrono::system_clock::time_point;
    using Clock = std::chrono::system_clock;

    explicit TimeOut(std::chrono::milliseconds timeOut) : _timeOut(timeOut) {}

    bool due() { return due(Clock::now()); }
    bool due(TimePoint now) {
        auto triggerDiff = std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastTrigger);
        return (triggerDiff > _timeOut);
    }

    void triggered() { return triggered(Clock::now()); }
    void triggered(TimePoint now) {
        _lastTrigger = now;
    }

    std::chrono::milliseconds timeLeft(TimePoint now) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - _lastTrigger);
    }

    void setTimeOut(std::chrono::milliseconds timeOut) { _timeOut = timeOut; }
    std::chrono::milliseconds getTimeOut() const { return _timeOut; }
private:
    // How much time since lastTrigger needs to pass before triggering.
    std::chrono::milliseconds _timeOut;
    TimePoint _lastTrigger{std::chrono::seconds(0)};
};


/// This is a base class for other classes to be put on a DoList.
/// Child classes of this class *MUST* be created with shared pointers.
///
/// A DoListItem is meant to checked periodically by the DoList
/// at a low frequency (a couple of times a second to once every
/// few hours or even days).
/// The DoListItems can cycle forever by just remaining on the
/// DoList where it will run their actions when the timer runs out,
/// which is useful for monitoring status.
/// Or they can be setup to run until they have completed once,
/// a oneShot, which is useful for looking up or inserting keys.
///
/// A typical action would be sending out a UDP request for status
/// every few seconds until a response is received. Then, after a
/// few minutes with no updates, repeating that request to make sure
/// the status hasn't changed.
/// The system is supposed to notify others on changes, but these
/// notifications can get lost, so it makes sense to ask again if
/// nothing has been received for a while.
class DoListItem : public std::enable_shared_from_this<DoListItem> {
public:
    using Ptr = std::shared_ptr<DoListItem>;

    DoListItem(DoListItem const&) = delete;
    DoListItem& operator=(DoListItem const&) = delete;

    virtual ~DoListItem() = default;

    util::CommandTracked::Ptr runIfNeeded(TimeOut::TimePoint now) {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_command == nullptr) {
            if (_isOneShotDone()) return nullptr;
            if ((_needInfo || _timeOut.due(now)) && _timeRequest.due(now)) {
                _timeRequest.triggered();
                _command = createCommand();
                return _command;
            }
        } else if (_command->isFinished()) {
            _command.reset(); // Allow the command to be sent again later.
        }
        return nullptr;
    }

    bool isAlreadyOnList() { return _addedToList; }

    /// Returns original value of _addedToList.
    bool setAddedToList(bool value) {
        return _addedToList.exchange(value);
    }

    /// @return true if this item should be removed from the list.
    bool shouldRemoveFromList() {
        std::lock_guard<std::mutex> lock(_mtx);
        return (_isOneShotDone() || _remove);
    }

    /// The info has been updated, so no need to ask for it for a while.
    void infoReceived() {
        std::lock_guard<std::mutex> lock(_mtx);
        _needInfo = false;
        _timeOut.triggered();
    }

    void setNeedInfo() {
        std::lock_guard<std::mutex> lock(_mtx);
        _needInfo = true;
    }

    DoListItem::Ptr getDoListItemPtr() {
        return shared_from_this();
    }

    void setTimeOut(std::chrono::milliseconds timeOut) { _timeOut.setTimeOut(timeOut); }

    virtual util::CommandTracked::Ptr createCommand()=0;

protected:
    /// All derived instances of this class *MUST* be created with shared pointers.
    /// A factory function to enforce this is not practical since
    /// this class is meant to serve as a base class for unknown
    /// future purposes. Sadly, the compiler doesn't enforce the rule.
    DoListItem() = default;

    /// Set true if this item only needs to be successfully completed once.
    void setOneShot(bool val) { _oneShot = val; }

private:
    /// Lock _mtx before calling.
    bool _isOneShotDone() {
        return (!_needInfo && _oneShot);
    }

    std::atomic<bool>    _addedToList{false}; ///< True when added to a DoList
    bool    _oneShot{false}; ///< True if after the needed information is gathered, this item can be dropped.
    bool    _needInfo{true}; ///< True if information is needed.
    bool    _remove{false}; ///< set to true if this item should no longer be checked.
    /// If no info is needed, check for info after this period of time.
    TimeOut _timeOut{std::chrono::minutes(5)};
    /// Rate limiter, no more than 1 message every few seconds
    TimeOut _timeRequest{std::chrono::seconds(4)};  // TODO: DM-17453 set via config
    util::CommandTracked::Ptr _command;
    std::mutex _mtx; ///< protects _timeOut, _timeRequest, _command, _oneShot, _needInfo
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_DOLISTITEM_H
