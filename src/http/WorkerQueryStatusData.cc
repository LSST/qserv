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
#include "http/WorkerQueryStatusData.h"

// System headers
#include <stdexcept>

// Qserv headers
#include "http/Client.h"
#include "http/MetaModule.h"
#include "http/RequestBodyJSON.h"
#include "util/common.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace nlohmann;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.http.WorkerQueryStatusData");
}  // namespace

namespace lsst::qserv::http {

json CzarContactInfo::serializeJson() const {
    json jsCzar;
    jsCzar["name"] = czName;
    jsCzar["id"] = czId;
    jsCzar["management-port"] = czPort;
    jsCzar["management-host-name"] = czHostName;
    return jsCzar;
}

CzarContactInfo::Ptr CzarContactInfo::createJson(nlohmann::json const& czJson) {
    try {
        auto czName_ = RequestBodyJSON::required<string>(czJson, "name");
        auto czId_ = RequestBodyJSON::required<CzarIdType>(czJson, "id");
        auto czPort_ = RequestBodyJSON::required<int>(czJson, "management-port");
        auto czHostName_ = RequestBodyJSON::required<string>(czJson, "management-host-name");
        return create(czName_, czId_, czPort_, czHostName_);
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("CzarContactInfo::createJson invalid ") << exc.what());
    }
    return nullptr;
}

std::string CzarContactInfo::dump() const {
    stringstream os;
    os << "czName=" << czName << " czId=" << czId << " czPort=" << czPort << " czHostName=" << czHostName;
    return os.str();
}

json WorkerContactInfo::serializeJson() const {
    json jsWorker;
    jsWorker["id"] = wId;
    jsWorker["host"] = wHost;
    jsWorker["management-host-name"] = wManagementHost;
    jsWorker["management-port"] = wPort;
    return jsWorker;
}

WorkerContactInfo::Ptr WorkerContactInfo::createJson(nlohmann::json const& wJson, TIMEPOINT updateTime_) {
    LOGS(_log, LOG_LVL_ERROR, "WorkerContactInfo::createJson &&& a");
    try {
        auto wId_ = RequestBodyJSON::required<string>(wJson, "id");
        LOGS(_log, LOG_LVL_ERROR, "WorkerContactInfo::createJson &&& b");
        auto wHost_ = RequestBodyJSON::required<string>(wJson, "host");
        LOGS(_log, LOG_LVL_ERROR, "WorkerContactInfo::createJson &&& c");
        auto wManagementHost_ = RequestBodyJSON::required<string>(wJson, "management-host-name");
        LOGS(_log, LOG_LVL_ERROR, "WorkerContactInfo::createJson &&& d");
        auto wPort_ = RequestBodyJSON::required<int>(wJson, "management-port");
        LOGS(_log, LOG_LVL_ERROR, "WorkerContactInfo::createJson &&& e");
        return create(wId_, wHost_, wManagementHost_, wPort_, updateTime_);
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("CWorkerContactInfo::createJson invalid ") << exc.what());
    }
    return nullptr;
}

string WorkerContactInfo::dump() const {
    stringstream os;
    os << "workerContactInfo{"
       << "id=" << wId << " host=" << wHost << " mgHost=" << wManagementHost << " port=" << wPort << "}";
    return os.str();
}

/*  &&&
string ActiveWorker::getStateStr(State st) {
    switch (st) {
    case ALIVE: return string("ALIVE");
    case QUESTIONABLE: return string("QUESTIONABLE");
    case DEAD: return string("DEAD");
    }
    return string("unknown");
}


bool WorkerQueryStatusData::compareContactInfo(WorkerContactInfo const& wcInfo) const {
    return _wInfo->isSameContactInfo(wcInfo);
}

void WorkerQueryStatusData::setWorkerContactInfo(WorkerContactInfo::Ptr const& wcInfo) {
    LOGS(_log, LOG_LVL_WARN, cName(__func__) << " new info=" << wcInfo->dump());
    _wInfo = wcInfo;
}
*/

shared_ptr<json> WorkerQueryStatusData::serializeJson(double maxLifetime) {
    // Go through the _qIdDoneKeepFiles, _qIdDoneDeleteFiles, and _qIdDeadUberJobs lists to build a
    // message to send to the worker.
    auto now = CLOCK::now();
    shared_ptr<json> jsWorkerReqPtr = make_shared<json>();
    json& jsWorkerR = *jsWorkerReqPtr;
    jsWorkerR["version"] = http::MetaModule::version;
    jsWorkerR["instance_id"] = _replicationInstanceId;
    jsWorkerR["auth_key"] = _replicationAuthKey;
    jsWorkerR["czar"] = _czInfo->serializeJson();
    jsWorkerR["worker"] = _wInfo->serializeJson();

    addListsToJson(jsWorkerR, now, maxLifetime);
    if (_czarCancelAfterRestart) {
        jsWorkerR["czarrestart"] = true;
        lock_guard<mutex> mapLg(_mapMtx);
        jsWorkerR["czarrestartcancelczid"] = _czarCancelAfterRestartCzId;
        jsWorkerR["czarrestartcancelqid"] = _czarCancelAfterRestartQId;
    } else {
        jsWorkerR["czarrestart"] = false;
    }

    return jsWorkerReqPtr;
}

void WorkerQueryStatusData::addListsToJson(json& jsWR, TIMEPOINT tm, double maxLifetime) {
    jsWR["qiddonekeepfiles"] = json::array();
    jsWR["qiddonedeletefiles"] = json::array();
    jsWR["qiddeaduberjobs"] = json::array();
    lock_guard<mutex> mapLg(_mapMtx);
    {
        auto& jsDoneKeep = jsWR["qiddonekeepfiles"];
        auto iterDoneKeep = _qIdDoneKeepFiles.begin();
        while (iterDoneKeep != _qIdDoneKeepFiles.end()) {
            auto qId = iterDoneKeep->first;
            jsDoneKeep.push_back(qId);
            auto tmStamp = iterDoneKeep->second;
            double ageSecs = std::chrono::duration<double>(tm - tmStamp).count();
            if (ageSecs > maxLifetime) {
                iterDoneKeep = _qIdDoneKeepFiles.erase(iterDoneKeep);
            } else {
                ++iterDoneKeep;
            }
        }
    }
    {
        auto& jsDoneDelete = jsWR["qiddonedeletefiles"];
        auto iterDoneDelete = _qIdDoneDeleteFiles.begin();
        while (iterDoneDelete != _qIdDoneDeleteFiles.end()) {
            auto qId = iterDoneDelete->first;
            jsDoneDelete.push_back(qId);
            auto tmStamp = iterDoneDelete->second;
            double ageSecs = std::chrono::duration<double>(tm - tmStamp).count();
            if (ageSecs > maxLifetime) {
                iterDoneDelete = _qIdDoneDeleteFiles.erase(iterDoneDelete);
            } else {
                ++iterDoneDelete;
            }
        }
    }
    {
        auto& jsDeadUj = jsWR["qiddeaduberjobs"];
        auto iterDeadUjQid = _qIdDeadUberJobs.begin();
        while (iterDeadUjQid != _qIdDeadUberJobs.end()) {
            TIMEPOINT oldestTm;  // default is zero
            auto qId = iterDeadUjQid->first;
            auto& ujIdMap = iterDeadUjQid->second;

            json jsQidUj = {{"qid", qId}, {"ujids", json::array()}};
            auto& jsUjIds = jsQidUj["ujids"];

            auto iterUjId = ujIdMap.begin();
            bool addedUjId = false;
            while (iterUjId != ujIdMap.end()) {
                UberJobId ujId = iterUjId->first;
                auto tmStamp = iterUjId->second;
                if (tmStamp > oldestTm) {
                    oldestTm = tmStamp;
                }

                jsUjIds.push_back(ujId);
                addedUjId = true;
                double ageSecs = std::chrono::duration<double>(tm - tmStamp).count();
                if (ageSecs > maxLifetime) {
                    iterUjId = ujIdMap.erase(iterUjId);
                } else {
                    ++iterUjId;
                }
            }

            if (addedUjId) {
                jsDeadUj.push_back(jsQidUj);
            }

            if (ujIdMap.empty() || std::chrono::duration<double>(tm - oldestTm).count() > maxLifetime) {
                iterDeadUjQid = _qIdDeadUberJobs.erase(iterDeadUjQid);
            } else {
                ++iterDeadUjQid;
            }
        }
    }
}

WorkerQueryStatusData::Ptr WorkerQueryStatusData::createJson(nlohmann::json const& jsWorkerReq,
                                                             std::string const& replicationInstanceId,
                                                             std::string const& replicationAuthKey,
                                                             TIMEPOINT updateTm) {
    LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson &&& a");
    try {
        if (jsWorkerReq["version"] != http::MetaModule::version) {
            LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson bad version");
            return nullptr;
        }

        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson &&& b");
        auto czInfo_ = CzarContactInfo::createJson(jsWorkerReq["czar"]);
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson &&& c");
        auto wInfo_ = WorkerContactInfo::createJson(jsWorkerReq["worker"], updateTm);
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson &&& d");
        if (czInfo_ == nullptr || wInfo_ == nullptr) {
            LOGS(_log, LOG_LVL_ERROR,
                 "WorkerQueryStatusData::createJson czar or worker info could not be parsed in "
                         << jsWorkerReq);
        }
        auto wqsData =
                WorkerQueryStatusData::create(wInfo_, czInfo_, replicationInstanceId, replicationAuthKey);
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson &&& e");
        wqsData->parseLists(jsWorkerReq, updateTm);
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::createJson &&& end");
        bool czarRestart = RequestBodyJSON::required<bool>(jsWorkerReq, "czarrestart");
        if (czarRestart) {
            auto restartCzarId = RequestBodyJSON::required<CzarIdType>(jsWorkerReq, "czarrestartcancelczid");
            auto restartQueryId = RequestBodyJSON::required<QueryId>(jsWorkerReq, "czarrestartcancelqid");
            wqsData->setCzarCancelAfterRestart(restartCzarId, restartQueryId);
        }
        return wqsData;
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, string("WorkerQueryStatusData::createJson invalid ") << exc.what());
    }
    return nullptr;
}

void WorkerQueryStatusData::parseLists(nlohmann::json const& jsWR, TIMEPOINT updateTm) {
    lock_guard<mutex> mapLg(_mapMtx);
    parseListsInto(jsWR, updateTm, _qIdDoneKeepFiles, _qIdDoneDeleteFiles, _qIdDeadUberJobs);
}

void WorkerQueryStatusData::parseListsInto(nlohmann::json const& jsWR, TIMEPOINT updateTm,
                                           std::map<QueryId, TIMEPOINT>& doneKeepF,
                                           std::map<QueryId, TIMEPOINT>& doneDeleteF,
                                           std::map<QueryId, std::map<UberJobId, TIMEPOINT>>& deadUberJobs) {
    LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& a");
    auto& jsQIdDoneKeepFiles = jsWR["qiddonekeepfiles"];
    LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& b");
    for (auto const& qidKeep : jsQIdDoneKeepFiles) {
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& b1");
        doneKeepF[qidKeep] = updateTm;
    }

    LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& c");
    auto& jsQIdDoneDeleteFiles = jsWR["qiddonedeletefiles"];
    LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& d");
    for (auto const& qidDelete : jsQIdDoneDeleteFiles) {
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& d1");
        doneDeleteF[qidDelete] = updateTm;
    }

    LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& e");
    auto& jsQIdDeadUberJobs = jsWR["qiddeaduberjobs"];
    LOGS(_log, LOG_LVL_ERROR,
         "WorkerQueryStatusData::parseListsInto &&& f jsQIdDeadUberJobs=" << jsQIdDeadUberJobs);
    // Interestingly, !jsQIdDeadUberJobs.empty() doesn't work, but .size() > 0 does.
    // Not having the size() check causes issues with the for loop trying to read the
    // first element of an empty list, which goes badly.
    if (jsQIdDeadUberJobs.size() > 0) {
        LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& f1");
        for (auto const& qDeadUjs : jsQIdDeadUberJobs) {
            LOGS(_log, LOG_LVL_ERROR, "WorkerQueryStatusData::parseListsInto &&& f1a qDeadUjs=" << qDeadUjs);
            QueryId qId = qDeadUjs["qid"];
            auto const& ujIds = qDeadUjs["ujids"];
            auto& mapOfUj = deadUberJobs[qId];
            for (auto const& ujId : ujIds) {
                LOGS(_log, LOG_LVL_ERROR,
                     "WorkerQueryStatusData::parseListsInto &&& f1d1 qId=" << qId << " ujId=" << ujId);
                mapOfUj[ujId] = updateTm;
            }
        }
    }
}

void WorkerQueryStatusData::addDeadUberJobs(QueryId qId, std::vector<UberJobId> ujIds, TIMEPOINT tm) {
    auto& ujMap = _qIdDeadUberJobs[qId];
    for (auto const ujId : ujIds) {
        ujMap[ujId] = tm;
    }
}

void WorkerQueryStatusData::addToDoneDeleteFiles(QueryId qId) {
    lock_guard<mutex> mapLg(_mapMtx);
    _qIdDoneDeleteFiles[qId] = CLOCK::now();
}

void WorkerQueryStatusData::addToDoneKeepFiles(QueryId qId) {
    lock_guard<mutex> mapLg(_mapMtx);
    _qIdDoneKeepFiles[qId] = CLOCK::now();
}

void WorkerQueryStatusData::removeDeadUberJobsFor(QueryId qId) {
    lock_guard<mutex> mapLg(_mapMtx);
    _qIdDeadUberJobs.erase(qId);
}

json WorkerQueryStatusData::serializeResponseJson() {
    // Go through the _qIdDoneKeepFiles, _qIdDoneDeleteFiles, and _qIdDeadUberJobs lists to build a
    // reponse. Nothing should be deleted and time is irrelevant for this, so maxLifetime is enormous
    // and any time could be used, but now is easy.
    double maxLifetime = std::numeric_limits<double>::max();
    auto now = CLOCK::now();
    json jsResp = {{"success", 1}, {"errortype", "none"}, {"note", ""}};
    addListsToJson(jsResp, now, maxLifetime);
    return jsResp;
}

bool WorkerQueryStatusData::handleResponseJson(nlohmann::json const& jsResp) {
    auto now = CLOCK::now();
    std::map<QueryId, TIMEPOINT> doneKeepF;
    std::map<QueryId, TIMEPOINT> doneDeleteF;
    std::map<QueryId, std::map<UberJobId, TIMEPOINT>> deadUberJobs;
    parseListsInto(jsResp, now, doneKeepF, doneDeleteF, deadUberJobs);

    lock_guard<mutex> mapLg(_mapMtx);
    // Remove entries from _qIdDoneKeepFiles
    for (auto const& [qId, tm] : doneKeepF) {
        _qIdDoneKeepFiles.erase(qId);
    }

    // Remove entries from _qIdDoneDeleteFiles
    for (auto const& [qId, tm] : doneDeleteF) {
        _qIdDoneDeleteFiles.erase(qId);
    }

    // Remove entries from _qIdDeadUberJobs
    for (auto const& [qId, ujMap] : deadUberJobs) {
        auto iter = _qIdDeadUberJobs.find(qId);
        if (iter != _qIdDeadUberJobs.end()) {
            auto& deadMap = iter->second;
            for (auto const& [ujId, tm] : ujMap) {
                deadMap.erase(ujId);
            }
            if (deadMap.empty()) {
                _qIdDeadUberJobs.erase(iter);
            }
        }
    }

    return true;
}

string WorkerQueryStatusData::dump() const {
    stringstream os;
    os << "ActiveWorker " << ((_wInfo == nullptr) ? "?" : _wInfo->dump());
    return os.str();
}

}  // namespace lsst::qserv::http
