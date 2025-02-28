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
#ifndef LSST_QSERV_PROTOJSON_WORKERQUERYSTATUSDATA_H
#define LSST_QSERV_PROTOJSON_WORKERQUERYSTATUSDATA_H

// System headers
#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// qserv headers
#include "global/clock_defs.h"
#include "global/intTypes.h"
#include "util/Mutex.h"

// This header declarations
namespace lsst::qserv::protojson {

/// This class just contains the czar id and network contact information.
class CzarContactInfo : public std::enable_shared_from_this<CzarContactInfo> {
public:
    using Ptr = std::shared_ptr<CzarContactInfo>;
    std::string cName(const char* fnc) const { return std::string("CzarContactInfo") + fnc; }

    CzarContactInfo() = delete;
    CzarContactInfo(CzarContactInfo const&) = default;
    CzarContactInfo& operator=(CzarContactInfo const&) = default;

    /// Return true is elements, other than czStartupTime, are the same.
    bool compare(CzarContactInfo const& other) {
        return (czName == other.czName && czId == other.czId && czPort == other.czPort &&
                czHostName == other.czHostName);
    }

    static Ptr create(std::string const& czName_, CzarIdType czId_, int czPort_,
                      std::string const& czHostName_, uint64_t czStartupTime_) {
        return Ptr(new CzarContactInfo(czName_, czId_, czPort_, czHostName_, czStartupTime_));
    }

    static Ptr createFromJson(nlohmann::json const& czarJson);

    std::string const czName;      ///< czar "name"
    CzarIdType const czId;         ///< czar "id"
    int const czPort;              ///< czar "management-port"
    std::string const czHostName;  ///< czar "management-host-name"
    uint64_t const czStartupTime;  ///< czar startup time

    /// Return a json version of the contents of this class.
    nlohmann::json serializeJson() const;

    std::string dump() const;

private:
    CzarContactInfo(std::string const& czName_, CzarIdType czId_, int czPort_, std::string const& czHostName_,
                    uint64_t czStartupTime_)
            : czName(czName_),
              czId(czId_),
              czPort(czPort_),
              czHostName(czHostName_),
              czStartupTime(czStartupTime_) {}
};

/// This class just contains the worker id and network communication information.
class WorkerContactInfo {
public:
    using Ptr = std::shared_ptr<WorkerContactInfo>;

    using WCMap = std::unordered_map<std::string, Ptr>;
    using WCMapPtr = std::shared_ptr<WCMap>;

    static Ptr create(std::string const& wId_, std::string const& wHost_, std::string const& wManagementHost_,
                      int wPort_, TIMEPOINT updateTime_) {
        return Ptr(new WorkerContactInfo(wId_, wHost_, wManagementHost_, wPort_, updateTime_));
    }

    /// This function creates a WorkerQueryStatusData object from a registry json message,
    /// which is provided by the system registry.
    static Ptr createFromJsonRegistry(std::string const& wId_, nlohmann::json const& regJson);

    /// This function creates a WorkerQueryStatusData object from a worker json message.
    static Ptr createFromJsonWorker(nlohmann::json const& workerJson, TIMEPOINT updateTime);

    /// Return a json version of the contents of this object.
    nlohmann::json serializeJson() const;

    std::string cName(const char* fn) { return std::string("WorkerContactInfo::") + fn; }

    std::string const wId;  ///< key, this is the one thing that cannot change.

    std::string getWHost() const {
        std::lock_guard lg(_rMtx);
        return _wHost;
    }

    std::string getWManagementHost() const {
        std::lock_guard lg(_rMtx);
        return _wManagementHost;
    }

    int getWPort() const {
        std::lock_guard lg(_rMtx);
        return _wPort;
    }

    /// Change host and port info to those provided in `other`.
    void changeBaseInfo(WorkerContactInfo const& other) {
        auto [oWId, oWHost, oWManagementHost, oWPort] = other.getAll();
        std::lock_guard lg(_rMtx);
        _wHost = oWHost;
        _wManagementHost = oWManagementHost;
        _wPort = oWPort;
    }

    /// @return wId - workerId
    /// @return _wHost - worker host
    /// @return _wManagementHost - management host
    /// @return _wPort - worker port
    std::tuple<std::string, std::string, std::string, int> getAll() const {
        std::lock_guard lg(_rMtx);
        return {wId, _wHost, _wManagementHost, _wPort};
    }

    /// Return true if communication related items are the same.
    bool isSameContactInfo(WorkerContactInfo const& other) const {
        auto [oWId, oWHost, oWManagementHost, oWPort] = other.getAll();
        std::lock_guard lg(_rMtx);
        return (wId == oWId && _wHost == oWHost && _wManagementHost == oWManagementHost && _wPort == oWPort);
    }

    void setRegUpdateTime(TIMEPOINT updateTime);

    TIMEPOINT getRegUpdateTime(TIMEPOINT updateTime) {
        std::lock_guard lg(_rMtx);
        return _regUpdateTime;
    }

    double timeSinceRegUpdateSeconds() const {
        std::lock_guard lg(_rMtx);
        double secs = std::chrono::duration<double>(CLOCK::now() - _regUpdateTime).count();
        return secs;
    }

    TIMEPOINT getRegUpdateTime() const {
        std::lock_guard lg(_rMtx);
        return _regUpdateTime;
    }

    /// @return true if startupTime equals _wStartupTime or _wStartupTime was never set,
    ///   if _wStartupTime was never set, it is set to startupTime.
    /// @return false indicates the worker was restarted and all associated jobs need
    ///   re-assignment.
    bool checkWStartupTime(uint64_t startupTime) {
        std::lock_guard lg(_rMtx);
        if (_wStartupTime == startupTime) {
            return true;
        }
        if (_wStartupTime == 0) {
            _wStartupTime = startupTime;
            return true;
        }
        _wStartupTime = startupTime;
        return false;
    }

    uint64_t getWStartupTime() const {
        std::lock_guard lg(_rMtx);
        return _wStartupTime;
    }

    std::string dump() const;

private:
    WorkerContactInfo(std::string const& wId_, std::string const& wHost_, std::string const& wManagementHost_,
                      int wPort_, TIMEPOINT updateTime_)
            : wId(wId_), _wHost(wHost_), _wManagementHost(wManagementHost_), _wPort(wPort_) {
        setRegUpdateTime(updateTime_);
    }

    // _rMtx must be locked before calling
    std::string _dump() const;

    // _rMtx must be locked before calling
    nlohmann::json _serializeJson() const;

    std::string _wHost;            ///< "host-addr" entry.
    std::string _wManagementHost;  ///< "management-host-name" entry.
    int _wPort;                    ///< "management-port" entry.

    /// Last time the registry heard from this worker. The ActiveWorker class
    /// will use this to determine the worker's state (alive/dead).
    TIMEPOINT _regUpdateTime;

    /// "w-startup-time", it's value is set to zero until the real value is
    /// received from the worker. Once it is non-zero, any change indicates
    /// the worker was restarted and all UberJobs that were assigned there
    /// need to be unassigned. On the worker, this should always be set from
    /// foreman()->getStartupTime();
    uint64_t _wStartupTime = 0;

    mutable MUTEX _rMtx;  ///< protects _regUpdate
};

/// This classes purpose is to be a structure to store and transfer information
/// about which queries have been completed or cancelled on the worker. This
/// class contains the functions that encode and decode the data they contain
/// to and from a json format.
class WorkerQueryStatusData {
public:
    using Ptr = std::shared_ptr<WorkerQueryStatusData>;

    WorkerQueryStatusData() = delete;
    WorkerQueryStatusData(WorkerQueryStatusData const&) = delete;
    WorkerQueryStatusData& operator=(WorkerQueryStatusData const&) = delete;

    std::string cName(const char* fName) { return std::string("WorkerQueryStatusData::") + fName; }

    static Ptr create(WorkerContactInfo::Ptr const& wInfo_, CzarContactInfo::Ptr const& czInfo_,
                      std::string const& replicationInstanceId_, std::string const& replicationAuthKey_) {
        return Ptr(new WorkerQueryStatusData(wInfo_, czInfo_, replicationInstanceId_, replicationAuthKey_));
    }

    /// This function creates a WorkerQueryStatusData object from the worker json `czarJson`, the
    /// other parameters are used to verify the json message.
    static Ptr createFromJson(nlohmann::json const& czarJson, std::string const& replicationInstanceId_,
                              std::string const& replicationAuthKey_, TIMEPOINT updateTm);

    ~WorkerQueryStatusData() = default;

    void setWInfo(WorkerContactInfo::Ptr const& wInfo_);

    WorkerContactInfo::Ptr getWInfo() const {
        std::lock_guard lgI(_infoMtx);
        return _wInfo;
    }
    CzarContactInfo::Ptr getCzInfo() const { return _czInfo; }

    /// `qId` and `ujId` identify a dead UberJob which is added to the list
    /// of dead UberJobs for this worker.
    void addDeadUberJob(QueryId qId, UberJobId ujId, TIMEPOINT tm);

    /// Add multiple UberJobIds for `qId` to the list of dead UberJobs for
    /// this worker.
    void addDeadUberJobs(QueryId qId, std::vector<UberJobId> ujIds, TIMEPOINT tm);

    /// Add `qId` to the list of user queries where all Tasks can be stopped
    /// and result files can be deleted.
    void addToDoneDeleteFiles(QueryId qId);

    /// Add `qId` to the list of user queries where all Tasks can be stopped
    /// but result files should be kept.
    void addToDoneKeepFiles(QueryId qId);

    /// Remove all UberJobs from the list of dead UberJobs with QueryId `qId`.
    /// There's no point in tracking individual UberJobs once the entire
    /// user query is finished or cancelled as they will all be deleted by
    /// `addToDoneDeleteFiles`
    void removeDeadUberJobsFor(QueryId qId);

    void setCzarCancelAfterRestart(CzarIdType czId, QueryId lastQId) {
        std::lock_guard mapLg(mapMtx);
        czarCancelAfterRestart = true;
        czarCancelAfterRestartCzId = czId;
        czarCancelAfterRestartQId = lastQId;
    }

    bool isCzarRestart() const { return czarCancelAfterRestart; }
    CzarIdType getCzarRestartCzarId() const { return czarCancelAfterRestartCzId; }
    QueryId getCzarRestartQueryId() const { return czarCancelAfterRestartQId; }

    /// Create a json object held by a shared pointer to use as a message.
    /// Old objects in this instance will be removed after being added to the
    /// json message.
    std::shared_ptr<nlohmann::json> serializeJson(double maxLifetime);

    /// Add contents of qIdDoneKeepFiles, _qIdDoneDeleteFiles, and _qIdDeadUberJobs to `jsWR`,
    /// and remove map elements that have an age (tmMark - element.touchTime) greater
    /// than maxLifetime.
    void addListsToJson(nlohmann::json& jsWR, TIMEPOINT tmMark, double maxLifetime);

    /// Parse the lists in `jsWR` to populate the lists for qIdDoneKeepFiles,
    /// qIdDoneDeleteFiles, and qIdDeadUberJobs.
    /// @throws std::invalid_argument
    void parseLists(nlohmann::json const& jsWR, TIMEPOINT updateTm);

    /// Return a json object indicating the status of the message for the
    /// original requester.
    nlohmann::json serializeResponseJson(uint64_t workerStartupTime);

    /// Use the worker's response, `jsResp`, to update the status of this object.
    /// The worker's response contains lists indicating what the worker
    /// received from the czar's json message created with `serializeResponseJson`.
    /// The czar can remove the ids from the lists as once the worker has
    /// verified them.
    /// @return transmitSuccess - true if the message was parsed successfully.
    /// @return workerRestarted - true if `workerStartupTime` doesn't match,
    ///        indicating the worker has been restarted and the czar should
    ///        invalidate and re-assign all UberJobs associated with this
    ///        worker.
    /// @throw invalid_argument if there are problems with json parsing.
    bool handleResponseJson(nlohmann::json const& jsResp);

    /// Parse the contents of `jsWR` to fill the maps `doneKeepF`, `doneDeleteF`,
    /// and `deadUberJobs`.
    static void parseListsInto(nlohmann::json const& jsWR, TIMEPOINT updateTm,
                               std::map<QueryId, TIMEPOINT>& doneKeepF,
                               std::map<QueryId, TIMEPOINT>& doneDeleteF,
                               std::map<QueryId, std::map<UberJobId, TIMEPOINT>>& deadUberJobs);

    std::string dump() const;

    // Making these private requires member functions to be written
    // that cause issues with linking. All of the workarounds are ugly.
    /// Map of QueryIds where the LIMIT clause has been satisfied so
    /// that Tasks can be stopped but result files need to be kept.
    std::map<QueryId, TIMEPOINT> qIdDoneKeepFiles;

    /// Map fo QueryIds where Tasks can be stopped and files deleted, which is
    /// used when user queries are cancelled or finished.
    std::map<QueryId, TIMEPOINT> qIdDoneDeleteFiles;

    /// Map used to indicated a specific UberJobs need to be killed.
    std::map<QueryId, std::map<UberJobId, TIMEPOINT>> qIdDeadUberJobs;

    /// If true, this indicates that this is a newly started czar and
    /// the worker should stop all previous work associated with this
    /// CzarId.
    std::atomic<bool> czarCancelAfterRestart = false;
    CzarIdType czarCancelAfterRestartCzId = 0;
    QueryId czarCancelAfterRestartQId = 0;

    /// Protects _qIdDoneKeepFiles, _qIdDoneDeleteFiles, _qIdDeadUberJobs,
    /// and czarCancelAfter variables.
    mutable MUTEX mapMtx;

private:
    WorkerQueryStatusData(WorkerContactInfo::Ptr const& wInfo_, CzarContactInfo::Ptr const& czInfo_,
                          std::string const& replicationInstanceId_, std::string const& replicationAuthKey_)
            : _wInfo(wInfo_),
              _czInfo(czInfo_),
              _replicationInstanceId(replicationInstanceId_),
              _replicationAuthKey(replicationAuthKey_) {}

    WorkerContactInfo::Ptr _wInfo;       ///< Information needed to contact the worker.
    CzarContactInfo::Ptr const _czInfo;  ///< Information needed to contact the czar.
    mutable MUTEX _infoMtx;              ///< protects _wInfo

    std::string const _replicationInstanceId;  ///< Used for message verification.
    std::string const _replicationAuthKey;     ///< Used for message verification.

    /// _infoMtx must be locked before calling.
    std::string _dump() const;
};

/// This class is used to send/receive a message from the worker to a specific
/// czar when there has been a communication issue with the worker sending UberJob
/// file ready messages. If there have been timeouts, the worker will send this
/// message to the czar immediately after the worker receives a
/// WorkerQueryStatusData message from the czar (indicating that communication
/// is now possible).
/// If communication with the czar has failed for a long time, the worker
/// will set "_thoughtCzarWasDead" and delete all incomplete work associated
/// with that czar. Result files will remain until garbage cleanup or the czar
/// calls for their removal.
/// TODO:UJ &&& UberJob complete messages that failed to be sent to the czar
/// TODO:UJ &&& will be added to this message. (uberjob file response)
/// Upon successful completion, the worker will clear all values set by the
/// the czar.
/// Currently, this message is expected to only be needed rarely.
class WorkerCzarComIssue {
public:
    using Ptr = std::shared_ptr<WorkerCzarComIssue>;

    WorkerCzarComIssue() = delete;
    ~WorkerCzarComIssue() = default;

    std::string cName(const char* funcN) { return std::string("WorkerCzarComIssue") + funcN; }

    static Ptr create(std::string const& replicationInstanceId_, std::string const& replicationAuthKey_) {
        return Ptr(new WorkerCzarComIssue(replicationInstanceId_, replicationAuthKey_));
    }

    static Ptr createFromJson(nlohmann::json const& workerJson, std::string const& replicationInstanceId_,
                              std::string const& replicationAuthKey_);

    void setThoughtCzarWasDead(bool wasDead) {
        std::lock_guard lg(_wciMtx);
        _thoughtCzarWasDead = wasDead;
    }

    bool getThoughtCzarWasDead() const { return _thoughtCzarWasDead; }

    /// Return true if there is a reason this WorkerCzarComIssue should be sent to this czar.
    bool needToSend() const {
        std::lock_guard lg(_wciMtx);
        // TODO:UJ &&& or list of failed transmits not empty.
        return _thoughtCzarWasDead;
    }

    /// Set the contact information for the appropriate czar and worker.
    void setContactInfo(WorkerContactInfo::Ptr const& wInfo_, CzarContactInfo::Ptr const& czInfo_) {
        std::lock_guard lgWci(_wciMtx);
        if (_wInfo == nullptr && wInfo_ != nullptr) _wInfo = wInfo_;
        if (_czInfo == nullptr && czInfo_ != nullptr) _czInfo = czInfo_;
    }

    CzarContactInfo::Ptr getCzarInfo() const {
        std::lock_guard lgWci(_wciMtx);
        return _czInfo;
    }

    WorkerContactInfo::Ptr getWorkerInfo() const {
        std::lock_guard lgWci(_wciMtx);
        return _wInfo;
    }

    /// Return a json version of the contents of this class.
    std::shared_ptr<nlohmann::json> serializeJson();

    /// Return a json object indicating the status of the message for the
    /// original requester.
    nlohmann::json serializeResponseJson();

    std::string dump() const;

private:
    WorkerCzarComIssue(std::string const& replicationInstanceId_, std::string const& replicationAuthKey_)
            : _replicationInstanceId(replicationInstanceId_), _replicationAuthKey(replicationAuthKey_) {}

    std::string _dump() const;

    WorkerContactInfo::Ptr _wInfo;
    CzarContactInfo::Ptr _czInfo;
    std::string const _replicationInstanceId;  ///< Used for message verification.
    std::string const _replicationAuthKey;     ///< Used for message verification.

    /// Set to by the worker true if the czar was considered dead, and reset to false
    /// after the czar has acknowledged successful reception of this message.
    bool _thoughtCzarWasDead = false;

    mutable MUTEX _wciMtx;  ///< protects all members.
};

class WorkerUberJobMsg {
public:
    using Ptr = std::shared_ptr<WorkerUberJobMsg>;

    static Ptr create();

private:
    WorkerUberJobMsg();
};

}  // namespace lsst::qserv::protojson

#endif  // LSST_QSERV_PROTOJSON_WORKERQUERYSTATUSDATA_H
