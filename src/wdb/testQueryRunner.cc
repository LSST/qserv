// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
/**
 * @brief Simple testing for class QueryRunner
 * Requires some setup, and assumes some access to a mysqld
 *
 * @author Daniel L. Wang, SLAC
 */

// Qserv headers
#include "mysql/MySqlConfig.h"
#include "protojson/ScanTableInfo.h"
#include "proto/worker.pb.h"
#include "wbase/FileChannelShared.h"
#include "wbase/Task.h"
#include "wbase/UberJobData.h"
#include "wconfig/WorkerConfig.h"
#include "wcontrol/Foreman.h"
#include "wcontrol/SqlConnMgr.h"
#include "wdb/ChunkResource.h"
#include "wdb/QueryRunner.h"
#include "wpublish/QueriesAndChunks.h"

// Boost unit test header
#define BOOST_TEST_MODULE QueryRunner
#include <boost/test/unit_test.hpp>

using namespace std;

namespace test = boost::test_tools;
namespace gio = google::protobuf::io;
namespace util = lsst::qserv::util;

using lsst::qserv::mysql::MySqlConfig;
using lsst::qserv::mysql::MySqlConnection;

using lsst::qserv::wbase::FileChannelShared;
using lsst::qserv::wbase::SendChannel;
using lsst::qserv::wbase::Task;
using lsst::qserv::wconfig::WorkerConfig;
using lsst::qserv::wcontrol::SqlConnMgr;
using lsst::qserv::wdb::ChunkResource;
using lsst::qserv::wdb::ChunkResourceMgr;
using lsst::qserv::wdb::FakeBackend;
using lsst::qserv::wdb::QueryRunner;
using lsst::qserv::wpublish::QueriesAndChunks;

struct Fixture {
    struct MsgInfo {
        string const db = "LSST";
        string const table = "Object";
        string const qry = "SELECT AVG(yFlux_PS) from LSST.Object_3240";
        int const chunkId = 3240;
        int const czarId = 5;
        string const czarName = "cz5";
        string const czarHostName = "cz5host";
        int const czarPort = 3437;
        string const targWorkerId = "a_worker";
        std::shared_ptr<lsst::qserv::wcontrol::Foreman> foreman;
        int const queryId = 23;
        int const jobId = 1;
        int const uberJobId = 1;
        int const attemptCount = 1;
        int const scanRating = 1;
        bool const scanInteractive = false;
        int const maxTableSize = 5000;
        bool const lockInMemory = false;
        string const resultName = "resName";
        string const authKey = "noAuthKey";
        int const rowLimit = 0;
    };

    shared_ptr<nlohmann::json> newTaskJson(MsgInfo const& mInfo) {
        // Derived from TaskMsgFactory::makeMsgJson

        auto jsJobMsgPtr = std::shared_ptr<nlohmann::json>(
                new nlohmann::json({{"czarId", mInfo.czarId},
                                    {"queryId", mInfo.queryId},
                                    {"jobId", mInfo.jobId},
                                    {"attemptCount", mInfo.attemptCount},
                                    {"querySpecDb", mInfo.db},
                                    {"maxTableSize", mInfo.maxTableSize},
                                    {"chunkScanTables", nlohmann::json::array()},
                                    {"chunkId", mInfo.chunkId},
                                    {"queryFragments", nlohmann::json::array()}}));

        auto& jsJobMsg = *jsJobMsgPtr;

        auto& chunkScanTables = jsJobMsg["chunkScanTables"];
        nlohmann::json cst = {{"db", mInfo.db},
                              {"table", mInfo.table},
                              {"lockInMemory", mInfo.lockInMemory},
                              {"tblScanRating", mInfo.scanRating}};
        chunkScanTables.push_back(move(cst));

        auto& jsFragments = jsJobMsg["queryFragments"];
        nlohmann::json jsFrag = {{"resultTable", mInfo.resultName},
                                 {"queries", nlohmann::json::array()},
                                 {"subchunkTables", nlohmann::json::array()},
                                 {"subchunkIds", nlohmann::json::array()}};

        auto& jsQueries = jsFrag["queries"];
        nlohmann::json jsQry = {{"subQuery", mInfo.qry}};
        jsQueries.push_back(move(jsQry));

        jsFragments.push_back(move(jsFrag));

        return jsJobMsgPtr;
    }

    MySqlConfig newMySqlConfig() {
        string user = "qsmaster";
        string password = "";
        string socket = "SET ME HERE";
        MySqlConfig mySqlConfig(user, password, socket);
        if (not MySqlConnection::checkConnection(mySqlConfig)) {
            throw runtime_error("Unable to connect to MySQL database with params: " + mySqlConfig.toString());
        }
        return mySqlConfig;
    }
    shared_ptr<QueriesAndChunks> queriesAndChunks() {
        bool resetForTesting = true;
        int maxTasksBooted = 5;
        int maxDarkTasks = 25;
        return QueriesAndChunks::setupGlobal(chrono::seconds(1), chrono::seconds(120), maxTasksBooted,
                                             maxDarkTasks, resetForTesting);
    }
};

BOOST_FIXTURE_TEST_SUITE(Basic, Fixture, *boost::unit_test::timeout(20))

BOOST_AUTO_TEST_CASE(Simple) {
    WorkerConfig::create();
    MsgInfo mInfo;
    auto msgJson = newTaskJson(mInfo);
    shared_ptr<SendChannel> sendC(SendChannel::newNopChannel());
    auto sChannel = FileChannelShared::create(sendC, mInfo.czarId);
    FakeBackend::Ptr backend = make_shared<FakeBackend>();
    shared_ptr<ChunkResourceMgr> crm = ChunkResourceMgr::newMgr(backend);
    SqlConnMgr::Ptr sqlConnMgr = make_shared<SqlConnMgr>(20, 9);
    auto const queries = queriesAndChunks();

    auto scanInfo = lsst::qserv::protojson::ScanInfo::create();
    scanInfo->scanRating = mInfo.scanRating;
    scanInfo->infoTables.emplace_back(mInfo.db, mInfo.table, mInfo.lockInMemory, mInfo.scanRating);
    auto ujData = lsst::qserv::wbase::UberJobData::create(
            mInfo.uberJobId, mInfo.czarName, mInfo.czarId, mInfo.czarHostName, mInfo.czarPort, mInfo.queryId,
            mInfo.rowLimit, mInfo.maxTableSize, scanInfo, mInfo.scanInteractive, mInfo.targWorkerId,
            mInfo.foreman, mInfo.authKey);

    vector<Task::Ptr> taskVect =
            Task::createTasksForUnitTest(ujData, *msgJson, sChannel, mInfo.maxTableSize, crm);

    Task::Ptr task = taskVect[0];
    QueryRunner::Ptr a(QueryRunner::newQueryRunner(task, crm, newMySqlConfig(), sqlConnMgr, queries));
    BOOST_CHECK(a->runQuery());
}

BOOST_AUTO_TEST_CASE(Output) {
    WorkerConfig::create();
    string out;
    MsgInfo mInfo;
    auto msgJson = newTaskJson(mInfo);
    shared_ptr<SendChannel> sendC(SendChannel::newStringChannel(out));
    auto sc = FileChannelShared::create(sendC, mInfo.czarId);
    FakeBackend::Ptr backend = make_shared<FakeBackend>();
    shared_ptr<ChunkResourceMgr> crm = ChunkResourceMgr::newMgr(backend);
    SqlConnMgr::Ptr sqlConnMgr = make_shared<SqlConnMgr>(20, 9);
    auto const queries = queriesAndChunks();
    auto scanInfo = lsst::qserv::protojson::ScanInfo::create();
    scanInfo->scanRating = mInfo.scanRating;
    scanInfo->infoTables.emplace_back(mInfo.db, mInfo.table, mInfo.lockInMemory, mInfo.scanRating);
    auto ujData = lsst::qserv::wbase::UberJobData::create(
            mInfo.uberJobId, mInfo.czarName, mInfo.czarId, mInfo.czarHostName, mInfo.czarPort, mInfo.queryId,
            mInfo.rowLimit, mInfo.maxTableSize, scanInfo, mInfo.scanInteractive, mInfo.targWorkerId,
            mInfo.foreman, mInfo.authKey);

    vector<Task::Ptr> taskVect = Task::createTasksForUnitTest(ujData, *msgJson, sc, mInfo.maxTableSize, crm);

    Task::Ptr task = taskVect[0];
    QueryRunner::Ptr a(QueryRunner::newQueryRunner(task, crm, newMySqlConfig(), sqlConnMgr, queries));
    BOOST_CHECK(a->runQuery());
}

BOOST_AUTO_TEST_SUITE_END()
