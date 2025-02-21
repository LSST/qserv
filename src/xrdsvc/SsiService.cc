// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 LSST Corporation.
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
#include "xrdsvc/SsiService.h"

// System headers
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <ranges>
#include <string>
#include <stdexcept>
#include <stdlib.h>
#include <thread>
#include <unistd.h>
#include <vector>

// Third-party headers
#include <nlohmann/json.hpp>
#include "XrdSsi/XrdSsiLogger.hh"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "http/Client.h"
#include "http/MetaModule.h"
#include "http/Method.h"
#include "memman/MemMan.h"
#include "memman/MemManNone.h"
#include "mysql/MySqlConfig.h"
#include "mysql/MySqlConnection.h"
#include "qhttp/Server.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "util/common.h"
#include "util/FileMonitor.h"
#include "util/HoldTrack.h"
#include "wbase/Base.h"
#include "wbase/FileChannelShared.h"
#include "wconfig/WorkerConfig.h"
#include "wconfig/WorkerConfigError.h"
#include "wcontrol/Foreman.h"
#include "wcontrol/SqlConnMgr.h"
#include "wpublish/ChunkInventory.h"
#include "wsched/BlendScheduler.h"
#include "wsched/FifoScheduler.h"
#include "wsched/GroupScheduler.h"
#include "wsched/ScanScheduler.h"
#include "xrdsvc/HttpSvc.h"
#include "xrdsvc/XrdName.h"

using namespace lsst::qserv;
using namespace nlohmann;
using namespace std;
using namespace std::literals;

class XrdPosixCallBack;  // Forward.

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.SsiService");

// add LWP to MDC in log messages
void initMDC() { LOG_MDC("LWP", to_string(lsst::log::lwpID())); }
int dummyInitMDC = LOG_MDC_INIT(initMDC);

std::shared_ptr<wpublish::ChunkInventory> makeChunkInventory(mysql::MySqlConfig const& mySqlConfig) {
    xrdsvc::XrdName x;
    if (!mySqlConfig.dbName.empty()) {
        LOGS(_log, LOG_LVL_FATAL, "dbName must be empty to prevent accidental context");
        throw runtime_error("dbName must be empty to prevent accidental context");
    }
    auto conn = sql::SqlConnectionFactory::make(mySqlConfig);
    assert(conn);
    auto inventory = make_shared<wpublish::ChunkInventory>(x.getName(), conn);
    ostringstream os;
    os << "Paths exported: ";
    inventory->dbgPrint(os);
    LOGS(_log, LOG_LVL_DEBUG, os.str());
    return inventory;
}

/**
 * This function will keep periodically updating worker's info in the Replication
 * System's Registry.
 * @param id The unique identifier of a worker to be registered.
 * @note The thread will terminate the process if the registraton request to the Registry
 * was explicitly denied by the service. This means the application may be misconfigured.
 * Transient communication errors when attempting to connect or send requests to
 * the Registry will be posted into the log stream and ignored.
 */
void registryUpdateLoop(string const& id) {
    auto const workerConfig = wconfig::WorkerConfig::instance();
    auto const method = http::Method::POST;
    string const url = "http://" + workerConfig->replicationRegistryHost() + ":" +
                       to_string(workerConfig->replicationRegistryPort()) + "/qserv-worker";
    vector<string> const headers = {"Content-Type: application/json"};
    json const request = json::object({{"version", http::MetaModule::version},
                                       {"instance_id", workerConfig->replicationInstanceId()},
                                       {"auth_key", workerConfig->replicationAuthKey()},
                                       {"worker",
                                        {{"name", id},
                                         {"management-port", workerConfig->replicationHttpPort()},
                                         {"management-host-name", util::get_current_host_fqdn()}}}});
    string const requestContext =
            "SsiService: '" + http::method2string(method) + "' request to '" + url + "'";
    http::Client client(method, url, request.dump(), headers);
    while (true) {
        try {
            json const response = client.readAsJson();
            if (0 == response.at("success").get<int>()) {
                string const error = response.at("error").get<string>();
                LOGS(_log, LOG_LVL_ERROR, requestContext + " was denied, error: '" + error + "'.");
                abort();
            }
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_WARN, requestContext + " failed, ex: " + ex.what());
        }
        this_thread::sleep_for(chrono::seconds(max(1U, workerConfig->replicationRegistryHearbeatIvalSec())));
    }
}

}  // namespace

namespace lsst::qserv::xrdsvc {

SsiService::SsiService(XrdSsiLogger* log) {
    LOGS(_log, LOG_LVL_DEBUG, "SsiService starting...");

    util::HoldTrack::setup(10min);

    auto const mySqlConfig = wconfig::WorkerConfig::instance()->getMySqlConfig();
    if (not mysql::MySqlConnection::checkConnection(mySqlConfig)) {
        LOGS(_log, LOG_LVL_FATAL, "Unable to connect to MySQL using configuration:" << mySqlConfig);
        throw wconfig::WorkerConfigError("Unable to connect to MySQL");
    }
    auto const workerConfig = wconfig::WorkerConfig::instance();
    string cfgMemMan = workerConfig->getMemManClass();
    memman::MemMan::Ptr memMan;
    if (cfgMemMan == "MemManReal") {
        // Default to 1 gigabyte
        uint64_t memManSize = workerConfig->getMemManSizeMb() * 1000000;
        LOGS(_log, LOG_LVL_DEBUG,
             "Using MemManReal with memManSizeMb=" << workerConfig->getMemManSizeMb()
                                                   << " location=" << workerConfig->getMemManLocation());
        memMan = shared_ptr<memman::MemMan>(
                memman::MemMan::create(memManSize, workerConfig->getMemManLocation()));
    } else if (cfgMemMan == "MemManNone") {
        memMan = make_shared<memman::MemManNone>(1, false);
    } else if (cfgMemMan == "MemManNoneRelaxed") {
        bool const alwaysLock = true;
        memMan = make_shared<memman::MemManNone>(1, alwaysLock);
    } else {
        LOGS(_log, LOG_LVL_ERROR, "Unrecognized memory manager " << cfgMemMan);
        throw wconfig::WorkerConfigError("Unrecognized memory manager.");
    }

    // Set thread pool size.
    unsigned int poolSize = ranges::max({wsched::BlendScheduler::getMinPoolSize(),
                                         workerConfig->getThreadPoolSize(), thread::hardware_concurrency()});

    unsigned int maxPoolThreads = max(workerConfig->getMaxPoolThreads(), poolSize);

    // poolSize should be greater than either GroupScheduler::maxThreads or ScanScheduler::maxThreads
    unsigned int maxThread = poolSize;
    int maxReserve = 2;
    auto group = make_shared<wsched::GroupScheduler>("SchedGroup", maxThread, maxReserve,
                                                     workerConfig->getMaxGroupSize(),
                                                     wsched::SchedulerBase::getMaxPriority());

    int const fastest = lsst::qserv::protojson::ScanInfo::Rating::FASTEST;
    int const fast = lsst::qserv::protojson::ScanInfo::Rating::FAST;
    int const medium = lsst::qserv::protojson::ScanInfo::Rating::MEDIUM;
    int const slow = lsst::qserv::protojson::ScanInfo::Rating::SLOW;
    int const slowest = lsst::qserv::protojson::ScanInfo::Rating::SLOWEST;
    double fastScanMaxMinutes = (double)workerConfig->getScanMaxMinutesFast();
    double medScanMaxMinutes = (double)workerConfig->getScanMaxMinutesMed();
    double slowScanMaxMinutes = (double)workerConfig->getScanMaxMinutesSlow();
    double snailScanMaxMinutes = (double)workerConfig->getScanMaxMinutesSnail();
    int maxTasksBootedPerUserQuery = workerConfig->getMaxTasksBootedPerUserQuery();
    int maxConcurrentBootedTasks = workerConfig->getMaxConcurrentBootedTasks();
    vector<wsched::ScanScheduler::Ptr> scanSchedulers{
            make_shared<wsched::ScanScheduler>("SchedSlow", maxThread, workerConfig->getMaxReserveSlow(),
                                               workerConfig->getPrioritySlow(),
                                               workerConfig->getMaxActiveChunksSlow(), memMan, medium + 1,
                                               slow, slowScanMaxMinutes),
            make_shared<wsched::ScanScheduler>("SchedFast", maxThread, workerConfig->getMaxReserveFast(),
                                               workerConfig->getPriorityFast(),
                                               workerConfig->getMaxActiveChunksFast(), memMan, fastest, fast,
                                               fastScanMaxMinutes),
            make_shared<wsched::ScanScheduler>(
                    "SchedMed", maxThread, workerConfig->getMaxReserveMed(), workerConfig->getPriorityMed(),
                    workerConfig->getMaxActiveChunksMed(), memMan, fast + 1, medium, medScanMaxMinutes),
    };

    auto snail = make_shared<wsched::ScanScheduler>(
            "SchedSnail", maxThread, workerConfig->getMaxReserveSnail(), workerConfig->getPrioritySnail(),
            workerConfig->getMaxActiveChunksSnail(), memMan, slow + 1, slowest, snailScanMaxMinutes);

    wpublish::QueriesAndChunks::Ptr queries = wpublish::QueriesAndChunks::setupGlobal(
            chrono::minutes(5), chrono::minutes(2), maxTasksBootedPerUserQuery, maxConcurrentBootedTasks,
            false);
    wsched::BlendScheduler::Ptr blendSched = make_shared<wsched::BlendScheduler>(
            "BlendSched", queries, maxThread, group, snail, scanSchedulers);
    blendSched->setPrioritizeByInFlight(workerConfig->getPrioritizeByInFlight());
    queries->setBlendScheduler(blendSched);

    unsigned int requiredTasksCompleted = workerConfig->getRequiredTasksCompleted();
    queries->setRequiredTasksCompleted(requiredTasksCompleted);

    int const maxSqlConn = workerConfig->getMaxSqlConnections();
    int const resvInteractiveSqlConn = workerConfig->getReservedInteractiveSqlConnections();
    auto sqlConnMgr = make_shared<wcontrol::SqlConnMgr>(maxSqlConn, maxSqlConn - resvInteractiveSqlConn);
    LOGS(_log, LOG_LVL_WARN, "config sqlConnMgr" << *sqlConnMgr);
    LOGS(_log, LOG_LVL_WARN, "maxPoolThreads=" << maxPoolThreads);

    int qPoolSize = workerConfig->getQPoolSize();
    int maxPriority = workerConfig->getQPoolMaxPriority();
    string vectRunSizesStr = workerConfig->getQPoolRunSizes();
    string vectMinRunningSizesStr = workerConfig->getQPoolMinRunningSizes();

    _foreman = wcontrol::Foreman::create(blendSched, poolSize, maxPoolThreads, mySqlConfig, queries,
                                         ::makeChunkInventory(mySqlConfig), sqlConnMgr, qPoolSize,
                                         maxPriority, vectRunSizesStr, vectMinRunningSizesStr);

    // Watch to see if the log configuration is changed.
    // If LSST_LOG_CONFIG is not defined, there's no good way to know what log
    // configuration file is in use.
    string logConfigFile = std::getenv("LSST_LOG_CONFIG");
    if (logConfigFile == "") {
        LOGS(_log, LOG_LVL_ERROR,
             "FileMonitor LSST_LOG_CONFIG was blank, no log configuration file to watch.");
    } else {
        LOGS(_log, LOG_LVL_ERROR, "logConfigFile=" << logConfigFile);
        _logFileMonitor = make_shared<util::FileMonitor>(logConfigFile);
    }

    // Garbage collect unclaimed result files (if any).
    // ATTENTION: this is the blocking operation since it needs to be run before accepting
    // new queries to ensure that worker had sufficient resources to process those.
    if (workerConfig->resultsCleanUpOnStart()) {
        wbase::FileChannelShared::cleanUpResultsOnWorkerRestart();
    }

    // Start the control server for processing worker management requests sent
    // by the Replication System. Update the port number in the configuration
    // in case if the server is run on the dynamically allocated port.
    _controlHttpSvc = HttpSvc::create(_foreman, workerConfig->replicationHttpPort(),
                                      workerConfig->getCzarComNumHttpThreads());

    auto const port = _controlHttpSvc->start();
    workerConfig->setReplicationHttpPort(port);

    // Begin periodically updating worker's status in the Replication System's registry
    // in the detached thread. This will continue before the application gets terminated.
    thread registryUpdateThread(::registryUpdateLoop, _foreman->chunkInventory()->id());
    registryUpdateThread.detach();
}

SsiService::~SsiService() {
    LOGS(_log, LOG_LVL_DEBUG, "SsiService dying.");
    _controlHttpSvc->stop();
}

void SsiService::ProcessRequest(XrdSsiRequest& reqRef, XrdSsiResource& resRef) {
    LOGS(_log, LOG_LVL_ERROR, "SsiService::ProcessRequest got called");
}

}  // namespace lsst::qserv::xrdsvc
