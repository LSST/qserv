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
#ifndef LSST_QSERV_CZAR_ACTIVEWORKER_H
#define LSST_QSERV_CZAR_ACTIVEWORKER_H

// System headers
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// qserv headers
#include "protojson/WorkerQueryStatusData.h"
#include "util/Bug.h"

namespace lsst::qserv::cconfig {
class CzarConfig;
}

// This header declarations
namespace lsst::qserv::czar {

/// This class is used to track information important to the czar and a
/// specific worker. Primarily the czar cares about the worker being alive
/// and informing the worker that various query IDs and UberJobs
/// have finished or need to be cancelled.
///  - maintain list of done/cancelled queries for an active worker, and send
///    that list to the worker. Once the worker has accepted the list, remove
///    all of those queryId's from the lists.
///  - maintain a list of killed UberJobs. If an UberJob is killed, nothing
///    will every look for its files, so they should be deleted, and the
///    worker should avoid working on Tasks for that UberJob.
///    The only UberJob deaths that need to be sent to a worker is when
///    the czar kills an UberJob because the worker died/vanished, and
///    the only time this would be sent is when a worker came back from
///    the dead.
///    The reason this only applies to died/vanished workers is that all
///    other workers know their UberJobs are dead because the worker killed
///    them. If the worker isn't told, it will continue working on
///    the UberJob until it finishes, and then find out the UberJob was killed
///    when it tries to return results to the czar. The worker should delete
///    files for said UberJob at that point.
///    So, this should be very rare, only results in extra load.
///
///    If a worker goes missing from the registry, it is considered DEAD and may be
///       removed after a period of time.
///    If a worker hasn't been heard from in (timeout period), it is considered QUESIONABLE.
///    If a QUESTIONABLE worker hasn't been heard from in (timeout period), its state is changed
///       to DEAD.
///
///    When a worker becomes DEAD: (see Czar::_monitor).
///       - Affected UberJobs are killed.
///       - New UberJobs are built to handle unassigned jobs where dead workers are skipped and
///         the jobs are assigned to alternate workers.
///
class ActiveWorker : public std::enable_shared_from_this<ActiveWorker> {
public:
    using Ptr = std::shared_ptr<ActiveWorker>;

    enum State { ALIVE = 0, QUESTIONABLE, DEAD };

    ActiveWorker() = delete;
    ActiveWorker(ActiveWorker const&) = delete;
    ActiveWorker& operator=(ActiveWorker const&) = delete;

    std::string cName(const char* fName) {
        auto wqsd = _wqsData;
        return std::string("ActiveWorker::") + fName + " " + ((wqsd == nullptr) ? "?" : wqsd->dump());
    }

    static std::string getStateStr(State st);

    static Ptr create(protojson::WorkerContactInfo::Ptr const& wInfo,
                      protojson::CzarContactInfo::Ptr const& czInfo, std::string const& replicationInstanceId,
                      std::string const& replicationAuthKey) {
        return Ptr(new ActiveWorker(wInfo, czInfo, replicationInstanceId, replicationAuthKey));
    }

    /// This function should only be called before the _monitor thread is started
    /// and shortly after czar startup: it tells all workers to delete all
    /// query information for queries with czarId `czId` and queryId less than
    /// or equal to `lastQId`.
    void setCzarCancelAfterRestart(CzarIdType czId, QueryId lastQId) {
        _wqsData->setCzarCancelAfterRestart(czId, lastQId);
    }

    protojson::WorkerContactInfo::Ptr getWInfo() const;

    ~ActiveWorker() = default;

    /// Return true if there were differences in  worker id, host, or port values.
    bool compareContactInfo(protojson::WorkerContactInfo const& wcInfo) const;

    void setWorkerContactInfo(protojson::WorkerContactInfo::Ptr const& wcInfo);

    /// Check this workers state (by looking at contact information) and queue
    /// the WorkerQueryStatusData message `_wqsData` to be sent if this worker
    /// isn't DEAD.
    void updateStateAndSendMessages(double timeoutAliveSecs, double timeoutDeadSecs, double maxLifetime);

    /// Add `qId` to list of QueryId's that the worker can discard all tasks and
    /// result files for. This `qId` will be removed from the list once the worker
    /// has responded to the `_wqsData` message with this `qId` in the appropriate
    /// list.
    /// It is expected that all completed or cancelled queries on this worker will
    /// be added to this list.
    void addToDoneDeleteFiles(QueryId qId);

    /// Add `qId` to list of QueryId's that the worker where the worker must hold
    /// onto result files but tasks can be eliminated. This `qId` will be removed
    /// from the list once the worker has responded to the `_wqsData` message with
    /// this `qId` in the appropriate list.
    void addToDoneKeepFiles(QueryId qId);

    /// Add the uberjob to the list of dead uberjobs. This `qId` will be removed
    /// from the list once the worker has responded to the `_wqsData` message with
    /// this `qId` in the appropriate list. Or the `qId` is in a
    /// removeDeadUberJobsFor() call.
    void addDeadUberJob(QueryId qId, UberJobId ujId);

    /// If a query is completed or cancelled, there's no reason to track the
    /// individual UberJobs anymore, so this function will get rid of them.
    void removeDeadUberJobsFor(QueryId qId);

    State getState() const;

    std::string dump() const;

private:
    ActiveWorker(protojson::WorkerContactInfo::Ptr const& wInfo,
                 protojson::CzarContactInfo::Ptr const& czInfo, std::string const& replicationInstanceId,
                 std::string const& replicationAuthKey)
            : _wqsData(protojson::WorkerQueryStatusData::create(wInfo, czInfo, replicationInstanceId,
                                                                replicationAuthKey)) {
        if (_wqsData == nullptr) {
            throw util::Bug(ERR_LOC, "ActiveWorker _wqsData null");
        }
    }

    /// Change the state to `newState` and log if it is different.
    /// _aMtx must be held before calling.
    void _changeStateTo(State newState, double secsSinceUpdate, std::string const& note);

    /// Send the `jsWorkerReqPtr` json message to the worker referenced by `wInf` to
    /// transmit the `_wqsData` state.
    void _sendStatusMsg(protojson::WorkerContactInfo::Ptr const& wInf,
                        std::shared_ptr<nlohmann::json> const& jsWorkerReqPtr);

    /// Dump a log string for this object.
    /// _aMtx must be held before calling.
    std::string _dump() const;

    /// Contains data that needs to be sent to workers about finished/cancelled
    /// user queries and UberJobs. It must not be null.
    protojson::WorkerQueryStatusData::Ptr const _wqsData;

    State _state{QUESTIONABLE};  ///< current state of this worker.

    mutable std::mutex _aMtx;  ///< protects _wInfo, _state, _qIdDoneKeepFiles, _qIdDoneDeleteFiles
};

/// This class maintains a list of all workers, indicating which are considered active.
/// Communication problems with workers could cause interesting race conditions, so
/// workers will remain on the list for a very long time after they have disappeared
/// in case they come back from the dead.
class ActiveWorkerMap {
public:
    using Ptr = std::shared_ptr<ActiveWorkerMap>;
    ActiveWorkerMap() = default;
    ActiveWorkerMap(ActiveWorkerMap const&) = delete;
    ActiveWorkerMap operator=(ActiveWorkerMap const&) = delete;

    ActiveWorkerMap(std::shared_ptr<cconfig::CzarConfig> const& czarConfig);

    ~ActiveWorkerMap() = default;

    std::string cName(const char* fName) { return std::string("ActiveWorkerMap::") + fName + " "; }

    /// Use information gathered from the registry to update the map. The registry
    /// contains last contact time (used for determining aliveness) and worker contact information.
    void updateMap(protojson::WorkerContactInfo::WCMap const& wcMap,
                   protojson::CzarContactInfo::Ptr const& czInfo, std::string const& replicationInstanceId,
                   std::string const& replicationAuthKey);

    /// If this is to be called, it must be called before Czar::_monitor is started:
    /// It tells the workers all queries from `czId` with QueryIds less than `lastQId`
    /// should be cancelled.
    void setCzarCancelAfterRestart(CzarIdType czId, QueryId lastQId);

    /// Return a pointer to the `ActiveWorker` associated with `workerId`.
    ActiveWorker::Ptr getActiveWorker(std::string const& workerId) const;

    /// Call `updateStateAndSendMessages` for all workers in this map.
    void sendActiveWorkersMessages();

    /// Add `qId` to the list of query ids where the worker can throw away all related
    /// Tasks and result files. This is used for all completed user queries and cancelled
    /// user queries.
    void addToDoneDeleteFiles(QueryId qId);

    /// Add `qId` to the list of query ids where the worker must hold onto result
    /// files but all incomplete Tasks can be stopped. This is used for `rowLimitComplete`
    /// where enough rows have been found to complete a user query with a LIMIT
    /// clause. The czar may still need to collect the result files from the worker.
    /// Once the czar has completed the user query, the `qId` will be added to
    /// `addToDoneDeleteFiles` so the workers will delete the files.
    void addToDoneKeepFiles(QueryId qId);

private:
    std::map<std::string, ActiveWorker::Ptr> _awMap;  ///< Key is worker id.
    mutable std::mutex _awMapMtx;                     ///< protects _awMap;

    /// @see CzarConfig::getActiveWorkerTimeoutAliveSecs()
    double _timeoutAliveSecs = 60.0 * 5.0;

    /// @see CzarConfig::getActiveWorkerTimeoutDeadSecs()
    double _timeoutDeadSecs = 60.0 * 10.0;

    /// @see CzarConfig::getActiveWorkerMaxLifetimeSecs()
    double _maxLifetime = 60.0 * 60.0;

    bool _czarCancelAfterRestart = false;
    CzarIdType _czarCancelAfterRestartCzId = 0;
    QueryId _czarCancelAfterRestartQId = 0;
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_ACTIVEWORKER_H
