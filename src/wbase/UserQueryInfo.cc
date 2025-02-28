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
#include "wbase/UserQueryInfo.h"

// Qserv headers
#include "util/Bug.h"
#include "wbase/UberJobData.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wbase.UserQueryInfo");
}

namespace lsst::qserv::wbase {

UserQueryInfo::UserQueryInfo(QueryId qId, CzarIdType czarId) : _qId(qId), _czarId(czarId) {}

size_t UserQueryInfo::addTemplate(std::string const& templateStr) {
    size_t j = 0;
    {
        lock_guard<mutex> lockUq(_uqMtx);
        for (; j < _templates.size(); ++j) {
            if (_templates[j] == templateStr) {
                return j;
            }
        }
        _templates.emplace_back(templateStr);
    }
    LOGS(_log, LOG_LVL_DEBUG, "QueryInfo:" << _qId << " j=" << j << " Added:" << templateStr);
    return j;
}

std::string UserQueryInfo::getTemplate(size_t id) {
    lock_guard<mutex> lockUq(_uqMtx);
    if (id >= _templates.size()) {
        throw util::Bug(ERR_LOC, "UserQueryInfo template index out of range id=" + to_string(id) +
                                         " size=" + to_string(_templates.size()));
    }
    return _templates[id];
}

void UserQueryInfo::addUberJob(std::shared_ptr<UberJobData> const& ujData) {
    lock_guard<mutex> lockUq(_uberJobMapMtx);
    UberJobId ujId = ujData->getUberJobId();
    _uberJobMap[ujId] = ujData;
}

void UserQueryInfo::cancelFromCzar() {
    if (_cancelledByCzar.exchange(true)) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " already cancelledByCzar");
        return;
    }
    lock_guard<mutex> lockUq(_uberJobMapMtx);
    for (auto const& [ujId, weakUjPtr] : _uberJobMap) {
        LOGS(_log, LOG_LVL_INFO, cName(__func__) << " cancelling ujId=" << ujId);
        auto ujPtr = weakUjPtr.lock();
        if (ujPtr != nullptr) {
            ujPtr->cancelAllTasks();
        }
    }
}

void UserQueryInfo::cancelUberJob(UberJobId ujId) {
    LOGS(_log, LOG_LVL_INFO, cName(__func__) << " cancelling ujId=" << ujId);
    lock_guard<mutex> lockUq(_uberJobMapMtx);
    _deadUberJobSet.insert(ujId);
    auto iter = _uberJobMap.find(ujId);
    if (iter != _uberJobMap.end()) {
        auto weakUjPtr = iter->second;
        auto ujPtr = weakUjPtr.lock();
        if (ujPtr != nullptr) {
            ujPtr->cancelAllTasks();
        }
    }
}

void UserQueryInfo::cancelAllUberJobs() {
    lock_guard<mutex> lockUq(_uberJobMapMtx);
    for (auto const& [ujKey, weakUjPtr] : _uberJobMap) {
        _deadUberJobSet.insert(ujKey);
        auto ujPtr = weakUjPtr.lock();
        if (ujPtr != nullptr) {
            ujPtr->cancelAllTasks();
        }
    }
}

bool UserQueryInfo::isUberJobDead(UberJobId ujId) const {
    lock_guard<mutex> lockUq(_uberJobMapMtx);
    auto iter = _deadUberJobSet.find(ujId);
    return iter != _deadUberJobSet.end();
}

}  // namespace lsst::qserv::wbase
