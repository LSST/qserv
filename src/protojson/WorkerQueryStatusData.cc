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
#include "protojson/WorkerQueryStatusData.h"

#include <stdexcept>

// Qserv headers
#include "http/Client.h"
#include "http/MetaModule.h"
#include "http/RequestBodyJSON.h"
#include "util/common.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace nlohmann;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.protojson.WorkerQueryStatusData");
}  // namespace

namespace lsst::qserv::protojson {

json CzarContactInfo::serializeJson() const {
    json jsCzar;
    jsCzar["name"] = czName;
    jsCzar["id"] = czId;
    jsCzar["management-port"] = czPort;
    jsCzar["management-host-name"] = czHostName;
    jsCzar["czar-startup-time"] = czStartupTime;
    return jsCzar;
}

CzarContactInfo::Ptr CzarContactInfo::createFromJson(nlohmann::json const& czJson) {
    try {
        auto czName_ = http::RequestBodyJSON::required<string>(czJson, "name");
        auto czId_ = http::RequestBodyJSON::required<CzarIdType>(czJson, "id");
        auto czPort_ = http::RequestBodyJSON::required<int>(czJson, "management-port");
        auto czHostName_ = http::RequestBodyJSON::required<string>(czJson, "management-host-name");
        auto czStartupTime_ = http::RequestBodyJSON::required<uint64_t>(czJson, "czar-startup-time");
        return create(czName_, czId_, czPort_, czHostName_, czStartupTime_);
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("CzarContactInfo::createJson invalid ") << exc.what());
    }
    return nullptr;
}

std::string CzarContactInfo::dump() const {
    stringstream os;
    os << "czName=" << czName << " czId=" << czId << " czPort=" << czPort << " czHostName=" << czHostName
       << " czStartupTime=" << czStartupTime;
    return os.str();
}

json WorkerContactInfo::serializeJson() const {
    lock_guard lg(_rMtx);
    return _serializeJson();
}

json WorkerContactInfo::_serializeJson() const {
    json jsWorker;
    jsWorker["id"] = wId;
    jsWorker["host"] = _wHost;
    jsWorker["management-host-name"] = _wManagementHost;
    jsWorker["management-port"] = _wPort;
    jsWorker["w-startup-time"] = _wStartupTime;
    return jsWorker;
}

WorkerContactInfo::Ptr WorkerContactInfo::createFromJsonRegistry(string const& wId_,
                                                                 nlohmann::json const& regJson) {
    try {
        auto wHost_ = http::RequestBodyJSON::required<string>(regJson, "host-addr");
        auto wManagementHost_ = http::RequestBodyJSON::required<string>(regJson, "management-host-name");
        auto wPort_ = http::RequestBodyJSON::required<int>(regJson, "management-port");
        auto updateTimeInt = http::RequestBodyJSON::required<uint64_t>(regJson, "update-time-ms");
        TIMEPOINT updateTime_ = TIMEPOINT(chrono::milliseconds(updateTimeInt));

        return create(wId_, wHost_, wManagementHost_, wPort_, updateTime_);
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("CWorkerContactInfo::createJson invalid ") << exc.what());
    }
    return nullptr;
}

WorkerContactInfo::Ptr WorkerContactInfo::createFromJsonWorker(nlohmann::json const& wJson,
                                                               TIMEPOINT updateTime_) {
    try {
        auto wId_ = http::RequestBodyJSON::required<string>(wJson, "id");
        auto wHost_ = http::RequestBodyJSON::required<string>(wJson, "host");
        auto wManagementHost_ = http::RequestBodyJSON::required<string>(wJson, "management-host-name");
        auto wPort_ = http::RequestBodyJSON::required<int>(wJson, "management-port");

        return create(wId_, wHost_, wManagementHost_, wPort_, updateTime_);
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("CWorkerContactInfo::createJson invalid ") << exc.what());
    }
    return nullptr;
}

void WorkerContactInfo::setRegUpdateTime(TIMEPOINT updateTime) {
    std::lock_guard lg(_rMtx);
    _regUpdateTime = updateTime;
    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " " << _dump());
}

string WorkerContactInfo::dump() const {
    lock_guard lg(_rMtx);
    return _dump();
}

string WorkerContactInfo::_dump() const {
    stringstream os;
    os << "workerContactInfo{"
       << "id=" << wId << " host=" << _wHost << " mgHost=" << _wManagementHost << " port=" << _wPort
       << " update=" << util::TimeUtils::timePointToDateTimeString(_regUpdateTime) << "}";
    return os.str();
}

shared_ptr<json> WorkerQueryStatusData::serializeJson(double maxLifetime) {
    // Go through the _qIdDoneKeepFiles, _qIdDoneDeleteFiles, and _qIdDeadUberJobs lists to build a
    // message to send to the worker.
    auto now = CLOCK::now();
    shared_ptr<json> jsWorkerReqPtr = make_shared<json>();
    json& jsWorkerR = *jsWorkerReqPtr;
    jsWorkerR["version"] = http::MetaModule::version;
    jsWorkerR["instance_id"] = _replicationInstanceId;
    jsWorkerR["auth_key"] = _replicationAuthKey;
    jsWorkerR["czarinfo"] = _czInfo->serializeJson();
    {
        lock_guard lgI(_infoMtx);
        if (_wInfo != nullptr) {
            jsWorkerR["workerinfo"] = _wInfo->serializeJson();
            jsWorkerR["worker"] = _wInfo->wId;
        } else {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " wInfo is null");
        }
    }

    // Note, old elements in the maps will be deleted after being added to the message
    // to keep the czar from keeping track of these forever.
    addListsToJson(jsWorkerR, now, maxLifetime);
    if (czarCancelAfterRestart) {
        jsWorkerR["czarrestart"] = true;
        lock_guard mapLg(mapMtx);
        jsWorkerR["czarrestartcancelczid"] = czarCancelAfterRestartCzId;
        jsWorkerR["czarrestartcancelqid"] = czarCancelAfterRestartQId;
    } else {
        jsWorkerR["czarrestart"] = false;
    }

    return jsWorkerReqPtr;
}

void WorkerQueryStatusData::addListsToJson(json& jsWR, TIMEPOINT tmMark, double maxLifetime) {
    jsWR["qiddonekeepfiles"] = json::array();
    jsWR["qiddonedeletefiles"] = json::array();
    jsWR["qiddeaduberjobs"] = json::array();
    lock_guard mapLg(mapMtx);
    {
        auto& jsDoneKeep = jsWR["qiddonekeepfiles"];
        auto iterDoneKeep = qIdDoneKeepFiles.begin();
        while (iterDoneKeep != qIdDoneKeepFiles.end()) {
            auto qId = iterDoneKeep->first;
            jsDoneKeep.push_back(qId);
            auto tmTouched = iterDoneKeep->second;
            double ageSecs = std::chrono::duration<double>(tmMark - tmTouched).count();
            if (ageSecs > maxLifetime) {
                iterDoneKeep = qIdDoneKeepFiles.erase(iterDoneKeep);
            } else {
                ++iterDoneKeep;
            }
        }
    }
    {
        auto& jsDoneDelete = jsWR["qiddonedeletefiles"];
        auto iterDoneDelete = qIdDoneDeleteFiles.begin();
        while (iterDoneDelete != qIdDoneDeleteFiles.end()) {
            auto qId = iterDoneDelete->first;
            jsDoneDelete.push_back(qId);
            auto tmStamp = iterDoneDelete->second;
            double ageSecs = std::chrono::duration<double>(tmMark - tmStamp).count();
            if (ageSecs > maxLifetime) {
                iterDoneDelete = qIdDoneDeleteFiles.erase(iterDoneDelete);
            } else {
                ++iterDoneDelete;
            }
        }
    }
    {
        auto& jsDeadUj = jsWR["qiddeaduberjobs"];
        auto iterDeadUjQid = qIdDeadUberJobs.begin();
        while (iterDeadUjQid != qIdDeadUberJobs.end()) {
            TIMEPOINT youngestTm = TIMEPOINT::max();  // need to find the youngest
            auto qId = iterDeadUjQid->first;
            auto& ujIdMap = iterDeadUjQid->second;

            json jsQidUj = {{"qid", qId}, {"ujids", json::array()}};
            auto& jsUjIds = jsQidUj["ujids"];

            auto iterUjId = ujIdMap.begin();
            bool addedUjId = false;

            while (iterUjId != ujIdMap.end()) {
                UberJobId ujId = iterUjId->first;
                auto tmStamp = iterUjId->second;
                if (tmStamp < youngestTm) {
                    youngestTm = tmStamp;
                }

                jsUjIds.push_back(ujId);
                addedUjId = true;
                double ageSecs = std::chrono::duration<double>(tmMark - tmStamp).count();
                if (ageSecs > maxLifetime) {
                    iterUjId = ujIdMap.erase(iterUjId);
                } else {
                    ++iterUjId;
                }
            }

            if (addedUjId) {
                jsDeadUj.push_back(jsQidUj);
            }

            // If the youngest element was too old, delete the map.
            if (ujIdMap.empty() || std::chrono::duration<double>(tmMark - youngestTm).count() > maxLifetime) {
                iterDeadUjQid = qIdDeadUberJobs.erase(iterDeadUjQid);
            } else {
                ++iterDeadUjQid;
            }
        }
    }
}

WorkerQueryStatusData::Ptr WorkerQueryStatusData::createFromJson(nlohmann::json const& jsWorkerReq,
                                                                 std::string const& replicationInstanceId_,
                                                                 std::string const& replicationAuthKey_,
                                                                 TIMEPOINT updateTm) {
    try {
        if (jsWorkerReq["version"] != http::MetaModule::version) {
            LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson bad version");
            return nullptr;
        }

        auto czInfo_ = CzarContactInfo::createFromJson(jsWorkerReq["czarinfo"]);
        auto wInfo_ = WorkerContactInfo::createFromJsonWorker(jsWorkerReq["workerinfo"], updateTm);
        if (czInfo_ == nullptr || wInfo_ == nullptr) {
            LOGS(_log, LOG_LVL_ERROR,
                 "WorkerQueryStatusData::createJson czar or worker info could not be parsed in "
                         << jsWorkerReq);
            return nullptr;
        }
        auto wqsData =
                WorkerQueryStatusData::create(wInfo_, czInfo_, replicationInstanceId_, replicationAuthKey_);
        wqsData->parseLists(jsWorkerReq, updateTm);

        bool czarRestart = http::RequestBodyJSON::required<bool>(jsWorkerReq, "czarrestart");
        if (czarRestart) {
            auto restartCzarId =
                    http::RequestBodyJSON::required<CzarIdType>(jsWorkerReq, "czarrestartcancelczid");
            auto restartQueryId =
                    http::RequestBodyJSON::required<QueryId>(jsWorkerReq, "czarrestartcancelqid");
            wqsData->setCzarCancelAfterRestart(restartCzarId, restartQueryId);
        }
        return wqsData;
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("WorkerQueryStatusData::createJson invalid ") << exc.what());
    }
    return nullptr;
}

void WorkerQueryStatusData::parseLists(nlohmann::json const& jsWR, TIMEPOINT updateTm) {
    lock_guard mapLg(mapMtx);
    parseListsInto(jsWR, updateTm, qIdDoneKeepFiles, qIdDoneDeleteFiles, qIdDeadUberJobs);
}

void WorkerQueryStatusData::parseListsInto(nlohmann::json const& jsWR, TIMEPOINT updateTm,
                                           std::map<QueryId, TIMEPOINT>& doneKeepF,
                                           std::map<QueryId, TIMEPOINT>& doneDeleteF,
                                           std::map<QueryId, std::map<UberJobId, TIMEPOINT>>& deadUberJobs) {
    auto& jsQIdDoneKeepFiles = jsWR["qiddonekeepfiles"];
    for (auto const& qidKeep : jsQIdDoneKeepFiles) {
        doneKeepF[qidKeep] = updateTm;
    }

    auto& jsQIdDoneDeleteFiles = jsWR["qiddonedeletefiles"];
    for (auto const& qidDelete : jsQIdDoneDeleteFiles) {
        doneDeleteF[qidDelete] = updateTm;
    }

    auto& jsQIdDeadUberJobs = jsWR["qiddeaduberjobs"];
    // Interestingly, !jsQIdDeadUberJobs.empty() doesn't work, but .size() > 0 does.
    // Not having the size() check causes issues with the for loop trying to read the
    // first element of an empty list, which goes badly.
    if (jsQIdDeadUberJobs.size() > 0) {
        for (auto const& qDeadUjs : jsQIdDeadUberJobs) {
            QueryId qId = qDeadUjs["qid"];
            auto const& ujIds = qDeadUjs["ujids"];
            auto& mapOfUj = deadUberJobs[qId];
            for (auto const& ujId : ujIds) {
                mapOfUj[ujId] = updateTm;
            }
        }
    }
}

void WorkerQueryStatusData::addDeadUberJobs(QueryId qId, std::vector<UberJobId> ujIds, TIMEPOINT tm) {
    lock_guard mapLg(mapMtx);
    auto& ujMap = qIdDeadUberJobs[qId];
    for (auto const ujId : ujIds) {
        ujMap[ujId] = tm;
    }
}

void WorkerQueryStatusData::setWInfo(WorkerContactInfo::Ptr const& wInfo_) {
    std::lock_guard lgI(_infoMtx);
    if (_wInfo == nullptr) {
        _wInfo = wInfo_;
        return;
    }
    if (wInfo_ != nullptr) {
        // This only changes host and port values of _wInfo.
        _wInfo->changeBaseInfo(*wInfo_);
    }
    LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " " << _wInfo->dump());
}

void WorkerQueryStatusData::addDeadUberJob(QueryId qId, UberJobId ujId, TIMEPOINT tm) {
    lock_guard mapLg(mapMtx);
    auto& ujMap = qIdDeadUberJobs[qId];
    ujMap[ujId] = tm;
}

void WorkerQueryStatusData::addToDoneDeleteFiles(QueryId qId) {
    lock_guard mapLg(mapMtx);
    qIdDoneDeleteFiles[qId] = CLOCK::now();
}

void WorkerQueryStatusData::addToDoneKeepFiles(QueryId qId) {
    lock_guard mapLg(mapMtx);
    qIdDoneKeepFiles[qId] = CLOCK::now();
}

void WorkerQueryStatusData::removeDeadUberJobsFor(QueryId qId) {
    lock_guard mapLg(mapMtx);
    qIdDeadUberJobs.erase(qId);
}

json WorkerQueryStatusData::serializeResponseJson(uint64_t workerStartupTime) {
    // Go through the _qIdDoneKeepFiles, _qIdDoneDeleteFiles, and _qIdDeadUberJobs lists to build a
    // response. Nothing should be deleted and time is irrelevant for this, so maxLifetime is enormous
    // and any time could be used for last contact, but now() is easy.
    // This is only called by the worker. As such nothing should be deleted here as the lifetime of
    // these elements is determined by the lifetime of the owning UserQueryInfo instance.
    double maxLifetime = std::numeric_limits<double>::max();
    auto now = CLOCK::now();
    json jsResp = {{"success", 1}, {"errortype", "none"}, {"note", ""}};
    jsResp["w-startup-time"] = workerStartupTime;
    addListsToJson(jsResp, now, maxLifetime);
    return jsResp;
}

bool WorkerQueryStatusData::handleResponseJson(nlohmann::json const& jsResp) {
    auto now = CLOCK::now();
    std::map<QueryId, TIMEPOINT> doneKeepF;
    std::map<QueryId, TIMEPOINT> doneDeleteF;
    std::map<QueryId, std::map<UberJobId, TIMEPOINT>> deadUberJobs;
    parseListsInto(jsResp, now, doneKeepF, doneDeleteF, deadUberJobs);

    lock_guard mapLg(mapMtx);
    // Remove entries from _qIdDoneKeepFiles
    for (auto const& [qId, tm] : doneKeepF) {
        qIdDoneKeepFiles.erase(qId);
    }

    // Remove entries from _qIdDoneDeleteFiles
    for (auto const& [qId, tm] : doneDeleteF) {
        qIdDoneDeleteFiles.erase(qId);
    }

    // Remove entries from _qIdDeadUberJobs
    for (auto const& [qId, ujMap] : deadUberJobs) {
        auto iter = qIdDeadUberJobs.find(qId);
        if (iter != qIdDeadUberJobs.end()) {
            auto& deadMap = iter->second;
            for (auto const& [ujId, tm] : ujMap) {
                deadMap.erase(ujId);
            }
            if (deadMap.empty()) {
                qIdDeadUberJobs.erase(iter);
            }
        }
    }

    bool workerRestarted = false;
    auto workerStartupTime = http::RequestBodyJSON::required<uint64_t>(jsResp, "w-startup-time");
    LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " workerStartupTime=" << workerStartupTime);
    if (!_wInfo->checkWStartupTime(workerStartupTime)) {
        LOGS(_log, LOG_LVL_ERROR,
             cName(__func__) << " startup time for worker=" << _wInfo->dump()
                             << " changed to=" << workerStartupTime << " Assuming worker restarted");
        workerRestarted = true;
    }
    return workerRestarted;
}

string WorkerQueryStatusData::dump() const {
    lock_guard lgI(_infoMtx);
    return _dump();
}

string WorkerQueryStatusData::_dump() const {
    VMUTEX_HELD(_infoMtx);
    stringstream os;
    os << "ActiveWorker " << ((_wInfo == nullptr) ? "?" : _wInfo->dump());
    return os.str();
}

shared_ptr<json> WorkerCzarComIssue::serializeJson() {
    shared_ptr<json> jsCzarReqPtr = make_shared<json>();
    json& jsCzarR = *jsCzarReqPtr;
    lock_guard _lgWciMtx(_wciMtx);
    if (_wInfo == nullptr || _czInfo == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " _wInfo or _czInfo was null");
        return jsCzarReqPtr;
    }

    jsCzarR["version"] = http::MetaModule::version;
    jsCzarR["instance_id"] = _replicationInstanceId;
    jsCzarR["auth_key"] = _replicationAuthKey;
    jsCzarR["czarinfo"] = _czInfo->serializeJson();
    jsCzarR["czar"] = _czInfo->czName;
    jsCzarR["workerinfo"] = _wInfo->serializeJson();

    jsCzarR["thoughtczarwasdead"] = _thoughtCzarWasDead;

    // TODO:UJ add list of failed transmits

    return jsCzarReqPtr;
}

WorkerCzarComIssue::Ptr WorkerCzarComIssue::createFromJson(nlohmann::json const& jsCzarReq,
                                                           std::string const& replicationInstanceId_,
                                                           std::string const& replicationAuthKey_) {
    string const fName("WorkerCzarComIssue::createFromJson");
    LOGS(_log, LOG_LVL_DEBUG, fName);
    try {
        if (jsCzarReq["version"] != http::MetaModule::version) {
            LOGS(_log, LOG_LVL_ERROR, fName << " bad version");
            return nullptr;
        }

        auto czInfo_ = CzarContactInfo::createFromJson(jsCzarReq["czarinfo"]);
        auto now = CLOCK::now();
        auto wInfo_ = WorkerContactInfo::createFromJsonWorker(jsCzarReq["workerinfo"], now);
        if (czInfo_ == nullptr || wInfo_ == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, fName << " or worker info could not be parsed in " << jsCzarReq);
        }
        auto wccIssue = create(replicationInstanceId_, replicationAuthKey_);
        wccIssue->setContactInfo(wInfo_, czInfo_);
        wccIssue->_thoughtCzarWasDead =
                http::RequestBodyJSON::required<bool>(jsCzarReq, "thoughtczarwasdead");
        return wccIssue;
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("WorkerQueryStatusData::createJson invalid ") << exc.what());
    }
    return nullptr;
}

json WorkerCzarComIssue::serializeResponseJson() {
    json jsResp = {{"success", 1}, {"errortype", "none"}, {"note", ""}};

    // TODO:UJ add lists of uberjobs that are scheduled to have files collected because of this message.
    return jsResp;
}

string WorkerCzarComIssue::dump() const {
    lock_guard _lgWciMtx(_wciMtx);
    return _dump();
}

string WorkerCzarComIssue::_dump() const {
    stringstream os;
    os << "WorkerCzarComIssue wInfo=" << ((_wInfo == nullptr) ? "?" : _wInfo->dump());
    os << " czInfo=" << _czInfo->dump();
    os << " thoughtCzarWasDead=" << _thoughtCzarWasDead;
    return os.str();
}

}  // namespace lsst::qserv::protojson
