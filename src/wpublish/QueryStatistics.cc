// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2016 LSST Corporation.
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
#include "wpublish/QueryStatistics.h"

// System headers
#include <algorithm>

// Qserv headers
#include "util/Bug.h"
#include "util/HoldTrack.h"
#include "util/TimeUtils.h"
#include "wbase/TaskState.h"
#include "wbase/UserQueryInfo.h"
#include "wsched/BlendScheduler.h"
#include "wsched/SchedulerBase.h"
#include "wsched/ScanScheduler.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
namespace wbase = lsst::qserv::wbase;
namespace wpublish = lsst::qserv::wpublish;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wpublish.QueriesAndChunks");
}  // namespace

namespace lsst::qserv::wpublish {

QueryStatistics::QueryStatistics(QueryId qId_, CzarIdType czarId_)
        : creationTime(CLOCK::now()),
          queryId(qId_),
          _userQueryInfo(wbase::UserQueryInfo::create(qId_, czarId_)) {
    /// For all of the histograms, all entries should be kept at least until the work is finished.
    string qidStr = to_string(queryId);
    _histSizePerTask = util::Histogram::Ptr(new util::Histogram(
            string("SizePerTask_") + qidStr, {1'000, 10'0000, 1'000'000, 10'000'000, 100'000'000}));
    _histRowsPerTask = util::Histogram::Ptr(new util::Histogram(string("RowsPerChunk_") + qidStr,
                                                                {1, 100, 1'000, 10'000, 100'000, 1'000'000}));
    _histTimeRunningPerTask = util::Histogram::Ptr(new util::Histogram(
            string("TimeRunningPerTask_") + qidStr, {0.1, 1, 10, 30, 60, 120, 300, 600, 1200, 10000}));
    _histTimeSubchunkPerTask = util::Histogram::Ptr(new util::Histogram(
            string("TimeSubchunkPerTask_") + qidStr, {0.1, 1, 10, 30, 60, 120, 300, 600, 1200, 10000}));
    _histTimeTransmittingPerTask = util::Histogram::Ptr(new util::Histogram(
            string("TimeTransmittingPerTask_") + qidStr, {0.1, 1, 10, 30, 60, 120, 300, 600, 1200, 10000}));
    _histTimeBufferFillPerTask = util::Histogram::Ptr(new util::Histogram(
            string("TimeFillingBuffersPerTask_") + qidStr, {0.1, 1, 10, 30, 60, 120, 300, 600, 1200, 10000}));
}

/// Return a json object containing high level data, such as histograms.
nlohmann::json QueryStatistics::getJsonHist() const {
    nlohmann::json js = nlohmann::json::object();
    js["timeRunningPerTask"] = _histTimeRunningPerTask->getJson();
    js["timeSubchunkPerTask"] = _histTimeSubchunkPerTask->getJson();
    js["timeTransmittingPerTask"] = _histTimeTransmittingPerTask->getJson();
    js["timeBufferFillPerTask"] = _histTimeBufferFillPerTask->getJson();
    js["sizePerTask"] = _histSizePerTask->getJson();
    js["rowsPerTask"] = _histRowsPerTask->getJson();
    return js;
}

nlohmann::json QueryStatistics::getJsonTasks(wbase::TaskSelector const& taskSelector) const {
    lock_guard<mutex> const guard(_qStatsMtx);
    nlohmann::json result = nlohmann::json::object();
    result["snapshotTime_msec"] = util::TimeUtils::now();
    result["entries"] = nlohmann::json::array();
    auto& taskEntriesJson = result["entries"];
    uint32_t numTasksSelected = 0;
    if (taskSelector.includeTasks) {
        auto const& ids = taskSelector.queryIds;
        auto const& states = taskSelector.taskStates;
        for (auto&& task : _tasks) {
            bool const selectedByQueryId =
                    ids.empty() || find(ids.begin(), ids.end(), task->getQueryId()) != ids.end();
            bool const selectedByTaskState =
                    states.empty() || find(states.begin(), states.end(), task->state()) != states.end();
            if (selectedByQueryId && selectedByTaskState) {
                ++numTasksSelected;
                // Stop reporting tasks if the limit has been reached. Just keep
                // counting the number of tasks selected by the filter.
                if ((taskSelector.maxTasks == 0) || (numTasksSelected < taskSelector.maxTasks)) {
                    taskEntriesJson.push_back(task->getJson());
                }
            }
        }
    }
    result["total"] = _tasks.size();
    result["selected"] = numTasksSelected;
    return result;
}

/// Add a Task to the user query statistics.
void QueryStatistics::addTask(wbase::Task::Ptr const& task) {
    lock_guard<mutex> guard(_qStatsMtx);
    if (queryId != task->getQueryId()) {
        string const msg = "QueryStatistics::" + string(__func__) +
                           ": the task has queryId=" + to_string(task->getQueryId()) +
                           " where queryId=" + to_string(queryId) + " was expected.";
        throw util::Bug(ERR_LOC, msg);
    }
    _tasks.push_back(task);
}

/// @return the number of Tasks that have been booted for this user query.
int QueryStatistics::getTasksBooted() {
    lock_guard<mutex> guard(_qStatsMtx);
    return _tasksBooted;
}

/// @return true if this query is done and has not been touched for deadTime.
bool QueryStatistics::isDead(chrono::seconds deadTime, chrono::system_clock::time_point now) {
    lock_guard<mutex> guard(_qStatsMtx);
    if (_isMostlyDead()) {
        if (now - _touched > deadTime) {
            return true;
        }
    }
    return false;
}

/// @return true if all Tasks for this query are complete.
/// Precondition, _qStatsMtx must be locked.
bool QueryStatistics::_isMostlyDead() const { return _tasksCompleted >= _size; }

ostream& operator<<(ostream& os, QueryStatistics const& q) {
    lock_guard<mutex> gd(q._qStatsMtx);
    os << QueryIdHelper::makeIdStr(q.queryId) << " time=" << q._totalTimeMinutes << " size=" << q._size
       << " tasksCompleted=" << q._tasksCompleted << " tasksRunning=" << q._tasksRunning
       << " tasksBooted=" << q._tasksBooted;
    return os;
}

void QueryStatistics::addTaskTransmit(double timeSeconds, int64_t bytesTransmitted, int64_t rowsTransmitted,
                                      double bufferFillSecs) {
    _histTimeTransmittingPerTask->addEntry(timeSeconds);
    _histRowsPerTask->addEntry(rowsTransmitted);
    _histSizePerTask->addEntry(bytesTransmitted);
    _histTimeBufferFillPerTask->addEntry(bufferFillSecs);
}

void QueryStatistics::addTaskRunQuery(double runTimeSeconds, double subchunkRunTimeSeconds) {
    _histTimeRunningPerTask->addEntry(runTimeSeconds);
    _histTimeSubchunkPerTask->addEntry(subchunkRunTimeSeconds);
}

void QueryStatistics::tasksAddedToScheduler(std::shared_ptr<wsched::SchedulerBase> const& sched,
                                            int numberOfTasksAdded) {
    if (sched == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, "QueryStatistics::tasksAddedToScheduler sched == nullptr");
        return;
    }
    string schedName = sched->getName();
    lock_guard<mutex> lockG(_qStatsMtx);
    auto iter = _taskSchedInfoMap.find(schedName);
    if (iter == _taskSchedInfoMap.end()) {
        _taskSchedInfoMap.insert({schedName, SchedulerTasksInfo(sched, numberOfTasksAdded)});
    } else {
        SchedulerTasksInfo& schedTasksInfo = iter->second;
        schedTasksInfo.taskCount += numberOfTasksAdded;
        schedTasksInfo.mostRecentTaskAdded = CLOCK::now();
    }
}

QueryStatistics::SchedTasksInfoMap QueryStatistics::getSchedulerTasksInfoMap() {
    lock_guard<mutex> lockG(_qStatsMtx);
    return _taskSchedInfoMap;
}

void QueryStatistics::addTask(TIMEPOINT const now) {
    lock_guard<mutex> lock(_qStatsMtx);
    _touched = now;
    _size += 1;
}

void QueryStatistics::addTaskRunning(TIMEPOINT const now) {
    lock_guard<mutex> lock(_qStatsMtx);
    _touched = now;
    _tasksRunning += 1;
}

void QueryStatistics::setQueryBooted(bool booted, TIMEPOINT now) {
    lock_guard<mutex> lock(_qStatsMtx);
    _queryBooted = booted;
    _queryBootedTime = now;
}

bool QueryStatistics::addTaskCompleted(TIMEPOINT const now, double const taskDuration) {
    lock_guard<mutex> gs(_qStatsMtx);
    _touched = now;
    _tasksRunning -= 1;
    _tasksCompleted += 1;
    _totalTimeMinutes += taskDuration;
    return _isMostlyDead();
}

vector<wbase::Task::Ptr> QueryStatistics::getRunningTasks() const {
    vector<wbase::Task::Ptr> runningTasks;
    {
        lock_guard<mutex> lock(_qStatsMtx);
        for (auto&& task : _tasks) {
            if (task->isRunning()) {
                auto const& tSched = dynamic_pointer_cast<wsched::ScanScheduler>(task->getTaskScheduler());
                if (tSched != nullptr) {
                    runningTasks.push_back(task);
                    LOGS(_log, LOG_LVL_DEBUG,
                         __func__ << " task=" << task->getIdStr() << " running=" << task->isRunning());
                }
            }
        };
    }
    return runningTasks;
}

vector<wbase::Task::Ptr> QueryStatistics::getTaskList() const {
    lock_guard<mutex> lock(_qStatsMtx);
    return _tasks;
}

void QueryStatistics::mySqlThread2task(set<unsigned long> const& activeMySqlThreadIds,
                                       nlohmann::json& result) const {
    for (auto&& task : _tasks) {
        auto const threadId = task->getMySqlThreadId();
        if ((threadId != 0) && activeMySqlThreadIds.contains(threadId)) {
            // Force the identifier to be converted into a string because the JSON library
            // doesn't support numeric keys in its dictionary class.
            result[to_string(threadId)] =
                    nlohmann::json::object({{"query_id", task->getQueryId()},
                                            {"job_id", task->getJobId()},
                                            {"chunk_id", task->getChunkId()},
                                            {"subchunk_id", task->getSubchunkId()},
                                            {"template_id", task->getTemplateId()},
                                            {"state", wbase::taskState2str(task->state())}});
        }
    }
}

}  // namespace lsst::qserv::wpublish
