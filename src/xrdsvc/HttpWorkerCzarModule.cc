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
#include "xrdsvc/HttpWorkerCzarModule.h"

// System headers
#include <sstream>
#include <stdexcept>
#include <vector>

// Third party headers
#include "lsst/log/Log.h"

// Qserv headers
#include "http/Client.h"  // TODO:UJ will probably need to be removed
#include "http/Exceptions.h"
#include "http/MetaModule.h"
#include "http/RequestBodyJSON.h"
#include "http/RequestQuery.h"
#include "mysql/MySqlUtils.h"
#include "protojson/UberJobMsg.h"
#include "protojson/WorkerQueryStatusData.h"
#include "qmeta/types.h"
#include "util/Command.h"
#include "util/Error.h"
#include "util/MultiError.h"
#include "util/String.h"
#include "util/Timer.h"
#include "wbase/FileChannelShared.h"
#include "wbase/Task.h"
#include "wbase/UberJobData.h"
#include "wbase/UserQueryInfo.h"
#include "wcontrol/Foreman.h"
#include "wcontrol/WCzarInfoMap.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/ResourceMonitor.h"
#include "wpublish/ChunkInventory.h"
#include "wpublish/QueriesAndChunks.h"
#include "wpublish/QueryStatistics.h"
#include "wsched/BlendScheduler.h"
#include "xrdsvc/SsiProvider.h"
#include "xrdsvc/XrdName.h"

using namespace std;
using json = nlohmann::json;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.HttpReplicaMgt");
}

namespace {
// These markers if reported in the extended error response object of the failed
// requests could be used by a caller for refining the completion status
// of the corresponding Controller-side operation.
// TODO:UJ Are these errors seem useful enought to be centralized ???
json const extErrorInvalidParam = json::object({{"invalid_param", 1}});
json const extErrorReplicaInUse = json::object({{"in_use", 1}});

}  // namespace

namespace lsst::qserv::xrdsvc {

void HttpWorkerCzarModule::process(string const& context, shared_ptr<wcontrol::Foreman> const& foreman,
                                   shared_ptr<qhttp::Request> const& req,
                                   shared_ptr<qhttp::Response> const& resp, string const& subModuleName,
                                   http::AuthType const authType) {
    HttpWorkerCzarModule module(context, foreman, req, resp);
    module.execute(subModuleName, authType);
}

HttpWorkerCzarModule::HttpWorkerCzarModule(string const& context,
                                           shared_ptr<wcontrol::Foreman> const& foreman,
                                           shared_ptr<qhttp::Request> const& req,
                                           shared_ptr<qhttp::Response> const& resp)
        : HttpModule(context, foreman, req, resp) {}

json HttpWorkerCzarModule::executeImpl(string const& subModuleName) {
    string const func = string(__func__) + "[sub-module='" + subModuleName + "']";
    enforceInstanceId(func, wconfig::WorkerConfig::instance()->replicationInstanceId());
    enforceWorkerId(func);
    if (subModuleName == "/queryjob") return _queryJob();
    if (subModuleName == "/querystatus") return _queryStatus();
    throw invalid_argument(context() + func + " unsupported sub-module");
}

json HttpWorkerCzarModule::_queryJob() {
    debug(__func__);
    checkApiVersion(__func__, 34);
    // At this point, API version, correct worker, and auth have been checked.
    json jsRet = _handleQueryJob(__func__);
    return jsRet;
}

json HttpWorkerCzarModule::_handleQueryJob(string const& func) {
    json jsRet;

    try {
        auto const& jsReq = body().objJson;
        auto uberJobMsg = protojson::UberJobMsg::createFromJson(jsReq);

        UberJobId ujId = uberJobMsg->getUberJobId();
        auto ujCzInfo = uberJobMsg->getCzarContactInfo();
        QueryId ujQueryId = uberJobMsg->getQueryId();
        int ujRowLimit = uberJobMsg->getRowLimit();
        auto targetWorkerId = uberJobMsg->getWorkerId();
        uint64_t maxTableSizeMb = uberJobMsg->getMaxTableSizeMb();
        uint64_t const MB_SIZE_BYTES = 1024 * 1024;
        uint64_t maxTableSizeBytes = maxTableSizeMb * MB_SIZE_BYTES;
        auto scanInfo = uberJobMsg->getScanInfo();
        bool scanInteractive = uberJobMsg->getScanInteractive();

        // Get or create QueryStatistics and UserQueryInfo instances.
        auto queryStats = foreman()->getQueriesAndChunks()->addQueryId(ujQueryId, ujCzInfo->czId);
        auto userQueryInfo = queryStats->getUserQueryInfo();

        if (userQueryInfo->getCancelledByCzar()) {
            throw wbase::TaskException(
                    ERR_LOC, string("Already cancelled by czar. ujQueryId=") + to_string(ujQueryId));
        }
        if (userQueryInfo->isUberJobDead(ujId)) {
            throw wbase::TaskException(ERR_LOC, string("UberJob already dead. ujQueryId=") +
                                                        to_string(ujQueryId) + " ujId=" + to_string(ujId));
        }

        std::shared_ptr<wcontrol::Foreman> foremanPtr = foreman();
        std::string authKeyStr = authKey();

        // It is important to create UberJobData at this point as it will be the only way to
        // inform the czar of errors after this function returns.
        auto ujData = wbase::UberJobData::create(ujId, ujCzInfo->czName, ujCzInfo->czId, ujCzInfo->czHostName,
                                                 ujCzInfo->czPort, ujQueryId, ujRowLimit, maxTableSizeBytes,
                                                 scanInfo, scanInteractive, targetWorkerId, foremanPtr,
                                                 authKeyStr, foremanPtr->httpPort());

        auto lFunc = [ujId, ujQueryId, ujCzInfo, ujRowLimit, maxTableSizeBytes, targetWorkerId, userQueryInfo,
                      uberJobMsg, foremanPtr, authKeyStr, ujData](util::CmdData*) {
            _buildTasks(ujId, ujQueryId, ujCzInfo, ujRowLimit, maxTableSizeBytes, targetWorkerId,
                        userQueryInfo, uberJobMsg, foremanPtr, authKeyStr, ujData);
        };

        util::Command::Ptr taskLoadCmd = std::make_shared<util::Command>(lFunc);
        foremanPtr->getScheduler()->queTaskLoad(taskLoadCmd);

        string note = string("qId=") + to_string(ujQueryId) + " ujId=" + to_string(ujId);
        jsRet = {{"success", 1}, {"errortype", "none"}, {"note", note}};
        LOGS(_log, LOG_LVL_TRACE, "_handleQueryJob jsRet=" << jsRet);
    } catch (wbase::TaskException const& texp) {
        LOGS(_log, LOG_LVL_ERROR,
             "HttpWorkerCzarModule::_handleQueryJob wbase::TaskException received " << texp.what());
        jsRet = {{"success", 0}, {"errortype", "parse"}, {"note", texp.what()}};
    }
    return jsRet;
}

void HttpWorkerCzarModule::_buildTasks(UberJobId ujId, QueryId ujQueryId,
                                       protojson::CzarContactInfo::Ptr const& ujCzInfo, int ujRowLimit,
                                       uint64_t maxTableSizeBytes, string const& targetWorkerId,
                                       std::shared_ptr<wbase::UserQueryInfo> const& userQueryInfo,
                                       protojson::UberJobMsg::Ptr const& uberJobMsg,
                                       shared_ptr<wcontrol::Foreman> const& foremanPtr,
                                       string const& authKeyStr, wbase::UberJobData::Ptr const& ujData) {
    try {
        LOGS(_log, LOG_LVL_TRACE, __func__ << " qid=" << ujQueryId << "ujId=" << ujId);
        util::Timer timerParse;
        timerParse.start();
        auto czarId = ujCzInfo->czId;

        // Find the entry for this queryId, create a new one if needed.
        userQueryInfo->addUberJob(ujData);
        auto channelShared = wbase::FileChannelShared::create(ujData, ujCzInfo->czId, ujCzInfo->czHostName,
                                                              ujCzInfo->czPort, targetWorkerId);

        ujData->setFileChannelShared(channelShared);

        auto ujTasks = wbase::Task::createTasksFromUberJobMsg(
                uberJobMsg, ujData, channelShared, foremanPtr->chunkResourceMgr(), foremanPtr->mySqlConfig(),
                foremanPtr->sqlConnMgr(), foremanPtr->queriesAndChunks());
        channelShared->setTaskCount(ujTasks.size());
        ujData->addTasks(ujTasks);

        // At this point, it looks like the message was sent successfully.
        wcontrol::WCzarInfoMap::Ptr wCzarMap = foremanPtr->getWCzarInfoMap();
        wcontrol::WCzarInfo::Ptr wCzarInfo = wCzarMap->getWCzarInfo(czarId);
        wCzarInfo->czarMsgReceived(CLOCK::now());

        timerParse.stop();
        util::Timer timer;
        timer.start();
        foremanPtr->processTasks(ujTasks);  // Queues tasks to be run later.
        timer.stop();

        LOGS(_log, LOG_LVL_DEBUG,
             __func__ << " Enqueued UberJob time=" << timer.getElapsed()
                      << " parseTime=" << timerParse.getElapsed() << " " << uberJobMsg->getIdStr());
    } catch (wbase::TaskException const& texp) {
        LOGS(_log, LOG_LVL_ERROR,
             "HttpWorkerCzarModule::_buildTasks wbase::TaskException received " << texp.what());
        // Send a message back saying this UberJobFailed
        util::MultiError multiErr;
        util::Error err(-1, string("UberJob parse error ") + texp.what());
        multiErr.push_back(err);
        ujData->responseError(multiErr, -1, false, LOG_LVL_ERROR);
    }
}

json HttpWorkerCzarModule::_queryStatus() {
    debug(__func__);
    checkApiVersion(__func__, 34);
    // At this point, API version, correct worker, and auth have been checked.
    json jsRet = _handleQueryStatus(__func__);
    return jsRet;
}

json HttpWorkerCzarModule::_handleQueryStatus(std::string const& func) {
    json jsRet;
    auto now = CLOCK::now();
    auto const workerConfig = wconfig::WorkerConfig::instance();
    auto const replicationInstanceId = workerConfig->replicationInstanceId();
    auto const replicationAuthKey = workerConfig->replicationAuthKey();

    auto const& jsReq = body().objJson;
    auto wqsData = protojson::WorkerQueryStatusData::createFromJson(jsReq, replicationInstanceId,
                                                                    replicationAuthKey, now);

    auto const czInfo = wqsData->getCzInfo();
    LOGS(_log, LOG_LVL_TRACE, " HttpWorkerCzarModule::_handleQueryStatus req=" << jsReq.dump());
    CzarIdType czId = czInfo->czId;
    wcontrol::WCzarInfoMap::Ptr wCzarMap = foreman()->getWCzarInfoMap();
    wcontrol::WCzarInfo::Ptr wCzarInfo = wCzarMap->getWCzarInfo(czId);
    wCzarInfo->czarMsgReceived(CLOCK::now());

    // For all queryId and czarId items, if the item can't be found, it is simply ignored. Anything that
    // is missed will eventually be picked up by other mechanisms, such as results being rejected
    // by the czar. This almost never happen, but the system should respond gracefully.

    // If a czar was restarted, cancel and delete the abandoned items.
    if (wqsData->isCzarRestart()) {
        auto restartCzarId = wqsData->getCzarRestartCzarId();
        auto restartQId = wqsData->getCzarRestartQueryId();
        if (restartCzarId > 0 && restartQId > 0) {
            wbase::FileChannelShared::cleanUpResultsOnCzarRestart(wqsData->getCzarRestartCzarId(),
                                                                  wqsData->getCzarRestartQueryId());
        }
    }

    // Take the values from the lists in the message to cancel the
    // appropriate queries and tasks as needed.
    auto const queriesAndChunks = foreman()->queriesAndChunks();
    vector<wbase::UserQueryInfo::Ptr> cancelledList;
    vector<wbase::UserQueryInfo::Ptr> deleteFilesList;
    {
        // Cancelled queries where we want to keep the files
        lock_guard mapLg(wqsData->mapMtx);
        for (auto const& [dkQid, dkTm] : wqsData->qIdDoneKeepFiles) {
            auto qStats = queriesAndChunks->addQueryId(dkQid, czId);
            if (qStats != nullptr) {
                auto uqInfo = qStats->getUserQueryInfo();
                if (uqInfo != nullptr) {
                    if (!uqInfo->getCancelledByCzar()) {
                        cancelledList.push_back(uqInfo);
                    }
                }
            }
        }
        for (auto const& [dkQid, dkTm] : wqsData->qIdDoneDeleteFiles) {
            auto qStats = queriesAndChunks->addQueryId(dkQid, czId);
            if (qStats != nullptr) {
                auto uqInfo = qStats->getUserQueryInfo();
                if (uqInfo != nullptr) {
                    if (!uqInfo->getCancelledByCzar()) {
                        cancelledList.push_back(uqInfo);
                    }
                    deleteFilesList.push_back(uqInfo);
                }
            }
        }
    }

    // Cancel everything in the cancelled list.
    for (auto const& canUqInfo : cancelledList) {
        canUqInfo->cancelFromCzar();
    }

    // For dead UberJobs, add them to a list of dead uberjobs within UserQueryInfo.
    // UserQueryInfo will cancel the tasks in the uberjobs if they exist.
    // New UberJob Id's will be checked against the list, and immediately be
    // killed if they are on it. (see HttpWorkerCzarModule::_handleQueryJob)
    for (auto const& [ujQid, ujIdMap] : wqsData->qIdDeadUberJobs) {
        auto qStats = queriesAndChunks->addQueryId(ujQid, czId);
        if (qStats != nullptr) {
            auto uqInfo = qStats->getUserQueryInfo();
            if (uqInfo != nullptr) {
                if (!uqInfo->getCancelledByCzar()) {
                    for (auto const& [ujId, tm] : ujIdMap) {
                        uqInfo->cancelUberJob(ujId);
                    }
                }
            }
        }
    }

    // Delete files that should be deleted
    CzarIdType czarId = wqsData->getCzInfo()->czId;
    for (wbase::UserQueryInfo::Ptr uqiPtr : deleteFilesList) {
        if (uqiPtr == nullptr) continue;
        QueryId qId = uqiPtr->getQueryId();
        wbase::FileChannelShared::cleanUpResults(czarId, qId);
    }
    // Syntax errors in the message would throw invalid_argument, which is handled elsewhere.

    // Return a message containing lists of the queries that were cancelled.
    jsRet = wqsData->serializeResponseJson(foreman()->getWorkerStartupTime());
    wCzarInfo->sendWorkerCzarComIssueIfNeeded(wqsData->getWInfo(), wqsData->getCzInfo());
    return jsRet;
}

}  // namespace lsst::qserv::xrdsvc
