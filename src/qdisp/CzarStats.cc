// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013 LSST Corporation.
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
#include "qdisp/CzarStats.h"

// System headers
#include <chrono>

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "util/Bug.h"
#include "util/QdispPool.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::chrono_literals;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.CzarStats");
}

namespace lsst::qserv::qdisp {

CzarStats::Ptr CzarStats::_globalCzarStats;
MUTEX CzarStats::_globalMtx;

void CzarStats::setup(util::QdispPool::Ptr const& qdispPool) {
    std::lock_guard lg(_globalMtx);
    if (_globalCzarStats != nullptr || qdispPool == nullptr) {
        throw util::Bug(ERR_LOC, "Error CzarStats::setup called after global pointer set or qdispPool=null.");
    }
    _globalCzarStats = Ptr(new CzarStats(qdispPool));
}

CzarStats::CzarStats(util::QdispPool::Ptr const& qdispPool)
        : _qdispPool(qdispPool), _startTimeMs(util::TimeUtils::now()) {
    auto bucketValsRates = {128'000.0,       512'000.0,       1'024'000.0,     16'000'000.0,
                            128'000'000.0,   256'000'000.0,   512'000'000.0,   768'000'000.0,
                            1'000'000'000.0, 2'000'000'000.0, 4'000'000'000.0, 8'000'000'000.0};
    _histXRootDSSIRecvRate = util::HistogramRolling::Ptr(
            new util::HistogramRolling("XRootDSSIRecvRateBytesPerSec", bucketValsRates, 1h, 10000));
    _histMergeRate = util::HistogramRolling::Ptr(
            new util::HistogramRolling("MergeRateBytesPerSec", bucketValsRates, 1h, 10000));
    _histFileReadRate = util::HistogramRolling::Ptr(
            new util::HistogramRolling("FileReadRateBytesPerSec", bucketValsRates, 1h, 10000));
    auto bucketValsTimes = {0.1, 1.0, 10.0, 100.0, 1000.0};
    _histRespSetup = util::HistogramRolling::Ptr(
            new util::HistogramRolling("RespSetupTime", bucketValsTimes, 1h, 10000));
    _histRespWait = util::HistogramRolling::Ptr(
            new util::HistogramRolling("RespWaitTime", bucketValsTimes, 1h, 10000));
    _histRespProcessing = util::HistogramRolling::Ptr(
            new util::HistogramRolling("RespProcessingTime", bucketValsTimes, 1h, 10000));
}

CzarStats::Ptr CzarStats::get() {
    std::lock_guard lg(_globalMtx);
    if (_globalCzarStats == nullptr) {
        throw util::Bug(ERR_LOC, "Error CzarStats::get called before CzarStats::setup.");
    }
    return _globalCzarStats;
}

void CzarStats::endQueryRespConcurrentSetup(TIMEPOINT start, TIMEPOINT end) {
    --_queryRespConcurrentSetup;
    std::chrono::duration<double> secs = end - start;
    _histRespSetup->addEntry(end, secs.count());
}

void CzarStats::endQueryRespConcurrentWait(TIMEPOINT start, TIMEPOINT end) {
    --_queryRespConcurrentWait;
    std::chrono::duration<double> secs = end - start;
    _histRespWait->addEntry(end, secs.count());
}

void CzarStats::endQueryRespConcurrentProcessing(TIMEPOINT start, TIMEPOINT end) {
    --_queryRespConcurrentProcessing;
    std::chrono::duration<double> secs = end - start;
    _histRespProcessing->addEntry(end, secs.count());
}

void CzarStats::addXRootDSSIRecvRate(double bytesPerSec) {
    _histXRootDSSIRecvRate->addEntry(bytesPerSec);
    LOGS(_log, LOG_LVL_TRACE,
         "CzarStats::" << __func__ << " " << bytesPerSec << " " << _histXRootDSSIRecvRate->getString(""));
}

void CzarStats::addMergeRate(double bytesPerSec) {
    _histMergeRate->addEntry(bytesPerSec);
    LOGS(_log, LOG_LVL_TRACE,
         "CzarStats::" << __func__ << " " << bytesPerSec << " " << _histMergeRate->getString("")
                       << " jsonA=" << getTransmitStatsJson() << " jsonB=" << getQdispStatsJson());
}

void CzarStats::addFileReadRate(double bytesPerSec) {
    _histFileReadRate->addEntry(bytesPerSec);
    LOGS(_log, LOG_LVL_TRACE,
         "CzarStats::" << __func__ << " " << bytesPerSec << " " << _histFileReadRate->getString(""));
}

void CzarStats::trackQueryProgress(QueryId qid) {
    if (qid == 0) return;
    uint64_t const currentTimestampMs = util::TimeUtils::now();
    std::lock_guard const lock(_queryProgressMtx);
    if (auto itr = _queryNumIncompleteJobs.find(qid); itr != _queryNumIncompleteJobs.end()) return;
    _queryNumIncompleteJobs[qid].emplace_back(currentTimestampMs, 0);
}

void CzarStats::updateQueryProgress(QueryId qid, int numUnfinishedJobs) {
    if (qid == 0) return;
    uint64_t const currentTimestampMs = util::TimeUtils::now();
    std::lock_guard const lock(_queryProgressMtx);
    if (auto itr = _queryNumIncompleteJobs.find(qid); itr != _queryNumIncompleteJobs.end()) {
        auto&& history = itr->second;
        if (history.empty() || (history.back().numJobs != numUnfinishedJobs)) {
            history.emplace_back(currentTimestampMs, numUnfinishedJobs);
        }
    } else {
        _queryNumIncompleteJobs[qid].emplace_back(currentTimestampMs, numUnfinishedJobs);
    }
}

void CzarStats::untrackQueryProgress(QueryId qid) {
    if (qid == 0) return;
    unsigned int const lastSeconds = cconfig::CzarConfig::instance()->czarStatsRetainPeriodSec();
    uint64_t const minTimestampMs = util::TimeUtils::now() - 1000 * lastSeconds;
    std::lock_guard const lock(_queryProgressMtx);
    if (lastSeconds == 0) {
        // The query gets removed instantaneously if archiving is not enabled.
        if (auto itr = _queryNumIncompleteJobs.find(qid); itr != _queryNumIncompleteJobs.end()) {
            _queryNumIncompleteJobs.erase(itr);
        }
    } else {
        // Erase queries with the last recorded timestamp that's older
        // than the specified cut-off time.
        for (auto iter = _queryNumIncompleteJobs.begin(); iter != _queryNumIncompleteJobs.end();) {
            auto const& history = iter->second;
            // It's critical that `iter` is only advanced once.
            if (!history.empty() && history.back().timestampMs < minTimestampMs) {
                iter = _queryNumIncompleteJobs.erase(iter);
            } else {
                ++iter;
            }
        }
    }
}

CzarStats::QueryProgress CzarStats::getQueryProgress(QueryId qid, unsigned int lastSeconds) const {
    uint64_t const minTimestampMs = util::TimeUtils::now() - 1000 * lastSeconds;
    std::lock_guard const lock(_queryProgressMtx);
    QueryProgress result;
    if (qid == 0) {
        if (lastSeconds == 0) {
            // Full histories of all registered queries
            result = _queryNumIncompleteJobs;
        } else {
            // Age restricted histories of all registered queries
            for (auto&& [qid, history] : _queryNumIncompleteJobs) {
                for (auto&& point : history) {
                    if (point.timestampMs >= minTimestampMs) result[qid].push_back(point);
                }
            }
        }
    } else {
        if (auto itr = _queryNumIncompleteJobs.find(qid); itr != _queryNumIncompleteJobs.end()) {
            auto&& history = itr->second;
            if (lastSeconds == 0) {
                // Full history of the specified query
                result[qid] = history;
            } else {
                // Age restricted history of the specified query
                for (auto&& point : history) {
                    if (point.timestampMs >= minTimestampMs) result[qid].push_back(point);
                }
            }
        }
    }
    return result;
}

nlohmann::json CzarStats::getQdispStatsJson() const {
    nlohmann::json result;
    result["QdispPool"] = _qdispPool->getJson();
    result["queryRespConcurrentSetupCount"] = _queryRespConcurrentSetup.load();
    result["queryRespConcurrentWaitCount"] = _queryRespConcurrentWait.load();
    result["queryRespConcurrentProcessingCount"] = _queryRespConcurrentProcessing.load();
    result[_histRespSetup->label()] = _histRespSetup->getJson();
    result[_histRespWait->label()] = _histRespWait->getJson();
    result[_histRespProcessing->label()] = _histRespProcessing->getJson();
    result["totalQueries"] = _totalQueries.load();
    result["totalJobs"] = _totalJobs.load();
    result["totalResultFiles"] = _totalResultFiles.load();
    result["totalResultMerges"] = _totalResultMerges.load();
    result["totalBytesRecv"] = _totalBytesRecv.load();
    result["totalRowsRecv"] = _totalRowsRecv.load();
    result["numQueries"] = _numQueries.load();
    result["numJobs"] = _numJobs.load();
    result["numResultFiles"] = _numResultFiles.load();
    result["numResultMerges"] = _numResultMerges.load();
    result["startTimeMs"] = _startTimeMs;
    result["snapshotTimeMs"] = util::TimeUtils::now();
    return result;
}

nlohmann::json CzarStats::getTransmitStatsJson() const {
    nlohmann::json result;
    result[_histXRootDSSIRecvRate->label()] = _histXRootDSSIRecvRate->getJson();
    result[_histMergeRate->label()] = _histMergeRate->getJson();
    result[_histFileReadRate->label()] = _histFileReadRate->getJson();
    return result;
}

}  // namespace lsst::qserv::qdisp
