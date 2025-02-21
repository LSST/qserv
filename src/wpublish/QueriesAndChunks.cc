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
#include "wpublish/QueriesAndChunks.h"

// System headers
#include <algorithm>

// Qserv headers
#include "util/Bug.h"
#include "util/HoldTrack.h"
#include "util/TimeUtils.h"
#include "wbase/TaskState.h"
#include "wbase/UserQueryInfo.h"
#include "wconfig/WorkerConfig.h"
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

QueriesAndChunks::Ptr QueriesAndChunks::_globalQueriesAndChunks;

QueriesAndChunks::Ptr QueriesAndChunks::get(bool noThrow) {
    if (_globalQueriesAndChunks == nullptr && !noThrow) {
        throw util::Bug(ERR_LOC, "QueriesAndChunks::get() called before QueriesAndChunks::setupGlobal");
    }
    return _globalQueriesAndChunks;
}

QueriesAndChunks::Ptr QueriesAndChunks::setupGlobal(chrono::seconds deadAfter, chrono::seconds examineAfter,
                                                    int maxTasksBooted, int maxDarkTasks,
                                                    bool resetForTesting) {
    if (resetForTesting) {
        _globalQueriesAndChunks.reset();
    }
    if (_globalQueriesAndChunks != nullptr) {
        throw util::Bug(ERR_LOC, "QueriesAndChunks::setupGlobal called twice");
    }
    _globalQueriesAndChunks = Ptr(new QueriesAndChunks(deadAfter, examineAfter));
    return _globalQueriesAndChunks;
}

QueriesAndChunks::QueriesAndChunks(chrono::seconds deadAfter, chrono::seconds examineAfter)
        : _deadAfter(deadAfter),
          _examineAfter(examineAfter),
          _maxTasksBooted(wconfig::WorkerConfig::instance()->getMaxTasksBootedPerUserQuery()),
          _maxDarkTasks(wconfig::WorkerConfig::instance()->getMaxConcurrentBootedTasks()) {
    auto rDead = [this]() {
        while (_loopRemoval) {
            removeDead();
            this_thread::sleep_for(_deadAfter);
        }
    };
    thread td(rDead);
    _removalThread = move(td);

    if (_examineAfter.count() == 0) {
        LOGS(_log, LOG_LVL_DEBUG, "QueriesAndChunks turning off examineThread");
        _loopExamine = false;
    }

    auto rExamine = [this]() {
        while (_loopExamine) {
            this_thread::sleep_for(_examineAfter);
            if (_loopExamine) examineAll();
        }
    };
    thread te(rExamine);
    _examineThread = move(te);
}

QueriesAndChunks::~QueriesAndChunks() {
    _loopRemoval = false;
    _loopExamine = false;
    try {
        _removalThread.join();
        _examineThread.join();
    } catch (system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "~QueriesAndChunks " << e.what());
    }
}

void QueriesAndChunks::setBlendScheduler(shared_ptr<wsched::BlendScheduler> const& blendSched) {
    _blendSched = blendSched;
}

void QueriesAndChunks::setRequiredTasksCompleted(unsigned int value) { _requiredTasksCompleted = value; }

QueryStatistics::Ptr QueriesAndChunks::_addQueryId(QueryId qId, CzarIdType czarId) {
    auto itr = _queryStatsMap.find(qId);
    QueryStatistics::Ptr stats;
    if (_queryStatsMap.end() == itr) {
        stats = QueryStatistics::create(qId, czarId);
        _queryStatsMap[qId] = stats;
    } else {
        stats = itr->second;
    }
    return stats;
}

QueryStatistics::Ptr QueriesAndChunks::addQueryId(QueryId qId, CzarIdType czarId) {
    unique_lock<mutex> guardStats(_queryStatsMapMtx);
    return _addQueryId(qId, czarId);
}

/// Add statistics for the Task, creating a QueryStatistics object if needed.
void QueriesAndChunks::addTask(wbase::Task::Ptr const& task) {
    auto qid = task->getQueryId();
    auto czId = task->getCzarId();
    auto stats = addQueryId(qid, czId);
    stats->addTask(task);
}
void QueriesAndChunks::addTasks(vector<wbase::Task::Ptr> const& tasks,
                                std::vector<util::Command::Ptr>& cmds) {
    unique_lock<mutex> guardStats(_queryStatsMapMtx);
    for (auto const& task : tasks) {
        auto qid = task->getQueryId();
        auto czId = task->getCzarId();
        auto stats = _addQueryId(qid, czId);
        stats->addTask(task);
        cmds.push_back(task);
    }
}

/// Update statistics for the Task that was just queued.
void QueriesAndChunks::queuedTask(wbase::Task::Ptr const& task) {
    auto now = chrono::system_clock::now();
    task->queued(now);

    QueryStatistics::Ptr stats = getStats(task->getQueryId());
    if (stats != nullptr) {
        stats->addTask(now);
    }
}

/// Update statistics for the Task that just started.
void QueriesAndChunks::startedTask(wbase::Task::Ptr const& task) {
    auto now = chrono::system_clock::now();
    task->started(now);

    QueryStatistics::Ptr stats = getStats(task->getQueryId());
    if (stats != nullptr) {
        stats->addTaskRunning(now);
    } else {
        LOGS(_log, LOG_LVL_ERROR, __func__ << " stats was nullptr");
    }
}

/// Update statistics for the Task that finished and the chunk it was querying.
void QueriesAndChunks::finishedTask(wbase::Task::Ptr const& task) {
    auto now = chrono::system_clock::now();
    double taskDuration = (double)(task->finished(now).count());
    taskDuration /= 60000.0;  // convert to minutes.

    QueryId qId = task->getQueryId();
    QueryStatistics::Ptr stats = getStats(qId);
    if (stats != nullptr) {
        bool mostlyDead = stats->addTaskCompleted(now, taskDuration);
        if (mostlyDead) {
            lock_guard<mutex> gd(_newlyDeadMtx);
            (*_newlyDeadQueries)[qId] = stats;
        }
    }
    if (task->isBooted()) {
        // _bootedTaskTracker is only interested in system load.
        // Once a task has finished running, it has little effect
        // on system load and needs to be removed from the tracker.
        _bootedTaskTracker.removeTask(task);
    }

    _finishedTaskForChunk(task, taskDuration);
}

/// Update statistics for the Task that finished and the chunk it was querying.
void QueriesAndChunks::_finishedTaskForChunk(wbase::Task::Ptr const& task, double minutes) {
    unique_lock<mutex> ul(_chunkMtx);
    pair<int, ChunkStatistics::Ptr> ele(task->getChunkId(), nullptr);
    auto res = _chunkStats.insert(ele);
    if (res.second) {
        res.first->second = make_shared<ChunkStatistics>(task->getChunkId());
    }
    ul.unlock();
    auto iter = res.first->second;
    protojson::ScanInfo::Ptr scanInfo = task->getScanInfo();
    string tblName;
    if (!scanInfo->infoTables.empty()) {
        protojson::ScanTableInfo& sti = scanInfo->infoTables.at(0);
        tblName = ChunkTableStats::makeTableName(sti.db, sti.table);
    }
    ChunkTableStats::Ptr tableStats = iter->add(tblName, minutes);
}

/// Go through the list of possibly dead queries and remove those that are too old.
void QueriesAndChunks::removeDead() {
    vector<QueryStatistics::Ptr> dList;
    auto now = chrono::system_clock::now();
    {
        shared_ptr<DeadQueriesType> newlyDead;
        {
            lock_guard<mutex> gnd(_newlyDeadMtx);
            newlyDead = _newlyDeadQueries;
            _newlyDeadQueries.reset(new DeadQueriesType);
        }

        lock_guard<mutex> gd(_deadMtx);
        // Copy newlyDead into dead.
        for (auto const& elem : *newlyDead) {
            _deadQueries[elem.first] = elem.second;
        }
        LOGS(_log, LOG_LVL_DEBUG, "QueriesAndChunks::removeDead deadQueries size=" << _deadQueries.size());
        auto iter = _deadQueries.begin();
        while (iter != _deadQueries.end()) {
            auto const& statPtr = iter->second;
            if (statPtr->isDead(_deadAfter, now)) {
                LOGS(_log, LOG_LVL_TRACE, "QueriesAndChunks::removeDead added to list");
                dList.push_back(statPtr);
                iter = _deadQueries.erase(iter);
            } else {
                ++iter;
            }
        }
    }

    for (auto const& dead : dList) {
        removeDead(dead);
    }
    if (LOG_CHECK_LVL(_log, LOG_LVL_DEBUG)) {
        lock_guard<mutex> gdend(_deadMtx);
        LOGS(_log, LOG_LVL_DEBUG,
             "QueriesAndChunks::removeDead end deadQueries size=" << _deadQueries.size());
    }
}

/// Remove a statistics for a user query.
/// Query Ids should be unique for the life of the system, so erasing
/// a qId multiple times from _queryStats should be harmless.
void QueriesAndChunks::removeDead(QueryStatistics::Ptr const& queryStats) {
    QueryId qId = queryStats->getQueryId();
    LOGS(_log, LOG_LVL_TRACE, "Queries::removeDead " << qId);

    _bootedTaskTracker.removeQuery(qId);
    lock_guard<mutex> gQ(_queryStatsMapMtx);
    _queryStatsMap.erase(qId);
}

QueryStatistics::Ptr QueriesAndChunks::getStats(QueryId qId) const {
    lock_guard<mutex> lockG(_queryStatsMapMtx);
    return _getStats(qId);
}

/// @return the statistics for a user query.
QueryStatistics::Ptr QueriesAndChunks::_getStats(QueryId const& qId) const {
    auto iter = _queryStatsMap.find(qId);
    if (iter != _queryStatsMap.end()) {
        return iter->second;
    }
    LOGS(_log, LOG_LVL_WARN, "QueriesAndChunks::getStats could not find qId=" << qId);
    return nullptr;
}

void QueriesAndChunks::examineAll() {
    // Make sure only one `examineAll()` is running at any given time.
    if (_runningExamineAll.exchange(true) == true) {
        // Already running this function.
        return;
    }

    /// Ensure that `_runningExamineAll` gets set to false.
    class SetExamineAllRunningFalse {
    public:
        SetExamineAllRunningFalse(std::atomic<bool>& runningExAll_) : _runningExAll(runningExAll_) {}
        ~SetExamineAllRunningFalse() { _runningExAll = false; }

    private:
        std::atomic<bool>& _runningExAll;
    };
    SetExamineAllRunningFalse setExamineAllRunningFalse(_runningExamineAll);

    // Need to know how long it takes to complete tasks on each table
    // in each chunk, and their percentage total of the whole.
    auto scanTblSums = _calcScanTableSums();

    // Copy a vector of the Queries in the map and work with the copy
    // to free up the mutex.
    vector<QueryStatistics::Ptr> uqStatList;
    {
        lock_guard<mutex> g(_queryStatsMapMtx);
        for (auto const& [statsQId, qStatsPtr] : _queryStatsMap) {
            uqStatList.push_back(qStatsPtr);
            LOGS(_log, LOG_LVL_TRACE, __func__ << " read stats for " << statsQId);
        }
    }

    /// Set of user queries that may be worth booting.
    set<QueryId> qIdsBootCandidates;

    // Go through all Tasks in each query and examine the running ones.
    // If a running Task is taking longer than its percent of total time, boot it.
    // Booting a Task may result in the entire user query it belongs to being moved
    // to the snail scan.
    for (auto const& uqStatElem : uqStatList) {
        // Copy all the running tasks that are on ScanSchedulers.
        vector<wbase::Task::Ptr> runningTasks = uqStatElem->getRunningTasks();

        // For each running task, check if it is taking too long, or if the query is taking too long.
        for (auto const& task : runningTasks) {
            auto const& sched = dynamic_pointer_cast<wsched::ScanScheduler>(task->getTaskScheduler());
            if (sched == nullptr) {
                continue;
            }
            double schedMaxTime = sched->getMaxTimeMinutes();  // Get max time for scheduler
            // Get the slowest scan table in task.
            auto begin = task->getScanInfo()->infoTables.begin();
            if (begin == task->getScanInfo()->infoTables.end()) {
                continue;
            }
            string const& slowestTable = begin->db + ":" + begin->table;
            auto iterTbl = scanTblSums.find(slowestTable);
            if (iterTbl != scanTblSums.end()) {
                LOGS(_log, LOG_LVL_DEBUG,
                     "examineAll a " << slowestTable << " chunkId=" << task->getChunkId() << " "
                                     << task->getIdStr());
                ScanTableSums& tblSums = iterTbl->second;

                // TODO: The size and expected percentage of chunks needs to be gathered from
                // replicator data and/or the czar.
                // This code pre-dates the replicator, so it constructs
                // the tables as they are used by tasks. This causes serious issues
                // with analyzing queries the first time the table is seen. Skipping analysis
                // for slow tasks on large tables is particularly brutal, so this assumes
                // that the table for the first chunk in the list is a close enough
                // approximation for the percentage.
                // Use the correct chunk id, if it can be found. Otherwise, use the first
                // chunk in the list as a reference. Use whatever value is found.
                auto iterChunk = tblSums.chunkPercentages.find(task->getChunkId());
                if (iterChunk == tblSums.chunkPercentages.end()) {
                    iterChunk = tblSums.chunkPercentages.begin();
                }
                if (iterChunk != tblSums.chunkPercentages.end()) {
                    LOGS(_log, LOG_LVL_DEBUG,
                         "examineAll c " << slowestTable << " chunkId=" << task->getChunkId() << " "
                                         << task->getIdStr());
                    // We can only make the check if there's data on past chunks/tables.
                    double percent = iterChunk->second.percent;
                    bool valid = iterChunk->second.valid;
                    // Allow for some variation in time, also need twice as much time for 2 replicas.
                    double leeway = 2.25;
                    double maxTimeChunk = percent * schedMaxTime * leeway;
                    auto runTimeMilli = task->getRunTime();
                    double runTimeMinutes = (double)runTimeMilli.count() / 60000.0;
                    bool booting = runTimeMinutes > maxTimeChunk && valid;
                    auto lvl = booting ? LOG_LVL_WARN : LOG_LVL_DEBUG;
                    LOGS(_log, lvl,
                         "examineAll " << (booting ? "booting" : "keeping") << " task " << task->getIdStr()
                                       << "maxTimeChunk(" << maxTimeChunk << ")=percent(" << percent
                                       << ")*schedMaxTime(" << schedMaxTime << ")"
                                       << " runTimeMinutes=" << runTimeMinutes << " valid=" << valid);
                    if (booting && !task->atMaxThreadCount()) {
                        LOGS(_log, lvl,
                             __func__ << " booting runtime=" << runTimeMinutes << " max=" << maxTimeChunk
                                      << " " << task->getIdStr());
                        _bootTask(uqStatElem, task, sched);
                    }
                }
            }
        }
    }

    _bootUserQueries();

    LOGS(_log, LOG_LVL_WARN, util::HoldTrack::CheckKeySet());
    LOGS(_log, LOG_LVL_DEBUG, "QueriesAndChunks::examineAll end");
}

nlohmann::json QueriesAndChunks::statusToJson(wbase::TaskSelector const& taskSelector) const {
    nlohmann::json status = nlohmann::json::object();
    {
        auto bSched = _blendSched.lock();
        if (bSched == nullptr) {
            LOGS(_log, LOG_LVL_WARN, "blendSched undefined, can't check user query");
            status["blend_scheduler"] = nlohmann::json::object();
        } else {
            status["blend_scheduler"] = bSched->statusToJsonBlend();
        }
    }
    status["query_stats"] = nlohmann::json::object();
    lock_guard<mutex> g(_queryStatsMapMtx);
    for (auto&& itr : _queryStatsMap) {
        string const qId = to_string(itr.first);  // forcing string type for the json object key
        QueryStatistics::Ptr const& qStats = itr.second;
        status["query_stats"][qId]["histograms"] = qStats->getJsonHist();
        status["query_stats"][qId]["tasks"] = qStats->getJsonTasks(taskSelector);
    }
    return status;
}

nlohmann::json QueriesAndChunks::mySqlThread2task(set<unsigned long> const& activeMySqlThreadIds) const {
    nlohmann::json result = nlohmann::json::object();
    lock_guard<mutex> g(_queryStatsMapMtx);
    for (auto const& [key, qStats] : _queryStatsMap) {
        qStats->mySqlThread2task(activeMySqlThreadIds, result);
    }
    return result;
}

/// @return a map that contains time totals for all chunks for tasks running on specific
/// tables. The map is sorted by table name and contains sub-maps ordered by chunk id.
/// The sub-maps contain information about how long tasks take to complete on that table
/// on that chunk. These are then used to see what the percentage total of the time it took
/// for tasks on each chunk.
/// The table names are based on the slowest scan table in each task.
QueriesAndChunks::ScanTableSumsMap QueriesAndChunks::_calcScanTableSums() {
    QueriesAndChunks::ScanTableSumsMap scanTblSums;

    // Copy a vector of all the chunks in the map;
    vector<ChunkStatistics::Ptr> chks;
    {
        lock_guard<mutex> g(_chunkMtx);
        for (auto const& ele : _chunkStats) {
            auto const& chk = ele.second;
            chks.push_back(chk);
        }
    }

    bool useTimeStatisticsForBoot = false;
    // Sum up the total time for handling the slowest table (aka largest table)
    // in this chunk. The slowest table values will be used for all other tables
    // in this chunk.
    for (auto const& chunkStats : chks) {
        auto chunkId = chunkStats->_chunkId;
        lock_guard<mutex> lock(chunkStats->_tStatsMtx);
        for (auto const& ele : chunkStats->_tableStats) {
            auto const& tblName = ele.first;
            if (!tblName.empty()) {
                auto& sTSums = scanTblSums[tblName];
                auto data = ele.second->getData();

                // Total time for `tblName` to run on all chunks used on this worker.
                // `tblName` is the biggest table in its chunk.
                sTSums.totalTime += data.avgCompletionTime;
                ChunkTimePercent& ctp = sTSums.chunkPercentages[chunkId];
                ctp.shardTime = data.avgCompletionTime;
                ctp.valid = data.tasksCompleted >= _requiredTasksCompleted;
            }
        }
    }

    int numberOfChunks = chks.size();
    double percentPerChunk = 1.0 / numberOfChunks;

    // Calculate percentages of time for the slowest table to finish
    // the current chunk, compared to the totalTime to run that table
    // on all chunks.
    for (auto& eleTbl : scanTblSums) {
        auto& scanTbl = eleTbl.second;

        // totalTime calculated above.
        auto totalTime = scanTbl.totalTime;
        for (auto& eleChunk : scanTbl.chunkPercentages) {
            auto& tPercent = eleChunk.second;
            if (useTimeStatisticsForBoot) {
                tPercent.percent = tPercent.shardTime / totalTime;
            } else {
                tPercent.percent = percentPerChunk;
                tPercent.valid = true;
            }
        }
    }

    return scanTblSums;
}

void QueriesAndChunks::_bootTask(QueryStatistics::Ptr const& uq, wbase::Task::Ptr const& task,
                                 wsched::SchedulerBase::Ptr const& sched) {
    LOGS(_log, LOG_LVL_INFO, "taking too long, booting from " << sched->getName());
    auto bSched = _blendSched.lock();
    if (bSched == nullptr) {
        LOGS(_log, LOG_LVL_WARN, __func__ << " blendSched==null, can't boot task=" << task->getIdStr());
        return;
    }
    if (bSched->isScanSnail(sched)) {
        LOGS(_log, LOG_LVL_WARN, __func__ << " can't boot task, already on snail " << task->getIdStr());
        return;
    }
    sched->removeTask(task, true);
    bool alreadyBooted = task->setBooted();
    if (alreadyBooted) {
        LOGS(_log, LOG_LVL_WARN, __func__ << task->getIdStr() << " was already booted");
        return;
    }
    uq->addTaskBooted();
    _bootedTaskTracker.addTask(task);
    return;
}

void QueriesAndChunks::_bootUserQueries() {
    auto bSched = _blendSched.lock();
    if (bSched == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "blendSched undefined, can't check user query");
        return;
    }
    // Track the total number of Tasks that are running while booted
    // and move the worst offenders to the snailScan. The worst offender is the UserQuery with
    // the most `Task`s running that are taking too long.
    // The following function only counts booted `Task`s that are still EXECUTING,
    // and `countQIdVect` is order by worst offender to least worst offender.
    auto [bootedTaskCount, countQIdVect] = _bootedTaskTracker.getTotalBootedTaskCount();
    // Sum of Tasks in
    int uqBootedTasksSum = 0;
    if (bootedTaskCount > _maxDarkTasks) {
        for (auto const& countQId : countQIdVect) {
            // If enough UserQueries have been booted to get under the Task limit,
            // stop booting UserQueries.
            bool enoughTasksBooted = (bootedTaskCount - uqBootedTasksSum) <= _maxDarkTasks;
            LOGS(_log, LOG_LVL_DEBUG,
                 __func__ << " a uq=" << countQId.qId << " enough=" << enoughTasksBooted
                          << " bootedTaskCount=" << bootedTaskCount
                          << " uqBootedTasksSum=" << uqBootedTasksSum
                          << " sub=" << (bootedTaskCount - uqBootedTasksSum) << " max=" << _maxDarkTasks);
            if (enoughTasksBooted) {
                break;
            }

            // Get query info
            QueryId qIdToBoot = countQId.qId;
            QueryStatistics::Ptr queryToBoot = getStats(qIdToBoot);
            if (queryToBoot != nullptr) {
                if (_bootUserQuery(queryToBoot, bSched)) {
                    uqBootedTasksSum += countQId.count;
                    LOGS(_log, LOG_LVL_DEBUG,
                         __func__ << " a booted uq=" << countQId.qId << ":count=" << countQId.count
                                  << " uqBootedTasksSum=" << uqBootedTasksSum);
                }
            } else {
                // This really should never happen.
                LOGS(_log, LOG_LVL_ERROR, __func__ << " queryToBoot is nullptr qIdToBoot=" << countQId.qId);
            }
        }
    }

    // Last check, if any of the queries have several booted `Task`s but have not been booted,
    // they should be booted now.
    for (auto const& countQId : countQIdVect) {
        QueryId qIdToCheck = countQId.qId;
        // Get query info
        QueryStatistics::Ptr queryToCheck = getStats(qIdToCheck);
        if (queryToCheck == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, __func__ << " Couldn't locate qIdToCheck=" << qIdToCheck);
            // Probably never happens, but may be a possibility during query cancellation.
            // Try to boot the next query in the list.
            continue;
        }

        if (!queryToCheck->getQueryBooted()) {
            auto tasksBooted = queryToCheck->getTasksBooted();
            LOGS(_log, LOG_LVL_DEBUG,
                 __func__ << " b check uq=" << countQId.qId << " tasksBooted=" << tasksBooted
                          << " max=" << (_maxTasksBooted));
            if (tasksBooted > _maxTasksBooted) {
                QueryStatistics::Ptr queryToBoot = getStats(qIdToCheck);
                if (_bootUserQuery(queryToBoot, bSched)) {
                    uqBootedTasksSum += countQId.count;
                    LOGS(_log, LOG_LVL_DEBUG,
                         __func__ << " b booted uq=" << countQId.qId << ":count=" << countQId.count
                                  << " tasksBooted=" << tasksBooted
                                  << " uqBootedTasksSum=" << uqBootedTasksSum);
                }
            }
        }
    }
    return;
}

bool QueriesAndChunks::_bootUserQuery(QueryStatistics::Ptr queryToBoot,
                                      wsched::BlendScheduler::Ptr const& bSched) {
    // Get query info
    if (queryToBoot == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, __func__ << " queryToBoot is nullptr");
        // Probably never happens, but may be a possibility during query cancellation.
        return false;
    }

    bool queryWasBooted = queryToBoot->getQueryBooted();

    // If the UserQuery was already booted, just add its booted `Task`s to the sum.
    if (queryWasBooted) {
        return true;
    }

    // It may be possible for Tasks associated with a QueryId to be on more than 1 scheduler.
    QueryStatistics::SchedTasksInfoMap schedTaskMap = queryToBoot->getSchedulerTasksInfoMap();
    for (auto const& [key, schedInfo] : schedTaskMap) {
        wsched::SchedulerBase::Ptr schedTarget = schedInfo.scheduler.lock();
        if (schedTarget == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, __func__ << " schedTarg was nullptr for key=" << key);
            continue;
        }
        if (bSched->isScanSnail(schedTarget)) {
            LOGS(_log, LOG_LVL_DEBUG, __func__ << " schedTarg was snailScan for key=" << key);
            continue;
        }

        LOGS(_log, LOG_LVL_INFO, "Booting uq queryId=" << queryToBoot->queryId);
        queryToBoot->setQueryBooted(true, CLOCK::now());
        bSched->moveUserQueryToSnail(queryToBoot->queryId, schedTarget);
        queryWasBooted = true;
        LOGS(_log, LOG_LVL_DEBUG, __func__ << " uq=" << queryToBoot->queryId);
    }
    return queryWasBooted;
}

/// Remove all Tasks belonging to the user query 'qId' from the queue of 'sched'.
/// If 'sched' is null, then Tasks will be removed from the queue of whatever scheduler
/// they are queued on.
/// Tasks that are already running continue, but are marked as complete on their
/// current scheduler. Stopping and rescheduling them would be difficult at best, and
/// probably not very helpful.
vector<wbase::Task::Ptr> QueriesAndChunks::removeQueryFrom(QueryId const& qId,
                                                           wsched::SchedulerBase::Ptr const& sched) {
    vector<wbase::Task::Ptr> removedList;  // Return value;

    // Find the user query.
    auto query = getStats(qId);
    if (query == nullptr) {
        LOGS(_log, LOG_LVL_DEBUG, "was not found by removeQueryFrom");
        return removedList;
    }

    // Remove Tasks from their scheduler put them on 'removedList', but only if their Scheduler is the same
    // as 'sched' or if sched == nullptr.
    vector<wbase::Task::Ptr> taskList = query->getTaskList();

    // Lambda function to do the work of moving tasks.
    auto moveTasks = [&sched, &taskList, &removedList](bool moveRunning) {
        vector<wbase::Task::Ptr> taskListNotRemoved;
        for (auto const& task : taskList) {
            auto taskSched = task->getTaskScheduler();
            if (taskSched != nullptr && (taskSched == sched || sched == nullptr)) {
                // Returns true only if the task still needs to be scheduled.
                if (taskSched->removeTask(task, moveRunning)) {
                    removedList.push_back(task);
                } else {
                    taskListNotRemoved.push_back(task);
                }
            }
        }
        taskList = taskListNotRemoved;
    };

    // Remove as many non-running tasks as possible from the scheduler queue. This
    // needs to be done before removing running tasks to avoid a race condition where
    // tasks are pulled off the scheduler queue every time a running one is removed.
    moveTasks(false);

    // Remove all remaining tasks. Most likely all will be running.
    moveTasks(true);

    return removedList;
}

void QueriesAndChunks::killAllQueriesFromCzar(CzarIdType czarId) {
    std::map<QueryId, QueryStatistics::Ptr> qsMap;
    {
        lock_guard<mutex> lgQsm(_queryStatsMapMtx);
        qsMap = _queryStatsMap;
    }

    for (auto const& [qsKey, qsPtr] : qsMap) {
        if (qsPtr != nullptr) {
            auto uqInfo = qsPtr->getUserQueryInfo();
            if (uqInfo != nullptr && uqInfo->getCzarId() == czarId) {
                uqInfo->cancelAllUberJobs();
            }
        }
    }
}

ostream& operator<<(ostream& os, QueriesAndChunks const& qc) {
    lock_guard<mutex> g(qc._chunkMtx);
    os << "Chunks(";
    for (auto const& ele : qc._chunkStats) {
        os << *(ele.second) << ";";
    }
    os << ")";
    return os;
}

/// Add the duration to the statistics for the table. Create a statistics object if needed.
/// @return the statistics for the table.
ChunkTableStats::Ptr ChunkStatistics::add(string const& scanTableName, double minutes) {
    pair<string, ChunkTableStats::Ptr> ele(scanTableName, nullptr);
    unique_lock<mutex> ul(_tStatsMtx);
    auto res = _tableStats.insert(ele);
    auto iter = res.first;
    if (res.second) {
        iter->second = make_shared<ChunkTableStats>(_chunkId, scanTableName);
    }
    ul.unlock();
    iter->second->addTaskFinished(minutes);
    return iter->second;
}

/// @return the statistics for a table. nullptr if the table is not found.
ChunkTableStats::Ptr ChunkStatistics::getStats(string const& scanTableName) const {
    lock_guard<mutex> g(_tStatsMtx);
    auto iter = _tableStats.find(scanTableName);
    if (iter != _tableStats.end()) {
        return iter->second;
    }
    return nullptr;
}

ostream& operator<<(ostream& os, ChunkStatistics const& cs) {
    lock_guard<mutex> g(cs._tStatsMtx);
    os << "ChunkStatsistics(" << cs._chunkId << "(";
    for (auto const& ele : cs._tableStats) {
        os << *(ele.second) << ";";
    }
    os << ")";
    return os;
}

/// Use the duration of the last Task completed to adjust the average completion time.
void ChunkTableStats::addTaskFinished(double minutes) {
    lock_guard<mutex> g(_dataMtx);
    ++_data.tasksCompleted;
    if (_data.tasksCompleted > 1) {
        _data.avgCompletionTime = (_data.avgCompletionTime * _weightAvg + minutes * _weightNew) / _weightSum;
    } else {
        _data.avgCompletionTime = minutes;
    }
    LOGS(_log, LOG_LVL_TRACE,
         "ChkId=" << _chunkId << ":tbl=" << _scanTableName << " completed=" << _data.tasksCompleted
                  << " avgCompletionTime=" << _data.avgCompletionTime);
}

ostream& operator<<(ostream& os, ChunkTableStats const& cts) {
    lock_guard<mutex> g(cts._dataMtx);
    os << "ChunkTableStats(" << cts._chunkId << ":" << cts._scanTableName
       << " tasks(completed=" << cts._data.tasksCompleted << ",avgTime=" << cts._data.avgCompletionTime
       << ",booted=" << cts._data.tasksBooted << "))";
    return os;
}

void BootedTaskTracker::addTask(wbase::Task::Ptr const& task) {
    lock_guard<mutex> lockG(_bootedMapMtx);
    QueryId qId = task->getQueryId();
    MapOfTasks& mapOfTasks = _bootedMap[qId];
    mapOfTasks[task->getTSeq()] = task;
}

void BootedTaskTracker::removeTask(wbase::Task::Ptr const& task) {
    lock_guard<mutex> lockG(_bootedMapMtx);
    QueryId qId = task->getQueryId();
    auto iter = _bootedMap.find(qId);
    if (iter != _bootedMap.end()) {
        MapOfTasks& setOfTasks = iter->second;
        setOfTasks.erase(task->getTSeq());
    }
}

void BootedTaskTracker::removeQuery(QueryId qId) {
    lock_guard<mutex> lockG(_bootedMapMtx);
    _bootedMap.erase(qId);
}

pair<int, vector<BootedTaskTracker::CountQId>> BootedTaskTracker::getTotalBootedTaskCount() const {
    lock_guard<mutex> lockG(_bootedMapMtx);
    int taskCount = 0;
    vector<CountQId> countQId;
    for (auto const& [key, mapOfTasks] : _bootedMap) {
        auto sz = mapOfTasks.size();
        if (sz > 0) {
            taskCount += sz;
            countQId.emplace_back(sz, key);
        }
    }
    // Sort to descending order of booted `Task`s per UserQuery.
    sort(countQId.begin(), countQId.end(), greater<CountQId>());

    if (LOG_CHECK_DEBUG()) {
        string msg;
        for (auto const& cQId : countQId) {
            msg += " (qId=" + to_string(cQId.qId) + " count=" + to_string(cQId.count) + ")";
        }
        LOGS(_log, LOG_LVL_DEBUG, __func__ << " total=" << taskCount << " order " << msg);
    }
    return {taskCount, countQId};
}

}  // namespace lsst::qserv::wpublish
