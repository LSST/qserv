/*
 * LSST Data Management System
 * Copyright 2015 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */

// System headers
#include <iostream>
#include <fstream>
#include <string>
#include <unistd.h>  // for getpass

// Third-party headers
#include "boost/algorithm/string/replace.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "qmeta/MessageStore.h"
#include "qmeta/QMetaMysql.h"
#include "qmeta/QStatusMysql.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "sql/SqlErrorObject.h"

// Local headers
#include "Exceptions.h"

// Boost unit test header
#define BOOST_TEST_MODULE QMeta_1
#include <boost/test/unit_test.hpp>

using lsst::qserv::mysql::MySqlConfig;
using namespace lsst::qserv::qmeta;
using lsst::qserv::sql::SqlConnection;
using lsst::qserv::sql::SqlConnectionFactory;
using lsst::qserv::sql::SqlErrorObject;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.qmeta.testQMeta");

struct TestDBGuard {
    TestDBGuard() {
        sqlConfig.hostname = "";
        sqlConfig.port = 0;
        sqlConfig.username = "root";
        sqlConfig.password = getpass("Enter mysql root password: ");
        std::cout << "Enter mysql socket: ";
        std::cin >> sqlConfig.socket;
        sqlConfig.dbName = "testQMetaZ012sdrt";

        std::ifstream schemaFile("admin/templates/configuration/tmp/configure/sql/QueryMetadata.sql");

        // read whole file into buffer
        std::string buffer;
        std::getline(schemaFile, buffer, '\0');

        // replace production schema name with test schema
        boost::replace_all(buffer, "qservMeta", sqlConfig.dbName);

        // need config without database name
        MySqlConfig sqlConfigLocal = sqlConfig;
        sqlConfigLocal.dbName = "";
        auto sqlConn = SqlConnectionFactory::make(sqlConfigLocal);

        SqlErrorObject errObj;
        sqlConn->runQuery(buffer, errObj);
        if (errObj.isSet()) {
            throw SqlError(ERR_LOC, errObj);
        }
    }

    ~TestDBGuard() {
        auto sqlConn = SqlConnectionFactory::make(sqlConfig);
        SqlErrorObject errObj;
        sqlConn->dropDb(sqlConfig.dbName, errObj);
    }

    MySqlConfig sqlConfig;
};

}  // namespace

struct PerTestFixture {
    PerTestFixture() {
        qMeta = std::make_shared<QMetaMysql>(testDB.sqlConfig, 1);
        sqlConn = SqlConnectionFactory::make(testDB.sqlConfig);
    }

    static TestDBGuard testDB;
    std::shared_ptr<SqlConnection> sqlConn;
    std::shared_ptr<QMeta> qMeta;
};

TestDBGuard PerTestFixture::testDB;

BOOST_FIXTURE_TEST_SUITE(SqlConnectionTestSuite, PerTestFixture)

BOOST_AUTO_TEST_CASE(messWithCzars) {
    // check for few non-existing names
    BOOST_CHECK_EQUAL(qMeta->getCzarID(""), 0U);
    BOOST_CHECK_EQUAL(qMeta->getCzarID("unknown"), 0U);

    // start with registering couple of czars
    CzarId cid1 = qMeta->registerCzar("czar:1000");
    BOOST_CHECK_EQUAL(qMeta->getCzarID("czar:1000"), cid1);
    CzarId cid2 = qMeta->registerCzar("czar-2:1000");
    BOOST_CHECK_EQUAL(qMeta->getCzarID("czar-2:1000"), cid2);

    // re-register existing czar, should get the same id
    CzarId cid3 = qMeta->registerCzar("czar-2:1000");
    BOOST_CHECK_EQUAL(cid3, cid2);
    BOOST_CHECK_EQUAL(qMeta->getCzarID("czar-2:1000"), cid3);

    // activate/deactivate, check exceptions
    BOOST_CHECK_NO_THROW(qMeta->setCzarActive(cid1, false));
    BOOST_CHECK_NO_THROW(qMeta->setCzarActive(cid1, true));
    BOOST_CHECK_THROW(qMeta->setCzarActive(9999999, true), CzarIdError);
}

BOOST_AUTO_TEST_CASE(messWithQueries) {
    // make sure that we have czars from previous test
    CzarId cid1 = qMeta->getCzarID("czar:1000");
    BOOST_CHECK(cid1 != 0U);

    // resister one query
    QInfo qinfo(QInfo::SYNC, cid1, "user1", "SELECT * from Object", "SELECT * from Object_{}",
                "SELECT Merge ' query", "result_#QID#", "message_12345", "SELECT obj FROM result_#QID#", 26);
    QMeta::TableNames tables(1, std::make_pair("TestDB", "Object"));
    lsst::qserv::QueryId qid1 = qMeta->registerQuery(qinfo, tables);
    BOOST_CHECK(qid1 != 0U);

    // get query info
    QInfo qinfo1 = qMeta->getQueryInfo(qid1);
    BOOST_CHECK_EQUAL(qinfo1.queryType(), qinfo.queryType());
    BOOST_CHECK_EQUAL(qinfo1.queryStatus(), QInfo::EXECUTING);
    BOOST_CHECK_EQUAL(qinfo1.czarId(), qinfo.czarId());
    BOOST_CHECK_EQUAL(qinfo1.user(), qinfo.user());
    BOOST_CHECK_EQUAL(qinfo1.queryText(), qinfo.queryText());
    BOOST_CHECK_EQUAL(qinfo1.queryTemplate(), qinfo.queryTemplate());
    BOOST_CHECK_EQUAL(qinfo1.mergeQuery(), qinfo.mergeQuery());
    BOOST_CHECK_EQUAL(qinfo1.msgTableName(), qinfo.msgTableName());
    BOOST_CHECK_EQUAL(qinfo1.resultQuery(), qinfo.resultQuery());
    BOOST_CHECK_EQUAL(qinfo1.resultLocation(), "result_" + std::to_string(qid1));
    BOOST_CHECK(qinfo1.submitted() != std::time_t(0));
    BOOST_CHECK_EQUAL(qinfo1.completed(), std::time_t(0));
    BOOST_CHECK_EQUAL(qinfo1.returned(), std::time_t(0));
    BOOST_CHECK_EQUAL(qinfo1.duration(), std::time_t(0));
    BOOST_CHECK_EQUAL(qinfo1.chunkCount(), qinfo.chunkCount());

    // get running queries
    std::vector<lsst::qserv::QueryId> queries = qMeta->getPendingQueries(cid1);
    BOOST_CHECK_EQUAL(queries.size(), 1U);
    BOOST_CHECK_EQUAL(queries[0], qid1);

    queries = qMeta->findQueries();
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::SYNC);
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "user1");
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "user2");
    BOOST_CHECK_EQUAL(queries.size(), 0U);

    queries = qMeta->findQueries(0, QInfo::SYNC, "user1");
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(1, QInfo::EXECUTING));
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    std::vector<QInfo::QStatus> statuses;
    statuses.push_back(QInfo::COMPLETED);
    statuses.push_back(QInfo::FAILED);
    statuses.push_back(QInfo::ABORTED);
    queries = qMeta->findQueries(0, QInfo::ANY, "", statuses);
    BOOST_CHECK_EQUAL(queries.size(), 0U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(), false);
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(), true);
    BOOST_CHECK_EQUAL(queries.size(), 0U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(), -1, false);
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(), -1, true);
    BOOST_CHECK_EQUAL(queries.size(), 0U);

    // update completed status
    BOOST_CHECK_THROW(qMeta->completeQuery(99999, QInfo::ABORTED), QueryIdError);
    qMeta->completeQuery(qid1, QInfo::COMPLETED);

    qinfo1 = qMeta->getQueryInfo(qid1);
    BOOST_CHECK_EQUAL(qinfo1.queryStatus(), QInfo::COMPLETED);
    BOOST_CHECK(qinfo1.submitted() != std::time_t(0));
    BOOST_CHECK(qinfo1.completed() != std::time_t(0));
    BOOST_CHECK_EQUAL(qinfo1.returned(), std::time_t(0));
    BOOST_CHECK(qinfo1.duration() >= std::time_t(0));

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(), true);
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(1, QInfo::COMPLETED));
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(1, QInfo::EXECUTING));
    BOOST_CHECK_EQUAL(queries.size(), 0U);

    // update finished status
    BOOST_CHECK_THROW(qMeta->finishQuery(99999), QueryIdError);
    qMeta->finishQuery(qid1);

    qinfo1 = qMeta->getQueryInfo(qid1);
    BOOST_CHECK(qinfo1.submitted() != std::time_t(0));
    BOOST_CHECK(qinfo1.completed() != std::time_t(0));
    BOOST_CHECK(qinfo1.returned() != std::time_t(0));
    BOOST_CHECK(qinfo1.duration() >= std::time_t(0));

    queries = qMeta->findQueries(0, QInfo::ANY, "", std::vector<QInfo::QStatus>(), -1, true);
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    // no running queries should be there
    queries = qMeta->getPendingQueries(cid1);
    BOOST_CHECK_EQUAL(queries.size(), 0U);
}

BOOST_AUTO_TEST_CASE(messWithQueries2) {
    // make sure that we have czars from previous test
    CzarId cid1 = qMeta->getCzarID("czar:1000");
    BOOST_CHECK(cid1 != 0U);
    CzarId cid2 = qMeta->getCzarID("czar-2:1000");
    BOOST_CHECK(cid2 != 0U);

    // resister few queries
    QInfo qinfo(QInfo::SYNC, cid1, "user1", "SELECT * from Object", "SELECT * from Object_{}", "", "", "", "",
                40);
    QMeta::TableNames tables(1, std::make_pair("TestDB", "Object"));
    lsst::qserv::QueryId qid1 = qMeta->registerQuery(qinfo, tables);
    lsst::qserv::QueryId qid2 = qMeta->registerQuery(qinfo, tables);
    qinfo = QInfo(QInfo::ASYNC, cid2, "user2", "SELECT * from Object", "SELECT * from Object_{}", "", "", "",
                  "", 30);
    lsst::qserv::QueryId qid3 = qMeta->registerQuery(qinfo, tables);
    lsst::qserv::QueryId qid4 = qMeta->registerQuery(qinfo, tables);

    // get running queries
    std::vector<lsst::qserv::QueryId> queries = qMeta->getPendingQueries(cid1);
    BOOST_CHECK_EQUAL(queries.size(), 2U);
    queries = qMeta->getPendingQueries(cid2);
    BOOST_CHECK_EQUAL(queries.size(), 2U);

    // update completed status
    qMeta->completeQuery(qid1, QInfo::COMPLETED);
    qMeta->finishQuery(qid1);
    qMeta->completeQuery(qid3, QInfo::COMPLETED);
    qMeta->finishQuery(qid3);
    queries = qMeta->getPendingQueries(cid1);
    BOOST_CHECK_EQUAL(queries.size(), 1U);
    queries = qMeta->getPendingQueries(cid2);
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    qMeta->completeQuery(qid2, QInfo::COMPLETED);
    qMeta->finishQuery(qid2);
    qMeta->completeQuery(qid4, QInfo::COMPLETED);
    qMeta->finishQuery(qid4);

    // no running queries should be there
    queries = qMeta->getPendingQueries(cid1);
    BOOST_CHECK_EQUAL(queries.size(), 0U);
    queries = qMeta->getPendingQueries(cid2);
    BOOST_CHECK_EQUAL(queries.size(), 0U);
}

BOOST_AUTO_TEST_CASE(messWithTables) {
    // make sure that we have czars from previous test
    CzarId cid1 = qMeta->getCzarID("czar:1000");
    BOOST_CHECK(cid1 != 0U);
    CzarId cid2 = qMeta->getCzarID("czar-2:1000");
    BOOST_CHECK(cid2 != 0U);

    // resister few queries
    QInfo qinfo(QInfo::SYNC, cid1, "user1", "SELECT * from Object", "SELECT * from Object_{}", "", "", "", "",
                0);
    QMeta::TableNames tables(1, std::make_pair("TestDB", "Object"));
    lsst::qserv::QueryId qid1 = qMeta->registerQuery(qinfo, tables);
    lsst::qserv::QueryId qid2 = qMeta->registerQuery(qinfo, tables);
    qinfo = QInfo(QInfo::ASYNC, cid2, "user2", "SELECT * from Object", "SELECT * from Object_{}", "", "", "",
                  "", 7);
    tables.push_back(std::make_pair("TestDB", "Source"));
    lsst::qserv::QueryId qid3 = qMeta->registerQuery(qinfo, tables);
    lsst::qserv::QueryId qid4 = qMeta->registerQuery(qinfo, tables);

    // get queries for tables
    std::vector<lsst::qserv::QueryId> queries = qMeta->getQueriesForTable("TestDB", "Object");
    BOOST_CHECK_EQUAL(queries.size(), 4U);
    queries = qMeta->getQueriesForTable("TestDB", "Source");
    BOOST_CHECK_EQUAL(queries.size(), 2U);

    // update completed status
    qMeta->finishQuery(qid1);
    qMeta->completeQuery(qid1, QInfo::COMPLETED);
    qMeta->finishQuery(qid3);
    qMeta->completeQuery(qid3, QInfo::COMPLETED);
    queries = qMeta->getQueriesForTable("TestDB", "Object");
    BOOST_CHECK_EQUAL(queries.size(), 2U);
    queries = qMeta->getQueriesForTable("TestDB", "Source");
    BOOST_CHECK_EQUAL(queries.size(), 1U);

    qMeta->finishQuery(qid2);
    qMeta->completeQuery(qid2, QInfo::COMPLETED);
    qMeta->finishQuery(qid4);
    qMeta->completeQuery(qid4, QInfo::COMPLETED);

    // no running queries should be there
    queries = qMeta->getQueriesForTable("TestDB", "Object");
    BOOST_CHECK_EQUAL(queries.size(), 0U);
    queries = qMeta->getQueriesForTable("TestDB", "Source");
    BOOST_CHECK_EQUAL(queries.size(), 0U);
}

BOOST_AUTO_TEST_CASE(messWithChunks) {
    // make sure that we have czars from previous test
    CzarId cid1 = qMeta->getCzarID("czar:1000");
    BOOST_CHECK(cid1 != 0U);
    CzarId cid2 = qMeta->getCzarID("czar-2:1000");
    BOOST_CHECK(cid2 != 0U);

    // resister one query
    QInfo qinfo(QInfo::SYNC, cid1, "user1", "SELECT * from Object", "SELECT * from Object_{}", "", "", "", "",
                0);
    QMeta::TableNames tables;
    tables.push_back(std::make_pair("TestDB", "Object"));
    lsst::qserv::QueryId qid1 = qMeta->registerQuery(qinfo, tables);
    BOOST_CHECK(qid1 != 0U);

    // register few chunks and assign them to workers
    std::vector<int> chunks;
    chunks.push_back(10);
    chunks.push_back(20);
    chunks.push_back(37);
    qMeta->addChunks(qid1, chunks);

    // assign chunks to workers
    qMeta->assignChunk(qid1, 10, "worker1");
    qMeta->assignChunk(qid1, 20, "worker2");
    qMeta->assignChunk(qid1, 37, "worker2");
    BOOST_CHECK_THROW(qMeta->assignChunk(qid1, 42, "worker2"), ChunkIdError);
    BOOST_CHECK_THROW(qMeta->assignChunk(99999, 10, "worker2"), ChunkIdError);

    // re-assign chunk
    qMeta->assignChunk(qid1, 37, "worker33");

    // mark chunks as complete
    qMeta->finishChunk(qid1, 10);
    qMeta->finishChunk(qid1, 20);
    qMeta->finishChunk(qid1, 37);
    BOOST_CHECK_THROW(qMeta->finishChunk(qid1, 42), ChunkIdError);
}

BOOST_AUTO_TEST_CASE(messWithQueryStats) {
    LOGS(_log, LOG_LVL_WARN, "messWithQueryStats connect");
    std::shared_ptr<QStatus> qStatus = std::make_shared<QStatusMysql>(testDB.sqlConfig);
    sqlConn = SqlConnectionFactory::make(testDB.sqlConfig);
    CzarId qid1 = 7;  // just need a number.

    int totalChunks = 99;
    LOGS(_log, LOG_LVL_WARN, "messWithQueryStats register");
    qStatus->queryStatsTmpRegister(qid1, totalChunks);

    LOGS(_log, LOG_LVL_WARN, "messWithQueryStats get");
    QStats qStats = qStatus->queryStatsTmpGet(qid1);
    BOOST_CHECK(qStats.totalChunks == totalChunks);
    BOOST_CHECK(qStats.completedChunks == 0);

    int completedChunks = 35;
    LOGS(_log, LOG_LVL_WARN, "messWithQueryStats chunk update");
    qStatus->queryStatsTmpChunkUpdate(qid1, completedChunks);

    qStats = qStatus->queryStatsTmpGet(qid1);
    BOOST_CHECK(qStats.totalChunks == totalChunks);
    BOOST_CHECK(qStats.completedChunks == completedChunks);

    LOGS(_log, LOG_LVL_WARN, "messWithQueryStats remove");
    qStatus->queryStatsTmpRemove(qid1);

    LOGS(_log, LOG_LVL_WARN, "messWithQueryStats check get failure discovery");
    bool caught = false;
    try {
        qStats = qStatus->queryStatsTmpGet(qid1);
    } catch (QueryIdError const&) {
        caught = true;
    }
    BOOST_CHECK(caught);
}

BOOST_AUTO_TEST_CASE(getChunkMap) {
    // The test assumes that the underlying tables exists and it's empty.
    QMetaChunkMap chunkMap;
    BOOST_CHECK_THROW(qMeta->getChunkMap(), EmptyTableError);
}

BOOST_AUTO_TEST_SUITE_END()
