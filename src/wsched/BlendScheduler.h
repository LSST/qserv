// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2016 LSST Corporation.
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
#ifndef LSST_QSERV_WSCHED_BLENDSCHEDULER_H
#define LSST_QSERV_WSCHED_BLENDSCHEDULER_H

// System headers
#include <map>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "wpublish/QueriesAndChunks.h"
#include "wsched/SchedulerBase.h"

// Forward declarations
namespace lsst::qserv::wsched {
class GroupScheduler;
class ScanScheduler;
}  // namespace lsst::qserv::wsched

namespace lsst::qserv::wsched {

// Class to pass control commands to the pool thread.
// The scheduler doesn't know what to do with commands that aren't associated with
// a Task, such as commands to stop a thread. Those commands are put on this
// queue and run when the Task scheduler has nothing to run.
class ControlCommandQueue {
public:
    void queCmd(util::Command::Ptr const& cmd);
    util::Command::Ptr getCmd();
    bool ready();

private:
    std::deque<util::Command::Ptr> _qu{};
    std::mutex _mx{};
};

/// BlendScheduler is a scheduler that places queries in one of
/// 4 sub-schedulers. Interactive queries are placed on the GroupScheduler
/// _group, which has the highest priority. Other queries, which are
/// expected to require all, or most, of the chunks on this node, go
/// to one of the ScanSchedulers: _scanFast, _scanMedium, _scanSlow.
/// The priority is _group, _scanFast, _scanMedium, _scaneSlow. This
/// should match the list in _schedulers.
///
/// There are several constraints BlendSheduler places on the sub-schedulers.
/// The schedulers run Tasks in a limited pool of threads. At any time,
/// all sub-schedulers should be able to run at least one thread. This is to
/// keep sub-schedulers from getting jammed by heavy loads, or prevent
/// high priority/fast sub-schedulers being stuck waiting for low priority/slow
/// sub-schedulers to finish a Task.
///
/// Each sub-scheduler wants to have some number of threads reserved for it.
/// The ScanScheduler schedulers work better with 2 Tasks running at the same time
/// as the Tasks running at the same time should be sharing some resources and
/// are unlikely to finish at the same time. The resources the 2 were
/// using remain locked when 1 stops, and a new Task that uses the same resources
/// can start immediately. If only one Task for a ScanScheduler is running,
/// and it finishes, it's resources would be unlocked, and if the next Task
/// needed those resources, it would have to lock them again.
///
/// Since we might only have a few threads available, say 12, and 3 schedulers
/// not running any Tasks, reserving 6 threads could seriously hurt throughput.
/// So, each scheduler will only reserve 1 more thread than it has Tasks inFlight,
/// leaving at most 3 threads unavailable at any given time.
///
/// Secondly, the ScanScheduler schedulers are only allowed to advance to a new chunk
/// if resources are available to read the chunk into memory, or if the sub-scheduler
/// has no Tasks inFlight.
class BlendScheduler : public wsched::SchedulerBase {
public:
    using Ptr = std::shared_ptr<BlendScheduler>;

    // This scheduler will have difficulty with less than 11 threads.
    static unsigned int getMinPoolSize() { return 11; }

    BlendScheduler(std::string const& name, wpublish::QueriesAndChunks::Ptr const& queries,
                   int subSchedMaxThreads, std::shared_ptr<GroupScheduler> const& group,
                   std::shared_ptr<ScanScheduler> const& snailScheduler,
                   std::vector<std::shared_ptr<ScanScheduler>> const& scanSchedulers);

    BlendScheduler() = delete;
    BlendScheduler(BlendScheduler const&) = delete;
    BlendScheduler& operator=(BlendScheduler const&) = delete;
    ~BlendScheduler() override = default;

    void queTaskLoad(util::Command::Ptr const& cmd);

    void queCmd(util::Command::Ptr const& cmd) override;
    void queCmd(std::vector<util::Command::Ptr> const& cmds) override;
    util::Command::Ptr getCmd(bool wait) override;

    void commandStart(util::Command::Ptr const& cmd) override;
    void commandFinish(util::Command::Ptr const& cmd) override;

    // SchedulerBase overrides methods.
    std::size_t getSize() const override;
    int getInFlight() const override;
    bool ready() override;
    int applyAvailableThreads(int tempMax) override { return tempMax; }  //< does nothing

    bool isScanSnail(SchedulerBase::Ptr const& scan);
    int moveUserQueryToSnail(QueryId qId, SchedulerBase::Ptr const& source);
    int moveUserQuery(QueryId qId, SchedulerBase::Ptr const& source, SchedulerBase::Ptr const& destination);

    void setPrioritizeByInFlight(bool val) { _prioritizeByInFlight = val; }

    /// @return a JSON representation of the object's status for the monitoring
    nlohmann::json statusToJsonBlend();

    /// Do nothing, the schedulers this class manages keep their own statistics.
    void recordPerformanceData() override {};

private:
    int _getAdjustedMaxThreads(int oldAdjMax, int inFlight);
    bool _ready();
    void _sortScanSchedulers();
    void _logChunkStatus();
    void _logSchedulers();

    /// _schedMtx must be locked before calling.
    int _calcAvailableTheads();
    ControlCommandQueue _ctrlCmdQueue;  ///< Needed for changing thread pool size.
    std::deque<util::Command::Ptr> _taskLoadQueue;

    int _schedMaxThreads;  ///< maximum number of threads that can run.

    // Sub-schedulers.
    std::shared_ptr<GroupScheduler> _group;       ///< group scheduler
    std::shared_ptr<ScanScheduler> _scanSnail;    ///< extremely slow scheduler.
    std::vector<SchedulerBase::Ptr> _schedulers;  ///< list of all schedulers including _group and _scanSnail
    mutable std::mutex _schedMtx;  ///< Protects _schedulers. NEVER lock this before util::CommandQueue::mx.

    std::atomic<bool> _infoChanged{true};  //< Used to limit debug logging.

    wpublish::QueriesAndChunks::Ptr _queries;  /// UserQuery statistics.

    /// Schedulers with more tasks inflight get lower priority.
    std::atomic<bool> _prioritizeByInFlight{false};
    SchedulerBase::Ptr _readySched;  //< Pointer to the scheduler with a ready task.

    /// Record performance data when this value is less than now(), and then this value us increased
    /// by _intervalRecordPerformanceTime;
    /// TODO: DM-??? set values from configuration, change values at runtime.
    std::chrono::system_clock::time_point _nextRecordPerformanceTime;
    std::chrono::milliseconds _intervalRecordPerformanceTime{15000};
    std::mutex _mtxRecordPerformance;  ///< protects _nextRecordPerformanceTime
};

}  // namespace lsst::qserv::wsched

extern lsst::qserv::wsched::BlendScheduler* dbgBlendScheduler;  ///< A symbol for gdb

#endif  // LSST_QSERV_WSCHED_BLENDSCHEDULER_H
