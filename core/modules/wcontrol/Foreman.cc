// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2008-2016 AURA/LSST.
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
#include "wcontrol/Foreman.h"

// System headers
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/UnsupportedError.h"
#include "mysql/MySqlConfig.h"
#include "proto/worker.pb.h"
#include "wbase/Base.h"
#include "wbase/SendChannelShared.h"
#include "wbase/WorkerCommand.h"
#include "wcontrol/SqlConnMgr.h"
#include "wdb/ChunkResource.h"
#include "wdb/QueryRunner.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wcontrol.Foreman");
}

using namespace std;

namespace lsst {
namespace qserv {
namespace wcontrol {

Foreman::Foreman(Scheduler::Ptr                  const& scheduler,
                 unsigned int                    poolSize,
                 unsigned int                    maxPoolThreads,
                 mysql::MySqlConfig              const& mySqlConfig,
                 wpublish::QueriesAndChunks::Ptr const& queries,
                 wcontrol::SqlConnMgr::Ptr       const& sqlConnMgr)

    :   _scheduler  (scheduler),
        _mySqlConfig(mySqlConfig),
        _queries    (queries),
        _sqlConnMgr (sqlConnMgr) {

    // Make the chunk resource mgr
    // Creating backend makes a connection to the database for making temporary tables.
    // It will delete temporary tables that it can identify as being created by a worker.
    // Previous instances of the worker will terminate when they try to use or create temporary tables.
    // Previous instances of the worker should be terminated before a new worker is started.
    _backend = make_shared<wdb::SQLBackend>(_mySqlConfig);
    _chunkResourceMgr = wdb::ChunkResourceMgr::newMgr(_backend);

    assert(_scheduler); // Cannot operate without scheduler.

    LOGS(_log, LOG_LVL_DEBUG, "poolSize=" << poolSize << " maxPoolThreads=" << maxPoolThreads);
    _pool = util::ThreadPool::newThreadPool(poolSize,  maxPoolThreads, _scheduler);

    _workerCommandQueue = make_shared<util::CommandQueue>();
    _workerCommandPool  = util::ThreadPool::newThreadPool(poolSize, _workerCommandQueue);

}

Foreman::~Foreman() {
    LOGS(_log, LOG_LVL_DEBUG, "Foreman::~Foreman()");
    // It will take significant effort to have xrootd shutdown cleanly and this will never get called
    // until that happens.
    _pool->shutdownPool();
}


void Foreman::_setRunFunc(shared_ptr<wbase::Task> const& task) {
    auto func = [this, task](util::CmdData*){
        proto::TaskMsg const& msg = task->msg;
        int const resultProtocol = 2; // See proto/worker.proto Result protocol
        if (!msg.has_protocol() || msg.protocol() < resultProtocol) {
            LOGS(_log, LOG_LVL_WARN, "processMsg Unsupported wire protocol");
            if (!task->checkCancelled()) {
                // We should not send anything back to xrootd if the task has been cancelled.
                lock_guard<mutex> streamLock(task->sendChannel->streamMutex);
                task->sendChannel->sendError(streamLock, "Unsupported wire protocol", 1);
            }
        } else {
            auto qr = wdb::QueryRunner::newQueryRunner(task, _chunkResourceMgr, _mySqlConfig,
                                                       _sqlConnMgr);
            bool success = false;
            try {
                success = qr->runQuery();
            } catch (UnsupportedError const& e) {
                LOGS(_log, LOG_LVL_ERROR, "runQuery threw UnsupportedError " << e.what() << *task);
            }
            if (not success) {
                lock_guard<mutex> streamLock(task->sendChannel->streamMutex);
                LOGS(_log, LOG_LVL_ERROR, "runQuery failed " << *task);
                if (not task->sendChannel->kill(streamLock, "Foreman::_setRunFunc")) {
                    LOGS(_log, LOG_LVL_WARN, "runQuery sendChannel killed");
                }
            }
        }
        // Transmission is done, but 'task' contains statistics that are still useful, but
        // the resources used by sendChannel need to be freed quickly.
        //   The QueryRunner class access to sendChannel for results is over by this point,
        //   so this wont be an issue there.
        task->sendChannel.reset(); // Frees its xrdsvc::SsiRequest object.
    };

    task->setFunc(func);
}


/// Put the task on the scheduler to be run later.
void Foreman::processTasks(vector<wbase::Task::Ptr> const& tasks) {
    std::vector<util::Command::Ptr> cmds;
    for (auto const& task:tasks) {
        _setRunFunc(task);
        _queries->addTask(task);
        cmds.push_back(task);
    }
    _scheduler->queCmd(cmds);
}


void Foreman::processCommand(shared_ptr<wbase::WorkerCommand> const& command) {
    _workerCommandQueue->queCmd(command);
}


nlohmann::json Foreman::statusToJson() {
    nlohmann::json status;
    status["queries"] = _queries->statusToJson();
    return status;
}

}}} // namespace
