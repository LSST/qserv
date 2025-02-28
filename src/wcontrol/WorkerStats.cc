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

// Class header
#include "wcontrol/WorkerStats.h"

#include <cstdio>
#include <float.h>
#include <set>

// qserv headers
#include "util/Bug.h"
#include "util/Histogram.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

using namespace std::chrono_literals;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wcontrol.WorkerStats");
}

namespace lsst::qserv::wcontrol {

WorkerStats::Ptr WorkerStats::_globalWorkerStats;
MUTEX WorkerStats::_globalMtx;

void WorkerStats::setup() {
    lock_guard lg(_globalMtx);
    if (_globalWorkerStats != nullptr) {
        throw util::Bug(ERR_LOC, "Error WorkerStats::setup called after global pointer set.");
    }
    _globalWorkerStats = Ptr(new WorkerStats());
}

WorkerStats::WorkerStats() {
    auto bucketTimes = {1.0, 20.0, 60.0, 600.0, 1000.0, 10'000.0};
    _histSendQueueWaitTime = util::HistogramRolling::Ptr(
            new util::HistogramRolling("SendQueueWaitTime", bucketTimes, 1h, 10'000));

    _histSendXrootdTime = util::HistogramRolling::Ptr(
            new util::HistogramRolling("SendXrootdTime", bucketTimes, 1h, 10'000));

    auto bucketVals = {10.0, 100.0, 1'000.0, 10'000.0, 100'000.0, 500'000.0, 1'000'000.0};
    _histConcurrentQueuedBuffers =
            util::Histogram::Ptr(new util::Histogram("ConcurrentQueuedBuffers", bucketVals));
    _histXrootdOwnedBuffers = util::Histogram::Ptr(new util::Histogram("XrootdOwnedBuffers", bucketVals));
}

WorkerStats::Ptr WorkerStats::get() {
    std::lock_guard lg(_globalMtx);
    if (_globalWorkerStats == nullptr) {
        throw util::Bug(ERR_LOC, "Error CzarStats::get called before CzarStats::setup.");
    }
    return _globalWorkerStats;
}

void WorkerStats::startQueryRespConcurrentQueued(TIMEPOINT created) {
    ++_queueCount;

    _histConcurrentQueuedBuffers->addEntry(created, _queueCount);
    LOGS(_log, LOG_LVL_TRACE, "startQueryRespConcurrentQueued: " << getSendStatsJson());
}

void WorkerStats::endQueryRespConcurrentQueued(TIMEPOINT created, TIMEPOINT start) {
    --_queueCount;
    ++_xrootdCount;
    std::chrono::duration<double> secs = start - created;
    _histSendQueueWaitTime->addEntry(start, secs.count());
    _histXrootdOwnedBuffers->addEntry(start, _xrootdCount);
    _histConcurrentQueuedBuffers->addEntry(start, _queueCount);

    LOGS(_log, LOG_LVL_TRACE, "endQueryRespConcurrentQueued: " << getSendStatsJson());
}

void WorkerStats::endQueryRespConcurrentXrootd(TIMEPOINT start, TIMEPOINT end) {
    --_xrootdCount;
    std::chrono::duration<double> secs = end - start;
    _histXrootdOwnedBuffers->addEntry(start, _xrootdCount);
    _histXrootdOwnedBuffers->addEntry(end, _queueCount);

    LOGS(_log, LOG_LVL_TRACE, "endQueryRespConcurrentXrootd: " << getSendStatsJson());
}

nlohmann::json WorkerStats::getSendStatsJson() const {
    nlohmann::json js;
    js["ConcurrentQueuedBuffers"] = _histConcurrentQueuedBuffers->getJson();
    js["XrootdOwnedBuffers"] = _histXrootdOwnedBuffers->getJson();
    js["SendQueueWaitTime"] = _histSendQueueWaitTime->getJson();
    js["SendXrootdTime"] = _histSendXrootdTime->getJson();
    return js;
}

}  // namespace lsst::qserv::wcontrol
