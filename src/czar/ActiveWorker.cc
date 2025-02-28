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
#include "czar/ActiveWorker.h"

// System headers
#include <stdexcept>

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "czar/Czar.h"
#include "http/Client.h"
#include "http/MetaModule.h"
#include "util/common.h"
#include "util/QdispPool.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace nlohmann;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.ActiveWorker");
}  // namespace

namespace lsst::qserv::czar {

string ActiveWorker::getStateStr(State st) {
    switch (st) {
        case ALIVE:
            return string("ALIVE");
        case QUESTIONABLE:
            return string("QUESTIONABLE");
        case DEAD:
            return string("DEAD");
    }
    return string("unknown");
}

bool ActiveWorker::compareContactInfo(protojson::WorkerContactInfo const& wcInfo) const {
    lock_guard<mutex> lg(_aMtx);
    auto wInfo_ = _wqsData->getWInfo();
    if (wInfo_ == nullptr) return false;
    return wInfo_->isSameContactInfo(wcInfo);
}

void ActiveWorker::setWorkerContactInfo(protojson::WorkerContactInfo::Ptr const& wcInfo) {
    LOGS(_log, LOG_LVL_INFO, cName(__func__) << " new info=" << wcInfo->dump());
    lock_guard<mutex> lg(_aMtx);
    _wqsData->setWInfo(wcInfo);
}

void ActiveWorker::_changeStateTo(State newState, double secsSinceUpdate, string const& note) {
    auto lLvl = (newState == DEAD) ? LOG_LVL_ERROR : LOG_LVL_INFO;
    LOGS(_log, lLvl,
         note << " oldState=" << getStateStr(_state) << " newState=" << getStateStr(newState)
              << " secsSince=" << secsSinceUpdate);
    _state = newState;
}

void ActiveWorker::updateStateAndSendMessages(double timeoutAliveSecs, double timeoutDeadSecs,
                                              double maxLifetime) {
    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " start");
    bool newlyDeadWorker = false;
    protojson::WorkerContactInfo::Ptr wInfo_;
    {
        lock_guard<mutex> lg(_aMtx);
        wInfo_ = _wqsData->getWInfo();
        if (wInfo_ == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " no WorkerContactInfo");
            return;
        }
        double secsSinceUpdate = wInfo_->timeSinceRegUpdateSeconds();
        LOGS(_log, LOG_LVL_TRACE,
             cName(__func__) << " wInfo=" << wInfo_->dump()
                             << " secsSince=" << wInfo_->timeSinceRegUpdateSeconds()
                             << " secsSinceUpdate=" << secsSinceUpdate);

        // Update the last time the registry contacted this worker.
        // TODO:UJ - This needs to be added to the dashboard.
        switch (_state) {
            case ALIVE: {
                if (secsSinceUpdate >= timeoutAliveSecs) {
                    _changeStateTo(QUESTIONABLE, secsSinceUpdate, cName(__func__));
                }
                break;
            }
            case QUESTIONABLE: {
                if (secsSinceUpdate < timeoutAliveSecs) {
                    _changeStateTo(ALIVE, secsSinceUpdate, cName(__func__));
                }
                if (secsSinceUpdate >= timeoutDeadSecs) {
                    _changeStateTo(DEAD, secsSinceUpdate, cName(__func__));
                    // All uberjobs for this worker need to die.
                    newlyDeadWorker = true;
                }
                break;
            }
            case DEAD: {
                if (secsSinceUpdate < timeoutAliveSecs) {
                    _changeStateTo(ALIVE, secsSinceUpdate, cName(__func__));
                } else {
                    // Don't waste time on this worker until the registry has heard from it.
                    return;
                }
                break;
            }
        }
    }

    // _aMtx must not be held when calling this.
    if (newlyDeadWorker) {
        LOGS(_log, LOG_LVL_WARN,
             cName(__func__) << " worker " << wInfo_->wId << " appears to have died, reassigning its jobs.");
        czar::Czar::getCzar()->killIncompleteUbjerJobsOn(wInfo_->wId);
    }

    shared_ptr<json> jsWorkerReqPtr;
    {
        // Go through the _qIdDoneKeepFiles, _qIdDoneDeleteFiles, and _qIdDeadUberJobs lists to build a
        // message to send to the worker.
        jsWorkerReqPtr = _wqsData->serializeJson(maxLifetime);
    }

    // Always send the message as it's a way to inform the worker that this
    // czar is functioning and capable of receiving requests.
    Ptr thisPtr = shared_from_this();
    auto sendStatusMsgFunc = [thisPtr, wInfo_, jsWorkerReqPtr](util::CmdData*) {
        thisPtr->_sendStatusMsg(wInfo_, jsWorkerReqPtr);
    };

    auto cmd = util::PriorityCommand::Ptr(new util::PriorityCommand(sendStatusMsgFunc));
    auto qdisppool = czar::Czar::getCzar()->getQdispPool();
    LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " queuing message");
    qdisppool->queCmd(cmd, 1);
}

void ActiveWorker::_sendStatusMsg(protojson::WorkerContactInfo::Ptr const& wInf,
                                  std::shared_ptr<nlohmann::json> const& jsWorkerReqPtr) {
    auto& jsWorkerReq = *jsWorkerReqPtr;
    auto const method = http::Method::POST;
    if (wInf == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " wInfo was null.");
        return;
    }
    auto [ciwId, ciwHost, ciwManag, ciwPort] = wInf->getAll();
    string const url = "http://" + ciwHost + ":" + to_string(ciwPort) + "/querystatus";
    vector<string> const headers = {"Content-Type: application/json"};
    auto const& czarConfig = cconfig::CzarConfig::instance();

    LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " REQ " << jsWorkerReq);
    string const requestContext = "Czar: '" + http::method2string(method) + "' stat request to '" + url + "'";
    LOGS(_log, LOG_LVL_TRACE,
         cName(__func__) << " czarPost url=" << url << " request=" << jsWorkerReq.dump()
                         << " headers=" << headers[0]);
    http::Client client(method, url, jsWorkerReq.dump(), headers);
    bool transmitSuccess = false;
    string exceptionWhat;
    json response;
    try {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " read start");
        response = client.readAsJson();
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " read end");
        if (0 != response.at("success").get<int>()) {
            bool startupTimeChanged = false;
            startupTimeChanged = _wqsData->handleResponseJson(response);
            transmitSuccess = true;
            if (startupTimeChanged) {
                LOGS(_log, LOG_LVL_WARN, cName(__func__) << " worker startupTime changed, likely rebooted.");
                // kill all incomplete UberJobs on this worker.
                czar::Czar::getCzar()->killIncompleteUbjerJobsOn(wInf->wId);
            }
        } else {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " transmit failure response success=0 " << response);
        }
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR, requestContext + " transmit failure, ex: " + ex.what());
        exceptionWhat = ex.what();
    }
    if (!transmitSuccess) {
        LOGS(_log, LOG_LVL_ERROR,
             cName(__func__) << " transmit failure " << jsWorkerReq.dump() << " resp=" << response);
    }
}

void ActiveWorker::addToDoneDeleteFiles(QueryId qId) { _wqsData->addToDoneDeleteFiles(qId); }

void ActiveWorker::addToDoneKeepFiles(QueryId qId) { _wqsData->addToDoneKeepFiles(qId); }

void ActiveWorker::removeDeadUberJobsFor(QueryId qId) { _wqsData->removeDeadUberJobsFor(qId); }

void ActiveWorker::addDeadUberJob(QueryId qId, UberJobId ujId) {
    auto now = CLOCK::now();
    _wqsData->addDeadUberJob(qId, ujId, now);
}

protojson::WorkerContactInfo::Ptr ActiveWorker::getWInfo() const {
    std::lock_guard lg(_aMtx);
    if (_wqsData == nullptr) return nullptr;
    return _wqsData->getWInfo();
}

ActiveWorker::State ActiveWorker::getState() const {
    std::lock_guard lg(_aMtx);
    return _state;
}

string ActiveWorker::dump() const {
    lock_guard<mutex> lg(_aMtx);
    return _dump();
}

string ActiveWorker::_dump() const {
    stringstream os;
    os << "ActiveWorker " << (_wqsData->dump());
    return os.str();
}

ActiveWorkerMap::ActiveWorkerMap(std::shared_ptr<cconfig::CzarConfig> const& czarConfig)
        : _timeoutAliveSecs(czarConfig->getActiveWorkerTimeoutAliveSecs()),
          _timeoutDeadSecs(czarConfig->getActiveWorkerTimeoutDeadSecs()),
          _maxLifetime(czarConfig->getActiveWorkerMaxLifetimeSecs()) {}

void ActiveWorkerMap::updateMap(protojson::WorkerContactInfo::WCMap const& wcMap,
                                protojson::CzarContactInfo::Ptr const& czInfo,
                                std::string const& replicationInstanceId,
                                std::string const& replicationAuthKey) {
    // Go through wcMap, update existing entries in _awMap, create new entries for those that don't exist,
    lock_guard<mutex> awLg(_awMapMtx);
    for (auto const& [wcKey, wcVal] : wcMap) {
        auto iter = _awMap.find(wcKey);
        if (iter == _awMap.end()) {
            auto newAW = ActiveWorker::create(wcVal, czInfo, replicationInstanceId, replicationAuthKey);
            LOGS(_log, LOG_LVL_INFO, cName(__func__) << " ActiveWorker created for " << wcKey);
            _awMap[wcKey] = newAW;
            if (_czarCancelAfterRestart) {
                newAW->setCzarCancelAfterRestart(_czarCancelAfterRestartCzId, _czarCancelAfterRestartQId);
            }
        } else {
            auto aWorker = iter->second;
            if (!aWorker->compareContactInfo(*wcVal)) {
                // This should not happen, but try to handle it gracefully if it does.
                LOGS(_log, LOG_LVL_WARN,
                     cName(__func__) << " worker contact info changed for " << wcKey
                                     << " new=" << wcVal->dump() << " old=" << aWorker->dump());
                // If there is existing information, only host and port values will change.
                aWorker->setWorkerContactInfo(wcVal);
            }
            aWorker->getWInfo()->setRegUpdateTime(wcVal->getRegUpdateTime());
        }
    }
}

void ActiveWorkerMap::setCzarCancelAfterRestart(CzarIdType czId, QueryId lastQId) {
    _czarCancelAfterRestart = true;
    _czarCancelAfterRestartCzId = czId;
    _czarCancelAfterRestartQId = lastQId;
}

ActiveWorker::Ptr ActiveWorkerMap::getActiveWorker(string const& workerId) const {
    lock_guard<mutex> lck(_awMapMtx);
    auto iter = _awMap.find(workerId);
    if (iter == _awMap.end()) return nullptr;
    return iter->second;
}

void ActiveWorkerMap::sendActiveWorkersMessages() {
    // Send messages to each active worker as needed
    lock_guard<mutex> lck(_awMapMtx);
    for (auto&& [wName, awPtr] : _awMap) {
        awPtr->updateStateAndSendMessages(_timeoutAliveSecs, _timeoutDeadSecs, _maxLifetime);
    }
}

void ActiveWorkerMap::addToDoneDeleteFiles(QueryId qId) {
    lock_guard<mutex> lck(_awMapMtx);
    for (auto const& [wName, awPtr] : _awMap) {
        awPtr->addToDoneDeleteFiles(qId);
        awPtr->removeDeadUberJobsFor(qId);
    }
}

void ActiveWorkerMap::addToDoneKeepFiles(QueryId qId) {
    lock_guard<mutex> lck(_awMapMtx);
    for (auto const& [wName, awPtr] : _awMap) {
        awPtr->addToDoneKeepFiles(qId);
        awPtr->removeDeadUberJobsFor(qId);
    }
}

}  // namespace lsst::qserv::czar
