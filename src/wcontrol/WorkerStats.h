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
#ifndef LSST_QSERV_WCONTROL_WORKERSTATS_H
#define LSST_QSERV_WCONTROL_WORKERSTATS_H

// System headers
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <ostream>
#include <queue>
#include <sys/time.h>
#include <time.h>
#include <vector>

// qserv headers
#include "global/constants.h"
#include "util/Histogram.h"
#include "util/Mutex.h"

// Third party headers
#include <nlohmann/json.hpp>

namespace lsst::qserv::wcontrol {

/// This class is used to track statistics for the worker.
/// setup() needs to be called before get().
class WorkerStats : std::enable_shared_from_this<WorkerStats> {
public:
    using Ptr = std::shared_ptr<WorkerStats>;

    WorkerStats(WorkerStats const&) = delete;
    WorkerStats& operator=(WorkerStats const&) = delete;

    ~WorkerStats() = default;

    /// Setup the global WorkerStats instance
    /// @throws Bug if global has already been set or qdispPool is null.
    static void setup();

    /// Return a pointer to the global WorkerStats instance.
    /// @throws Bug if get() is called before setup()
    static Ptr get();

    /// Decrease the count and add the time taken to the histogram.
    void startQueryRespConcurrentQueued(TIMEPOINT created);

    /// Decrease the count and add the time taken to the histogram.
    void endQueryRespConcurrentQueued(TIMEPOINT created, TIMEPOINT start);

    /// Decrease the count and add the time taken to the histogram.
    void endQueryRespConcurrentXrootd(TIMEPOINT start, TIMEPOINT end);

    /// Get a json object describing queuing and waiting for transmission to the czar.
    nlohmann::json getSendStatsJson() const;

private:
    WorkerStats();
    static Ptr _globalWorkerStats;  ///< Pointer to the global instance.
    static MUTEX _globalMtx;        ///< Protects `_globalWorkerStats`

    std::atomic<int> _queueCount{
            0};  ///< Number of buffers on queues (there are many queues, one per ChannelShared)
    std::atomic<int> _xrootdCount{0};                   ///< Number of buffers held by xrootd.
    util::Histogram::Ptr _histConcurrentQueuedBuffers;  ///< How many buffers are queued at a given time
    util::Histogram::Ptr _histXrootdOwnedBuffers;  ///< How many of these buffers xrootd has at a given time
    util::HistogramRolling::Ptr _histSendQueueWaitTime;  ///< How long these buffers were on the queue
    util::HistogramRolling::Ptr _histSendXrootdTime;     ///< How long xrootd had possession of the buffers
};

}  // namespace lsst::qserv::wcontrol

#endif  // LSST_QSERV_WCONTROL_WORKERSTATS_H
