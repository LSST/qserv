// -*- LSST-C++ -*-
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
#ifndef LSST_QSERV_GLOBAL_CLOCKDEFS_H
#define LSST_QSERV_GLOBAL_CLOCKDEFS_H

// System headers
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <ostream>
#include <sys/time.h>
#include <time.h>

namespace lsst::qserv {

using CLOCK = std::chrono::system_clock;
using TIMEPOINT = std::chrono::time_point<CLOCK>;

inline uint64_t millisecSinceEpoch(TIMEPOINT tm) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch()).count();
}

/// RAII class to help track a changing sum through a begin and end time.
template <typename TType>
class TimeCountTracker {
public:
    using Ptr = std::shared_ptr<TimeCountTracker>;

    using CALLBACKFUNC = std::function<void(TIMEPOINT start, TIMEPOINT end, TType sum, bool success)>;
    TimeCountTracker() = delete;
    TimeCountTracker(TimeCountTracker const&) = delete;
    TimeCountTracker& operator=(TimeCountTracker const&) = delete;

    /// Constructor that includes the callback function that the destructor will call.
    TimeCountTracker(CALLBACKFUNC callback) : _callback(callback) {
        auto now = CLOCK::now();
        _startTime = now;
        _endTime = now;
    }

    /// Call the callback function as the dying act.
    ~TimeCountTracker() {
        TType sum;
        {
            std::lock_guard lg(_mtx);
            _endTime = CLOCK::now();
            sum = _sum;
        }
        _callback(_startTime, _endTime, sum, _success);
    }

    /// Add val to _sum
    void addToValue(TType val) {
        std::lock_guard lg(_mtx);
        _sum += val;
    }

    /// Call if the related action completed.
    void setSuccess() { _success = true; }

private:
    TIMEPOINT _startTime;
    TIMEPOINT _endTime;
    TType _sum = 0;  ///< atomic double doesn't support +=
    std::atomic<bool> _success{false};
    CALLBACKFUNC _callback;
    std::mutex _mtx;
};

}  // namespace lsst::qserv

#endif  // LSST_QSERV_GLOBAL_CLOCKDEFS_H
