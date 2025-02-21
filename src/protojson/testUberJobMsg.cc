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

#include "nlohmann/json.hpp"

// Qserv headers
#include "global/clock_defs.h"
#include "lsst/log/Log.h"
#include "protojson/UberJobMsg.h"

// Boost unit test header
#define BOOST_TEST_MODULE RequestQuery
#include <boost/test/unit_test.hpp>

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.protojson.testUberJobMsg");
}

using namespace std;
namespace test = boost::test_tools;
using namespace lsst::qserv::protojson;

BOOST_AUTO_TEST_SUITE(Suite)

string testA() {
    string ta =
            R"({"maxtablesizemb":5432,"scaninteractive":true,"auth_key":"replauthkey","czarinfo":{"czar-startup-time":1732658208085,"id":1,"management-host-name":"3a8b68cf9b67","management-port":40865,"name":"proxy"},"dbtables_map":{"dbtable_map":[],"scanrating_map":[]},"scaninfo":{"infoscanrating":0,"infotables":[]},"instance_id":"qserv_proj","jobs":[{"attemptCount":0,"chunkId":1234567890,"chunkresultname":"r_1_a0d45001254932466b784acf90323565_1234567890_0","chunkscantables_indexes":[],"jobId":0,"queryFragments":[{"dbtables_indexes":[],"resulttblname":"r_1_a0d45001254932466b784acf90323565_1234567890_0","subchunkids":[],"subquerytemplate_indexes":[0]}],"querySpecDb":"qcase01","scanInteractive":true,"scanPriority":0}],"queryid":1,"rowlimit":0,"subqueries_map":{"subquerytemplate_map":[{"index":0,"template":"SELECT `qcase01.Filter`.`filterId` AS `filterId`,`qcase01.Filter`.`filterName` AS `filterName`,`qcase01.Filter`.`photClam` AS `photClam`,`qcase01.Filter`.`photBW` AS `photBW` FROM `qcase01`.`Filter`AS`qcase01.Filter` WHERE (`qcase01.Filter`.`filterId`<<1)=2"}]},"uberjobid":2,"version":39,"worker":"6c56ba9b-ac40-11ef-acb7-0242c0a8030a"})";
    return ta;
}

string testB() {
    string tb =
            R"({"auth_key":"slac6dev:kukara4a","czarinfo":{"czar-startup-time":1733499789161,"id":7,"management-host-name":"sdfqserv001.sdf.slac.stanford.edu","management-port":41923,"name":"proxy"},"dbtables_map":{"dbtable_map":[{"db":"dp02_dc2_catalogs","index":0,"table":"Object"}],"scanrating_map":[{"index":0,"lockinmem":true,"scanrating":1}]},"instance_id":"slac6dev","jobs":[{"attemptCount":0,"chunkId":79680,"chunkresultname":"r_280607_e6eac6bb53b0f8505ed36bf82a4d93f1_79680_0","chunkscantables_indexes":[0],"jobId":1398,"queryFragments":[{"dbtables_indexes":[],"resulttblname":"r_280607_e6eac6bb53b0f8505ed36bf82a4d93f1_79680_0","subchunkids":[],"subquerytemplate_indexes":[0]}],"querySpecDb":"dp02_dc2_catalogs","scanInteractive":false,"scanPriority":1},{"attemptCount":0,"chunkId":80358,"chunkresultname":"r_280607_e6eac6bb53b0f8505ed36bf82a4d93f1_80358_0","chunkscantables_indexes":[0],"jobId":1435,"queryFragments":[{"dbtables_indexes":[],"resulttblname":"r_280607_e6eac6bb53b0f8505ed36bf82a4d93f1_80358_0","subchunkids":[],"subquerytemplate_indexes":[1]}],"querySpecDb":"dp02_dc2_catalogs","scanInteractive":false,"scanPriority":1},{"attemptCount":0,"chunkId":81017,"chunkresultname":"r_280607_e6eac6bb53b0f8505ed36bf82a4d93f1_81017_0","chunkscantables_indexes":[0],"jobId":1452,"queryFragments":[{"dbtables_indexes":[],"resulttblname":"r_280607_e6eac6bb53b0f8505ed36bf82a4d93f1_81017_0","subchunkids":[],"subquerytemplate_indexes":[2]}],"querySpecDb":"dp02_dc2_catalogs","scanInteractive":false,"scanPriority":1}],"maxtablesizemb":5100,"scaninteractive":false,"queryid":280607,"rowlimit":0,"scaninfo":{"infoscanrating":1,"infotables":[{"sidb":"dp02_dc2_catalogs","silockinmem":true,"sirating":1,"sitable":"Object"}]},"subqueries_map":{"subquerytemplate_map":[{"index":0,"template":"SELECT COUNT(`obj`.`g_ap12Flux`) AS `QS1_COUNT`,SUM(`obj`.`g_ap12Flux`) AS `QS2_SUM`,MIN(`obj`.`g_ap12Flux`) AS `QS3_MIN`,MAX(`obj`.`g_ap12Flux`) AS `QS4_MAX`,COUNT(`obj`.`g_ap12FluxErr`) AS `QS5_COUNT`,SUM(`obj`.`g_ap12FluxErr`) AS `QS6_SUM`,MIN(`obj`.`g_ap12FluxErr`) AS `QS7_MIN`,MAX(`obj`.`g_ap12FluxErr`) AS `QS8_MAX`,COUNT(`obj`.`g_ap25Flux`) AS `QS9_COUNT`,SUM(`obj`.`g_ap25Flux`) AS `QS10_SUM`,MIN(`obj`.`g_ap25Flux`) AS `QS11_MIN`,MAX(`obj`.`g_ap25Flux`) AS `QS12_MAX`,COUNT(`obj`.`g_ap25FluxErr`) AS `QS13_COUNT`,SUM(`obj`.`g_ap25FluxErr`) AS `QS14_SUM`,MIN(`obj`.`g_ap25FluxErr`) AS `QS15_MIN`,MAX(`obj`.`g_ap25FluxErr`) AS `QS16_MAX` FROM `dp02_dc2_catalogs`.`Object_79680` AS `obj`"},{"index":1,"template":"SELECT COUNT(`obj`.`g_ap12Flux`) AS `QS1_COUNT`,SUM(`obj`.`g_ap12Flux`) AS `QS2_SUM`,MIN(`obj`.`g_ap12Flux`) AS `QS3_MIN`,MAX(`obj`.`g_ap12Flux`) AS `QS4_MAX`,COUNT(`obj`.`g_ap12FluxErr`) AS `QS5_COUNT`,SUM(`obj`.`g_ap12FluxErr`) AS `QS6_SUM`,MIN(`obj`.`g_ap12FluxErr`) AS `QS7_MIN`,MAX(`obj`.`g_ap12FluxErr`) AS `QS8_MAX`,COUNT(`obj`.`g_ap25Flux`) AS `QS9_COUNT`,SUM(`obj`.`g_ap25Flux`) AS `QS10_SUM`,MIN(`obj`.`g_ap25Flux`) AS `QS11_MIN`,MAX(`obj`.`g_ap25Flux`) AS `QS12_MAX`,COUNT(`obj`.`g_ap25FluxErr`) AS `QS13_COUNT`,SUM(`obj`.`g_ap25FluxErr`) AS `QS14_SUM`,MIN(`obj`.`g_ap25FluxErr`) AS `QS15_MIN`,MAX(`obj`.`g_ap25FluxErr`) AS `QS16_MAX` FROM `dp02_dc2_catalogs`.`Object_80358` AS `obj`"},{"index":2,"template":"SELECT COUNT(`obj`.`g_ap12Flux`) AS `QS1_COUNT`,SUM(`obj`.`g_ap12Flux`) AS `QS2_SUM`,MIN(`obj`.`g_ap12Flux`) AS `QS3_MIN`,MAX(`obj`.`g_ap12Flux`) AS `QS4_MAX`,COUNT(`obj`.`g_ap12FluxErr`) AS `QS5_COUNT`,SUM(`obj`.`g_ap12FluxErr`) AS `QS6_SUM`,MIN(`obj`.`g_ap12FluxErr`) AS `QS7_MIN`,MAX(`obj`.`g_ap12FluxErr`) AS `QS8_MAX`,COUNT(`obj`.`g_ap25Flux`) AS `QS9_COUNT`,SUM(`obj`.`g_ap25Flux`) AS `QS10_SUM`,MIN(`obj`.`g_ap25Flux`) AS `QS11_MIN`,MAX(`obj`.`g_ap25Flux`) AS `QS12_MAX`,COUNT(`obj`.`g_ap25FluxErr`) AS `QS13_COUNT`,SUM(`obj`.`g_ap25FluxErr`) AS `QS14_SUM`,MIN(`obj`.`g_ap25FluxErr`) AS `QS15_MIN`,MAX(`obj`.`g_ap25FluxErr`) AS `QS16_MAX` FROM `dp02_dc2_catalogs`.`Object_81017` AS `obj`"}]},"uberjobid":147,"version":39,"worker":"db04"})";
    return tb;
}

bool parseSerializeReparseCheck(string const& jsStr, string const& note) {
    string fName("parseSerialize ");
    fName += note + " ";
    LOGS(_log, LOG_LVL_INFO, fName << " start " << jsStr);
    nlohmann::json js = nlohmann::json::parse(jsStr);
    LOGS(_log, LOG_LVL_INFO, fName << " parse 1");

    UberJobMsg::Ptr ujm = UberJobMsg::createFromJson(js);
    BOOST_REQUIRE(ujm != nullptr);

    nlohmann::json jsUjm = ujm->serializeJson();
    LOGS(_log, LOG_LVL_INFO, fName << " serialized jsUjm=" << jsUjm);

    UberJobMsg::Ptr ujmCreated = UberJobMsg::createFromJson(jsUjm);
    LOGS(_log, LOG_LVL_INFO, fName << " created");
    nlohmann::json jsUjmCreated = ujmCreated->serializeJson();
    LOGS(_log, LOG_LVL_INFO, fName << " created->serialized");

    bool createdMatchesOriginal = jsUjm == jsUjmCreated;
    if (createdMatchesOriginal) {
        LOGS(_log, LOG_LVL_INFO, fName << "created matches original");
    } else {
        LOGS(_log, LOG_LVL_ERROR, "jsUjm != jsUjmCreated");
        LOGS(_log, LOG_LVL_ERROR, "jsUjm=" << jsUjm);
        LOGS(_log, LOG_LVL_ERROR, "jsUjmCreated=" << jsUjmCreated);
    }
    BOOST_REQUIRE(createdMatchesOriginal);
    return createdMatchesOriginal;
}

BOOST_AUTO_TEST_CASE(WorkerQueryStatusData) {
    string const replicationInstanceId = "repliInstId";
    string const replicationAuthKey = "repliIAuthKey";

    LOGS(_log, LOG_LVL_INFO, "testUJM start");
    string jsStr = testA();
    nlohmann::json js = nlohmann::json::parse(jsStr);
    UberJobMsg::Ptr ujm = UberJobMsg::createFromJson(js);
    BOOST_REQUIRE(ujm != nullptr);

    nlohmann::json jsUjm = ujm->serializeJson();

    LOGS(_log, LOG_LVL_INFO, "js=" << js);
    LOGS(_log, LOG_LVL_INFO, "jsUjm=" << jsUjm);

    UberJobMsg::Ptr ujmCreated = UberJobMsg::createFromJson(jsUjm);
    LOGS(_log, LOG_LVL_INFO, "ujmCreated=" << ujmCreated);
    nlohmann::json jsUjmCreated = ujmCreated->serializeJson();

    bool createdMatchesOriginal = jsUjm == jsUjmCreated;
    if (!createdMatchesOriginal) {
        LOGS(_log, LOG_LVL_ERROR, "jsUjm != jsUjmCreated");
        LOGS(_log, LOG_LVL_ERROR, "jsUjm=" << jsUjm);
        LOGS(_log, LOG_LVL_ERROR, "jsUjmCreated=" << jsUjmCreated);
    }
    BOOST_REQUIRE(createdMatchesOriginal);

    BOOST_REQUIRE(parseSerializeReparseCheck(testA(), "A"));
    BOOST_REQUIRE(parseSerializeReparseCheck(testB(), "B"));
}

BOOST_AUTO_TEST_SUITE_END()
