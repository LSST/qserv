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
#include "wcontrol/WCzarInfoMap.h"

#include <cstdio>
#include <float.h>
#include <set>

// Third party headers
#include "nlohmann/json.hpp"

// qserv headers
#include "http/Client.h"
#include "protojson/WorkerQueryStatusData.h"
#include "util/Bug.h"
#include "util/Histogram.h"
#include "wbase/UberJobData.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/Foreman.h"
#include "wpublish/QueriesAndChunks.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

using namespace std::chrono_literals;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wcontrol.WCzarInfoMap");
}

namespace lsst::qserv::wcontrol {

WCzarInfo::WCzarInfo(CzarIdType czarId_)
        : czarId(czarId_),
          _workerCzarComIssue(protojson::WorkerCzarComIssue::create(
                  wconfig::WorkerConfig::instance()->replicationInstanceId(),
                  wconfig::WorkerConfig::instance()->replicationAuthKey())) {}

void WCzarInfo::czarMsgReceived(TIMEPOINT tm) {
    unique_lock<mutex> uniLock(_wciMtx);
    _lastTouch = tm;
    if (_alive.exchange(true) == false) {
        uniLock.unlock();
        LOGS(_log, LOG_LVL_WARN, cName(__func__) << " was dead and is now alive");
        _workerCzarComIssue->setThoughtCzarWasDead(true);
    }
}

void WCzarInfo::sendWorkerCzarComIssueIfNeeded(protojson::WorkerContactInfo::Ptr const& wInfo_,
                                               protojson::CzarContactInfo::Ptr const& czInfo_) {
    unique_lock<mutex> uniLock(_wciMtx);
    if (_workerCzarComIssue->needToSend()) {
        // Having more than one of this message being sent at one time
        // could cause race issues and it would be a problem if it was
        // stuck in a queue, so it gets its own thread.
        if (_msgThreadRunning.exchange(true) == true) {
            LOGS(_log, LOG_LVL_INFO, cName(__func__) << " message thread already running");
            return;
        }
        _workerCzarComIssue->setContactInfo(wInfo_, czInfo_);
        auto selfPtr = weak_from_this();
        auto thrdFunc = [selfPtr]() {
            auto sPtr = selfPtr.lock();
            if (sPtr == nullptr) {
                LOGS(_log, LOG_LVL_WARN, "WCzarInfo::sendWorkerCzarComIssueIfNeeded thrdFunc sPtr was null");
            }
            sPtr->_sendMessage();
        };

        thread thrd(thrdFunc);
        thrd.detach();
    }
}

void WCzarInfo::_sendMessage() {
    // Make certain _msgThreadRunning is set to false when this function ends.
    class ClearMsgThreadRunning {
    public:
        ClearMsgThreadRunning(WCzarInfo* wcInfo) : _wcInfo(wcInfo) {}
        ~ClearMsgThreadRunning() { _wcInfo->_msgThreadRunning = false; }
        WCzarInfo* const _wcInfo;
    };
    ClearMsgThreadRunning clearMsgThreadRunning(this);

    auto const method = http::Method::POST;

    unique_lock<mutex> uniLock(_wciMtx);
    auto czInfo = _workerCzarComIssue->getCzarInfo();
    // If thoughtCzarWasDead is set now, it needs to be cleared on successful reception from czar.
    bool needToClearThoughtCzarWasDead = _workerCzarComIssue->getThoughtCzarWasDead();
    if (czInfo == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " czar info was null");
        return;
    }
    vector<string> const headers = {"Content-Type: application/json"};
    string const url =
            "http://" + czInfo->czHostName + ":" + to_string(czInfo->czPort) + "/workerczarcomissue";
    auto jsReqPtr = _workerCzarComIssue->serializeJson();
    uniLock.unlock();  // Must unlock before communication

    auto requestStr = jsReqPtr->dump();
    http::Client client(method, url, requestStr, headers);
    bool transmitSuccess = false;
    try {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " read start");
        nlohmann::json const response = client.readAsJson();
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " read end");
        uniLock.lock();
        if (0 != response.at("success").get<int>()) {
            transmitSuccess = true;
            if (needToClearThoughtCzarWasDead) {
                _workerCzarComIssue->setThoughtCzarWasDead(false);
            }
        } else {
            LOGS(_log, LOG_LVL_WARN, cName(__func__) << " Transmit success == 0");
            // There's no point in re-sending as the czar got the message and didn't like
            // it.
            // TODO:UJ Maybe add this czId+ujId to a list of failed uberjobs that can be put
            // TODO:UJ status return?
        }
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_WARN, cName(__func__) + " " + requestStr + " failed, ex: " + ex.what());
    }

    if (!transmitSuccess) {
        // If this fails, wait for
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " failed to send message");
    }
}

bool WCzarInfo::checkAlive(TIMEPOINT tmMark) {
    lock_guard<mutex> lg(_wciMtx);
    if (_alive) {
        auto timeSinceContact = tmMark - _lastTouch;
        std::chrono::seconds deadTime(wconfig::WorkerConfig::instance()->getCzarDeadTimeSec());
        if (timeSinceContact >= deadTime) {
            // Contact with the czar has timed out.
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " czar timeout");
            _alive = false;
            // Kill all queries from this czar
            auto fMan = Foreman::getForeman();
            if (fMan != nullptr) {
                auto queriesAndChunks = fMan->getQueriesAndChunks();
                if (queriesAndChunks != nullptr) {
                    queriesAndChunks->killAllQueriesFromCzar(czarId);
                }
            }
        }
    }
    return _alive;
}

WCzarInfo::Ptr WCzarInfoMap::getWCzarInfo(CzarIdType czId) {
    std::lock_guard lg(_wczMapMtx);
    auto iter = _wczMap.find(czId);
    if (iter == _wczMap.end()) {
        LOGS(_log, LOG_LVL_INFO, cName(__func__) << " new czar contacted " << czId);
        auto const newCzInfo = WCzarInfo::create(czId);
        _wczMap[czId] = newCzInfo;
        return newCzInfo;
    }
    return iter->second;
}

}  // namespace lsst::qserv::wcontrol
