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

#ifndef LSST_QSERV_WPUBLISH_QUERIESANDCHUNKS_H
#define LSST_QSERV_WPUBLISH_QUERIESANDCHUNKS_H

// System headers
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <set>
#include <string>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "global/intTypes.h"
#include "wbase/Task.h"
#include "wpublish/QueryStatistics.h"

// Forward declarations
namespace lsst::qserv::wbase {
struct TaskSelector;
}  // namespace lsst::qserv::wbase

namespace lsst::qserv::wsched {
class SchedulerBase;
class BlendScheduler;
class ScanScheduler;
}  // namespace lsst::qserv::wsched

namespace lsst::qserv::wpublish {
class QueriesAndChunks;
}  // namespace lsst::qserv::wpublish

// This header declarations
namespace lsst::qserv::wpublish {

/// Statistics for a table in a chunk. Statistics are based on the slowest table in a query,
/// so this most likely includes values for queries on _scanTableName and queries that join
/// against _scanTableName. Source is slower than Object, so joins with Source and Object will
/// have their statistics logged with Source.
class ChunkTableStats {
public:
    using Ptr = std::shared_ptr<ChunkTableStats>;

    /// Contains statistics data for this table in this chunk.
    struct Data {
        std::uint64_t tasksCompleted = 0;  ///< Number of Tasks that have completed on this chunk/table.
        std::uint64_t tasksBooted = 0;     ///< Number of Tasks that have been booted for taking too long.
        double avgCompletionTime = 0.0;    ///< weighted average of completion time in minutes.
    };

    static std::string makeTableName(std::string const& db, std::string const& table) {
        return db + ":" + table;
    }

    ChunkTableStats(int chunkId, std::string const& name) : _chunkId{chunkId}, _scanTableName{name} {}

    void addTaskFinished(double minutes);

    /// @return a copy of the statics data.
    Data getData() {
        std::lock_guard<std::mutex> g(_dataMtx);
        return _data;
    }

    friend std::ostream& operator<<(std::ostream& os, ChunkTableStats const& cts);

private:
    mutable std::mutex _dataMtx;  ///< Protects _data.
    int const _chunkId;
    std::string const _scanTableName;

    Data _data;                                   ///< Statistics for this table in this chunk.
    double _weightAvg = 49.0;                     ///< weight of previous average
    double _weightNew = 1.0;                      ///< weight of new measurement
    double _weightSum = _weightAvg + _weightNew;  ///< denominator
};

/// Statistics for one chunk, including scan table statistics.
class ChunkStatistics {
public:
    using Ptr = std::shared_ptr<ChunkStatistics>;

    ChunkStatistics(int chunkId) : _chunkId{chunkId} {}

    ChunkTableStats::Ptr add(std::string const& scanTableName, double duration);
    ChunkTableStats::Ptr getStats(std::string const& scanTableName) const;

    friend QueriesAndChunks;
    friend std::ostream& operator<<(std::ostream& os, ChunkStatistics const& cs);

private:
    int const _chunkId;
    mutable std::mutex _tStatsMtx;  ///< protects _tableStats;
    /// Map of chunk scan table statistics indexed by slowest scan table name in query.
    std::map<std::string, ChunkTableStats::Ptr> _tableStats;
};

/// This class tracks the tasks that have been booted from their scheduler and are
/// still running. The tasks are grouped by their related QueryId.
class BootedTaskTracker {
public:
    /// Map of weak Task pointers organized by Task::_tSeq.
    using MapOfTasks = std::map<uint64_t, std::weak_ptr<wbase::Task>>;

    /// Class to sort UserQueries by number of booted `Task`s.
    class CountQId {
    public:
        CountQId() = delete;
        CountQId(size_t count_, QueryId qId_) : count(count_), qId(qId_) {}
        bool operator<(CountQId const& other) const {
            if (count == other.count) {
                return qId < other.qId;
            }
            return count < other.count;
        }
        bool operator>(CountQId const& other) const { return other < *this; }
        size_t count;
        QueryId qId;
    };

    BootedTaskTracker() = default;
    BootedTaskTracker(BootedTaskTracker const&) = default;

    void addTask(wbase::Task::Ptr const& task);
    void removeTask(wbase::Task::Ptr const& task);
    void removeQuery(QueryId qId);  ///< Remove a `QueryId` and all associated `Task`s from `_bootedMap`.

    /// Return a count of all tasks booted from all queries in `_bootedMap` and
    /// a vector of `QueryId`s that is sorted to have the UserQueries with the
    /// most booted `Task`s at the front.
    std::pair<int, std::vector<CountQId>> getTotalBootedTaskCount() const;

private:
    /// Map of booted tasks that are still running organized by `QueryId`, `Task::_tSeq`.
    std::map<QueryId, MapOfTasks> _bootedMap;
    mutable std::mutex _bootedMapMtx;  ///< protects `_bootedMap`.
};

class QueriesAndChunks {
public:
    using Ptr = std::shared_ptr<QueriesAndChunks>;

    /// Setup the global instance and return a pointer to it.
    /// @param deadAfter - consider a user query to be dead after this number of seconds.
    /// @param examineAfter - examine all know tasks after this much time has passed since the last
    ///                 examineAll() call
    /// @param maxTasksBooted - after this many tasks have been booted, the query should be
    ///                 moved to the snail scheduler.
    /// @param resetForTesting - set this to true ONLY if the class needs to be reset for unit testing.
    static Ptr setupGlobal(std::chrono::seconds deadAfter, std::chrono::seconds examineAfter,
                           int maxTasksBooted, int maxDarkQueries, bool resetForTesting);

    /// Return the pointer to the global object.
    /// @param noThrow - if true, this will not throw an exception when called and
    ///                  the function can return nullptr. This should only be true in unit testing.
    /// @throws - throws util::Bug if setupGlobal() has not been called before this.
    static Ptr get(bool noThrow = false);

    virtual ~QueriesAndChunks();

    void setBlendScheduler(std::shared_ptr<wsched::BlendScheduler> const& blendsched);
    void setRequiredTasksCompleted(unsigned int value);

    std::vector<wbase::Task::Ptr> removeQueryFrom(QueryId const& qId,
                                                  std::shared_ptr<wsched::SchedulerBase> const& sched);
    void removeDead();
    void removeDead(QueryStatistics::Ptr const& queryStats);

    /// Return the statistics for a user query, may be nullptr,
    /// in many cases addQueryId() may be preferable if
    /// new information is being added to the returned object.
    /// @see addQueryId()
    QueryStatistics::Ptr getStats(QueryId qId) const;

    /// @see _addQueryId
    QueryStatistics::Ptr addQueryId(QueryId qId, CzarIdType czarId);

    void addTask(wbase::Task::Ptr const& task);
    void addTasks(std::vector<wbase::Task::Ptr> const& tasks, std::vector<util::Command::Ptr>& cmds);
    void queuedTask(wbase::Task::Ptr const& task);
    void startedTask(wbase::Task::Ptr const& task);
    void finishedTask(wbase::Task::Ptr const& task);

    /// Examine all running Tasks and boot Tasks that are taking too long and
    /// move user queries that are too slow to the snail scan.
    /// This is expected to be called maybe once every 5 minutes.
    void examineAll();

    /**
     * Retreive monitoring data for the worker.
     * @param taskSelector Task selection criterias.
     * @return a JSON representation of the object's status for the monitoring
     */
    nlohmann::json statusToJson(wbase::TaskSelector const& taskSelector) const;

    /**
     * Retrieve info on tasks that are associated with the specified MySQL threads.
     * @param activeMySqlThreadIds A collection of the MySQL threads.
     * @return a JSON object linking the threads to the corresponding tasks.
     */
    nlohmann::json mySqlThread2task(std::set<unsigned long> const& activeMySqlThreadIds) const;

    // Figure out each chunkTable's percentage of time.
    // Store average time for a task to run on this table for this chunk.
    struct ChunkTimePercent {
        double shardTime = 0.0;
        double percent = 0.0;
        bool valid = false;
    };
    // Store the time to scan entire table with time for each chunk within that table.
    struct ScanTableSums {
        double totalTime = 0.0;
        std::map<int, ChunkTimePercent> chunkPercentages;
    };
    using ScanTableSumsMap = std::map<std::string, ScanTableSums>;

    /// If the worker believes this czar has died, it calls this to stop
    /// all Tasks associated with that czar.
    void killAllQueriesFromCzar(CzarIdType czarId);

    friend std::ostream& operator<<(std::ostream& os, QueriesAndChunks const& qc);

private:
    static Ptr _globalQueriesAndChunks;
    QueriesAndChunks(std::chrono::seconds deadAfter, std::chrono::seconds examineAfter);

    /// Return the statistics for a user query, creating if needed.
    /// Since it is possible to get messages out of order, there
    /// are several case where something like a cancellation
    /// message arrives before any tasks have been created.
    /// @see getStats()
    /// _queryStatsMapMtx must be locked before calling.
    QueryStatistics::Ptr _addQueryId(QueryId qId, CzarIdType czarId);

    /// @return the statistics for a user query.
    /// _queryStatsMtx must be locked before calling.
    QueryStatistics::Ptr _getStats(QueryId const& qId) const;

    /// Remove the running 'task' from a scheduler and possibly move all Tasks that belong to its user query
    /// to the snail scheduler. 'task' continues to run in its thread, but the scheduler is told 'task' is
    /// finished, which allows the scheduler to move on to another Task.
    /// If too many Tasks from the same user query are booted, all remaining tasks are moved to the snail
    /// scheduler in an attempt to keep a single user query from jamming up a scheduler.
    void _bootTask(QueryStatistics::Ptr const& uq, wbase::Task::Ptr const& task,
                   std::shared_ptr<wsched::SchedulerBase> const& sched);

    /// Examine the UserQueries in `_bootedTaskTracker` and try to boot the biggest offending
    /// UserQueries.
    /// If there are too many booted tasks running without a scheduler, this keeps booting
    /// the UserQuery with the most booted `Task`s until the number of remaining
    /// booted tasks is well below the limit.
    void _bootUserQueries();

    /// Remove all the `Task`s from `queryToBoot` from their current scheduler and place
    /// them on the `SnailSched`.
    /// @param `queryToBoot` - The UserQuery to boot.
    /// @param `bSched` - a pointer to the `BlendScheduler`.
    /// @return - Return true if the UserQuery was successfully booted.
    bool _bootUserQuery(QueryStatistics::Ptr queryToBoot,
                        std::shared_ptr<wsched::BlendScheduler> const& bSched);

    ScanTableSumsMap _calcScanTableSums();
    void _finishedTaskForChunk(wbase::Task::Ptr const& task, double minutes);

    mutable std::mutex _queryStatsMapMtx;                    ///< protects _queryStats;
    std::map<QueryId, QueryStatistics::Ptr> _queryStatsMap;  ///< Map of Query stats indexed by QueryId.

    mutable std::mutex _chunkMtx;
    std::map<int, ChunkStatistics::Ptr> _chunkStats;  ///< Map of Chunk stats indexed by chunk id.

    std::weak_ptr<wsched::BlendScheduler> _blendSched;  ///< Pointer to the BlendScheduler.

    // Query removal thread members. A user query is dead if all its tasks are complete and it hasn't
    // been touched for a period of time.
    std::thread _removalThread;
    std::atomic<bool> _loopRemoval{true};  ///< While true, check to see if any Queries can be removed.
    /// A user query must be complete and inactive this long before it can be considered dead.
    std::chrono::seconds _deadAfter = std::chrono::minutes(5);

    std::mutex _deadMtx;       ///< Protects _deadQueries.
    std::mutex _newlyDeadMtx;  ///< Protects _newlyDeadQueries.
    using DeadQueriesType = std::map<QueryId, QueryStatistics::Ptr>;
    DeadQueriesType _deadQueries;  ///< Map of user queries that might be dead.
    std::shared_ptr<DeadQueriesType> _newlyDeadQueries{new DeadQueriesType()};

    // Members for running a separate thread to examine all the running Tasks on the scan schedulers
    // and remove those that are taking too long (boot them). If too many Tasks in a single user query
    // take too long, move all remaining task to the snail scan.
    // Booted Tasks are removed from he scheduler they were on but the Tasks should complete. Booting
    // them allows the scheduler to move onto other queries.
    std::thread _examineThread;
    std::atomic<bool> _loopExamine{true};
    std::chrono::seconds _examineAfter = std::chrono::minutes(5);

    /// Maximum number of tasks that can be booted until entire UserQuery is put on snailScan.
    /// This should be set by the config with "scheduler.maxtasksbootedperuserquery"
    int _maxTasksBooted = 5;

    /// Maximum number of booted `Task`s allowed to be running at a given time.
    /// This should be set by the config with "scheduler.maxconcurrentbootedtasks"
    int _maxDarkTasks = 25;

    /// Number of completed Tasks needed before ChunkTableStats::_avgCompletionTime can be
    /// considered valid enough to boot a Task.
    unsigned int _requiredTasksCompleted = 50;
    // TODO: remove _requiredTasksCompleted when real chunk and scan info is available.

    BootedTaskTracker _bootedTaskTracker;  ///< Keeps track of booted Tasks.

    std::atomic<bool> _runningExamineAll{false};  ///< Latch to only allow one call to `examineAll` at a time.
};

}  // namespace lsst::qserv::wpublish

#endif  // LSST_QSERV_WPUBLISH_QUERIESANDCHUNKS_H
