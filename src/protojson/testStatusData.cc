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

// System headers
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

// Qserv headers
#include "global/clock_defs.h"
#include "lsst/log/Log.h"
#include "protojson/WorkerQueryStatusData.h"

// Boost unit test header
#define BOOST_TEST_MODULE RequestQuery
#include <boost/test/unit_test.hpp>

using namespace std;
namespace test = boost::test_tools;
using namespace lsst::qserv::protojson;

BOOST_AUTO_TEST_SUITE(Suite)

BOOST_AUTO_TEST_CASE(WorkerQueryStatusData) {
    string const replicationInstanceId = "repliInstId";
    string const replicationAuthKey = "repliIAuthKey";

    uint64_t cxrStartTime = lsst::qserv::millisecSinceEpoch(lsst::qserv::CLOCK::now() - 5s);
    uint64_t wkrStartTime = lsst::qserv::millisecSinceEpoch(lsst::qserv::CLOCK::now() - 10s);

    string const czrName("czar_name");
    lsst::qserv::CzarIdType const czrId = 32;
    int czrPort = 2022;
    string const czrHost("cz_host");

    auto czarA =
            lsst::qserv::protojson::CzarContactInfo::create(czrName, czrId, czrPort, czrHost, cxrStartTime);

    auto czarAJs = czarA->serializeJson();

    auto czarB = lsst::qserv::protojson::CzarContactInfo::createFromJson(czarAJs);
    BOOST_REQUIRE(czarA->compare(*czarB));

    auto czarC = lsst::qserv::protojson::CzarContactInfo::create("different", czrId, czrPort, czrHost,
                                                                 cxrStartTime);
    BOOST_REQUIRE(!czarA->compare(*czarC));

    auto start = lsst::qserv::CLOCK::now();
    auto workerA = WorkerContactInfo::create("sd_workerA", "host_w1", "mgmhost_a", 3421, start);

    auto workerB = WorkerContactInfo::create("sd_workerB", "host_w2", "mgmhost_a", 3421, start);
    auto workerC = WorkerContactInfo::create("sd_workerC", "host_w3", "mgmhost_b", 3422, start);

    auto jsWorkerA = workerA->serializeJson();
    auto start1Sec = start + 1s;
    auto workerA1 = WorkerContactInfo::createFromJsonWorker(jsWorkerA, start1Sec);
    BOOST_REQUIRE(workerA->isSameContactInfo(*workerA1));

    // WorkerQueryStatusData
    auto wqsdA = lsst::qserv::protojson::WorkerQueryStatusData::create(workerA, czarA, replicationInstanceId,
                                                                       replicationAuthKey);

    double maxLifetime = 300.0;
    auto jsDataA = wqsdA->serializeJson(maxLifetime);

    // Check that empty lists work.
    auto wqsdA1 = lsst::qserv::protojson::WorkerQueryStatusData::createFromJson(
            *jsDataA, replicationInstanceId, replicationAuthKey, start1Sec);
    auto jsDataA1 = wqsdA1->serializeJson(maxLifetime);
    BOOST_REQUIRE(*jsDataA == *jsDataA1);

    vector<lsst::qserv::QueryId> qIdsDelFiles = {7, 8, 9, 15, 25, 26, 27, 30};
    vector<lsst::qserv::QueryId> qIdsKeepFiles = {1, 2, 3, 4, 6, 10, 13, 19, 33};
    for (auto const qIdDF : qIdsDelFiles) {
        wqsdA->qIdDoneDeleteFiles[qIdDF] = start;
    }

    jsDataA = wqsdA->serializeJson(maxLifetime);
    BOOST_REQUIRE(*jsDataA != *jsDataA1);

    for (auto const qIdKF : qIdsKeepFiles) {
        wqsdA->qIdDoneKeepFiles[qIdKF] = start;
    }

    wqsdA->addDeadUberJobs(12, {1, 3}, start);

    jsDataA = wqsdA->serializeJson(maxLifetime);

    auto start5Sec = start + 5s;
    auto workerAFromJson = lsst::qserv::protojson::WorkerQueryStatusData::createFromJson(
            *jsDataA, replicationInstanceId, replicationAuthKey, start5Sec);
    auto jsWorkerAFromJson = workerAFromJson->serializeJson(maxLifetime);
    BOOST_REQUIRE(*jsDataA == *jsWorkerAFromJson);

    wqsdA->addDeadUberJobs(12, {34}, start5Sec);
    wqsdA->addDeadUberJobs(91, {77}, start5Sec);
    wqsdA->addDeadUberJobs(1059, {1, 4, 6, 7, 8, 10, 3, 22, 93}, start5Sec);

    jsDataA = wqsdA->serializeJson(maxLifetime);
    BOOST_REQUIRE(*jsDataA != *jsWorkerAFromJson);

    workerAFromJson = lsst::qserv::protojson::WorkerQueryStatusData::createFromJson(
            *jsDataA, replicationInstanceId, replicationAuthKey, start5Sec);
    jsWorkerAFromJson = workerAFromJson->serializeJson(maxLifetime);
    BOOST_REQUIRE(*jsDataA == *jsWorkerAFromJson);

    // Make the response, which contains lists of the items handled by the workers.
    auto jsWorkerResp = workerAFromJson->serializeResponseJson(wkrStartTime);

    // test removal of elements after response.
    BOOST_REQUIRE(!wqsdA->qIdDoneDeleteFiles.empty());
    BOOST_REQUIRE(!wqsdA->qIdDoneKeepFiles.empty());
    BOOST_REQUIRE(!wqsdA->qIdDeadUberJobs.empty());

    wqsdA->handleResponseJson(jsWorkerResp);
    auto workerRestarted = wqsdA->handleResponseJson(jsWorkerResp);
    BOOST_REQUIRE(workerRestarted == false);

    BOOST_REQUIRE(wqsdA->qIdDoneDeleteFiles.empty());
    BOOST_REQUIRE(wqsdA->qIdDoneKeepFiles.empty());
    BOOST_REQUIRE(wqsdA->qIdDeadUberJobs.empty());
}

BOOST_AUTO_TEST_CASE(WorkerCzarComIssue) {
    string const replicationInstanceId = "repliInstId";
    string const replicationAuthKey = "repliIAuthKey";

    uint64_t cxrStartTime = lsst::qserv::millisecSinceEpoch(lsst::qserv::CLOCK::now() - 5s);

    string const czrName("czar_name");
    lsst::qserv::CzarIdType const czrId = 32;
    int czrPort = 2022;
    string const czrHost("cz_host");

    auto czarA =
            lsst::qserv::protojson::CzarContactInfo::create(czrName, czrId, czrPort, czrHost, cxrStartTime);
    auto czarAJs = czarA->serializeJson();

    auto start = lsst::qserv::CLOCK::now();
    auto workerA = WorkerContactInfo::create("sd_workerA", "host_w1", "mgmhost_a", 3421, start);
    auto jsWorkerA = workerA->serializeJson();

    // WorkerCzarComIssue
    auto wccIssueA =
            lsst::qserv::protojson::WorkerCzarComIssue::create(replicationInstanceId, replicationAuthKey);
    wccIssueA->setContactInfo(workerA, czarA);
    BOOST_REQUIRE(wccIssueA->needToSend() == false);
    wccIssueA->setThoughtCzarWasDead(true);
    BOOST_REQUIRE(wccIssueA->needToSend() == true);

    auto jsIssueA = wccIssueA->serializeJson();

    auto wccIssueA1 = lsst::qserv::protojson::WorkerCzarComIssue::createFromJson(
            *jsIssueA, replicationInstanceId, replicationAuthKey);
    auto jsIssueA1 = wccIssueA1->serializeJson();
    BOOST_REQUIRE(*jsIssueA == *jsIssueA1);

    // TODO:UJ Test with items in lists.
}

BOOST_AUTO_TEST_SUITE_END()
