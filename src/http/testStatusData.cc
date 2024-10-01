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
#include "http/WorkerQueryStatusData.h"

// LSST headers
#include "lsst/log/Log.h"

// Boost unit test header
#define BOOST_TEST_MODULE RequestQuery
#include <boost/test/unit_test.hpp>

using namespace std;
namespace test = boost::test_tools;
using namespace lsst::qserv::http;

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

    //&&&auto czarA = lsst::qserv::http::CzarContactInfo::create(czrName, czrId, czrPort, czrHost);
    auto czarA = lsst::qserv::http::CzarContactInfo::create(czrName, czrId, czrPort, czrHost, cxrStartTime);
    LOGS_ERROR("&&& a czarA=" << czarA->dump());

    auto czarAJs = czarA->serializeJson();
    LOGS_ERROR("&&& b czarAJs=" << czarAJs);

    auto czarB = lsst::qserv::http::CzarContactInfo::createFromJson(czarAJs);
    LOGS_ERROR("&&& c czarB=" << czarB);
    BOOST_REQUIRE(czarA->compare(*czarB));

    //&&&auto czarC = lsst::qserv::http::CzarContactInfo::create("different", czrId, czrPort, czrHost);
    auto czarC =
            lsst::qserv::http::CzarContactInfo::create("different", czrId, czrPort, czrHost, cxrStartTime);
    BOOST_REQUIRE(!czarA->compare(*czarC));

    auto start = lsst::qserv::CLOCK::now();
    auto workerA = WorkerContactInfo::create("sd_workerA", "host_w1", "mgmhost_a", 3421, start);

    auto workerB = WorkerContactInfo::create("sd_workerB", "host_w2", "mgmhost_a", 3421, start);
    auto workerC = WorkerContactInfo::create("sd_workerC", "host_w3", "mgmhost_b", 3422, start);

    LOGS_ERROR("&&& d workerA=" << workerA->dump());

    auto jsWorkerA = workerA->serializeJson();
    LOGS_ERROR("&&& e jsWorkerA=" << jsWorkerA);
    auto start1Sec = start + 1s;
    auto workerA1 = WorkerContactInfo::createFromJsonWorker(jsWorkerA, start1Sec);
    LOGS_ERROR("&&& f workerA1=" << workerA1->dump());
    BOOST_REQUIRE(workerA->isSameContactInfo(*workerA1));

    // WorkerQueryStatusData
    auto wqsdA = lsst::qserv::http::WorkerQueryStatusData::create(workerA, czarA, replicationInstanceId,
                                                                  replicationAuthKey);
    LOGS_ERROR("&&& g wqsdA=" << wqsdA->dump());

    //&&&double timeoutAliveSecs = 100.0;
    //&&&double timeoutDeadSecs = 2*timeoutAliveSecs;
    double maxLifetime = 300.0;
    auto jsDataA = wqsdA->serializeJson(maxLifetime);
    LOGS_ERROR("&&& h jsDataA=" << *jsDataA);

    // Check that empty lists work.
    auto wqsdA1 = lsst::qserv::http::WorkerQueryStatusData::createFromJson(*jsDataA, replicationInstanceId,
                                                                           replicationAuthKey, start1Sec);
    LOGS_ERROR("&&& i wqsdA1=" << wqsdA1->dump());
    LOGS_ERROR("&&& i wqsdA=" << wqsdA->dump());
    auto jsDataA1 = wqsdA1->serializeJson(maxLifetime);
    LOGS_ERROR("&&& i jsDataA1=" << *jsDataA1);
    LOGS_ERROR("&&& i jsDataA=" << *jsDataA);
    BOOST_REQUIRE(*jsDataA == *jsDataA1);

    vector<lsst::qserv::QueryId> qIdsDelFiles = {7, 8, 9, 15, 25, 26, 27, 30};
    vector<lsst::qserv::QueryId> qIdsKeepFiles = {1, 2, 3, 4, 6, 10, 13, 19, 33};
    for (auto const qIdDF : qIdsDelFiles) {
        wqsdA->qIdDoneDeleteFiles[qIdDF] = start;
    }

    jsDataA = wqsdA->serializeJson(maxLifetime);
    LOGS_ERROR("&&& j jsDataA=" << jsDataA);
    BOOST_REQUIRE(*jsDataA != *jsDataA1);

    for (auto const qIdKF : qIdsKeepFiles) {
        wqsdA->qIdDoneKeepFiles[qIdKF] = start;
    }

    wqsdA->addDeadUberJobs(12, {1, 3}, start);

    LOGS_ERROR("&&& i wqsdA=" << wqsdA->dump());

    jsDataA = wqsdA->serializeJson(maxLifetime);
    LOGS_ERROR("&&& j jsDataA=" << *jsDataA);

    auto start5Sec = start + 5s;
    auto workerAFromJson = lsst::qserv::http::WorkerQueryStatusData::createFromJson(
            *jsDataA, replicationInstanceId, replicationAuthKey, start5Sec);
    auto jsWorkerAFromJson = workerAFromJson->serializeJson(maxLifetime);
    BOOST_REQUIRE(*jsDataA == *jsWorkerAFromJson);

    wqsdA->addDeadUberJobs(12, {34}, start5Sec);
    wqsdA->addDeadUberJobs(91, {77}, start5Sec);
    wqsdA->addDeadUberJobs(1059, {1, 4, 6, 7, 8, 10, 3, 22, 93}, start5Sec);

    jsDataA = wqsdA->serializeJson(maxLifetime);
    LOGS_ERROR("&&& k jsDataA=" << *jsDataA);
    BOOST_REQUIRE(*jsDataA != *jsWorkerAFromJson);

    workerAFromJson = lsst::qserv::http::WorkerQueryStatusData::createFromJson(
            *jsDataA, replicationInstanceId, replicationAuthKey, start5Sec);
    jsWorkerAFromJson = workerAFromJson->serializeJson(maxLifetime);
    LOGS_ERROR("&&& l jsWorkerAFromJson=" << *jsWorkerAFromJson);
    BOOST_REQUIRE(*jsDataA == *jsWorkerAFromJson);

    // Make the response, which contains lists of the items handled by the workers.
    auto jsWorkerResp = workerAFromJson->serializeResponseJson(wkrStartTime);

    // test removal of elements after response.
    BOOST_REQUIRE(!wqsdA->qIdDoneDeleteFiles.empty());
    BOOST_REQUIRE(!wqsdA->qIdDoneKeepFiles.empty());
    BOOST_REQUIRE(!wqsdA->qIdDeadUberJobs.empty());

    wqsdA->handleResponseJson(jsWorkerResp);
    auto [respSuccess, workerRestarted] = wqsdA->handleResponseJson(jsWorkerResp);
    BOOST_REQUIRE(respSuccess == true);
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

    auto czarA = lsst::qserv::http::CzarContactInfo::create(czrName, czrId, czrPort, czrHost, cxrStartTime);
    LOGS_ERROR("&&&i a czarA=" << czarA->dump());
    auto czarAJs = czarA->serializeJson();
    LOGS_ERROR("&&&i b czarAJs=" << czarAJs);

    auto start = lsst::qserv::CLOCK::now();
    auto workerA = WorkerContactInfo::create("sd_workerA", "host_w1", "mgmhost_a", 3421, start);
    LOGS_ERROR("&&&i d workerA=" << workerA->dump());
    auto jsWorkerA = workerA->serializeJson();
    LOGS_ERROR("&&&i e jsWorkerA=" << jsWorkerA);

    // WorkerCzarComIssue
    //&&&auto wccIssueA = lsst::qserv::http::WorkerCzarComIssue::create(workerA, czarA, replicationInstanceId,
    //replicationAuthKey);
    auto wccIssueA = lsst::qserv::http::WorkerCzarComIssue::create(replicationInstanceId, replicationAuthKey);
    wccIssueA->setContactInfo(workerA, czarA);
    BOOST_REQUIRE(wccIssueA->needToSend() == false);
    wccIssueA->setThoughtCzarWasDead(true);
    BOOST_REQUIRE(wccIssueA->needToSend() == true);

    LOGS_ERROR("&&&i f wccIssue=" << wccIssueA->dump());

    auto jsIssueA = wccIssueA->serializeJson();
    LOGS_ERROR("&&&i g jsIssue=" << *jsIssueA);

    auto wccIssueA1 = lsst::qserv::http::WorkerCzarComIssue::createFromJson(*jsIssueA, replicationInstanceId,
                                                                            replicationAuthKey);
    LOGS_ERROR("&&&i i wccIssueA1=" << wccIssueA1->dump());
    LOGS_ERROR("&&&i i wccIssueA=" << wccIssueA->dump());
    auto jsIssueA1 = wccIssueA1->serializeJson();
    LOGS_ERROR("&&&i i jsIssueA1=" << *jsIssueA1);
    LOGS_ERROR("&&&i i jsIssueA=" << *jsIssueA);
    BOOST_REQUIRE(*jsIssueA == *jsIssueA1);

    // &&& Test with items in lists.
}

BOOST_AUTO_TEST_SUITE_END()
