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
/**
 * @brief test SemanticMap
 */

// System headers
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Third party headers
#include "boost/asio.hpp"
#include "nlohmann/json.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/constants.h"
#include "replica/config/ConfigTestData.h"
#include "replica/config/Configuration.h"
#include "replica/util/Common.h"
#include "replica/util/ProtocolBuffer.h"

// Boost unit test header
#define BOOST_TEST_MODULE Configuration
#include <boost/test/unit_test.hpp>

using namespace std;
namespace test = boost::test_tools;
using json = nlohmann::json;
using namespace lsst::qserv::replica;

namespace {
/// The configuration is shared by all tests.
Configuration::Ptr config;
}  // namespace

BOOST_AUTO_TEST_SUITE(Suite)

BOOST_AUTO_TEST_CASE(ConfigurationTestStaticParameters) {
    LOGS_INFO("Testing static parameters");

    BOOST_CHECK_THROW(Configuration::setQservCzarDbUrl(""), std::invalid_argument);
    BOOST_CHECK_THROW(Configuration::setQservWorkerDbUrl(""), std::invalid_argument);

    BOOST_REQUIRE_NO_THROW(Configuration::setDatabaseAllowReconnect(true));
    BOOST_CHECK(Configuration::databaseAllowReconnect() == true);
    BOOST_REQUIRE_NO_THROW(Configuration::setDatabaseAllowReconnect(false));
    BOOST_CHECK(Configuration::databaseAllowReconnect() == false);

    BOOST_CHECK_THROW(Configuration::setDatabaseConnectTimeoutSec(0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(Configuration::setDatabaseConnectTimeoutSec(1));
    BOOST_CHECK(Configuration::databaseConnectTimeoutSec() == 1);

    BOOST_CHECK_THROW(Configuration::setDatabaseMaxReconnects(0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(Configuration::setDatabaseMaxReconnects(2));
    BOOST_CHECK(Configuration::databaseMaxReconnects() == 2);

    BOOST_CHECK_THROW(Configuration::setDatabaseTransactionTimeoutSec(0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(Configuration::setDatabaseTransactionTimeoutSec(3));
    BOOST_CHECK(Configuration::databaseTransactionTimeoutSec() == 3);

    BOOST_REQUIRE_NO_THROW(Configuration::setSchemaUpgradeWait(true));
    BOOST_CHECK(Configuration::schemaUpgradeWait() == true);
    BOOST_REQUIRE_NO_THROW(Configuration::setSchemaUpgradeWait(false));
    BOOST_CHECK(Configuration::schemaUpgradeWait() == false);

    BOOST_CHECK_THROW(Configuration::setSchemaUpgradeWaitTimeoutSec(0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(Configuration::setSchemaUpgradeWaitTimeoutSec(4));
    BOOST_CHECK(Configuration::schemaUpgradeWaitTimeoutSec() == 4);
}

BOOST_AUTO_TEST_CASE(ConfigurationInitTestJSON) {
    LOGS_INFO("Testing JSON initialization");
    BOOST_REQUIRE_NO_THROW(config = Configuration::load(ConfigTestData::data()));
    BOOST_CHECK(config != nullptr);
    BOOST_CHECK(config->configUrl().empty());
    string const configJsonStr = config->toJson().dump();
    BOOST_CHECK(!configJsonStr.empty());
}

BOOST_AUTO_TEST_CASE(ConfigurationTestDir) {
    LOGS_INFO("Testing directory functions");
    BOOST_CHECK(config->parameters() == ConfigTestData::parameters());
}

BOOST_AUTO_TEST_CASE(ConfigurationTestReadingGeneralParameters) {
    LOGS_INFO("Testing reading general parameters");

    // Fetching values of general parameters.
    BOOST_CHECK(config->get<size_t>("common", "request-buf-size-bytes") == 8192);
    BOOST_CHECK(config->get<unsigned int>("common", "request-retry-interval-sec") == 1);

    BOOST_CHECK(config->get<string>("registry", "host") == "127.0.0.1");
    BOOST_CHECK(config->get<uint16_t>("registry", "port") == 8081);
    BOOST_CHECK(config->get<unsigned int>("registry", "max-listen-conn") == 512);
    BOOST_CHECK(config->get<size_t>("registry", "threads") == 4);
    BOOST_CHECK(config->get<unsigned int>("registry", "heartbeat-ival-sec") == 10);

    BOOST_CHECK(config->get<size_t>("controller", "num-threads") == 2);
    BOOST_CHECK(config->get<uint16_t>("controller", "http-server-port") == 8080);
    BOOST_CHECK(config->get<unsigned int>("controller", "http-max-listen-conn") == 256);
    BOOST_CHECK(config->get<size_t>("controller", "http-server-threads") == 3);
    BOOST_CHECK(config->get<unsigned int>("controller", "request-timeout-sec") == 100);
    BOOST_CHECK(config->get<unsigned int>("controller", "job-timeout-sec") == 200);
    BOOST_CHECK(config->get<unsigned int>("controller", "job-heartbeat-sec") == 300);
    BOOST_CHECK(config->get<unsigned int>("controller", "max-repl-level") == 2);
    BOOST_CHECK(config->get<int>("controller", "worker-evict-priority-level") == 1);
    BOOST_CHECK(config->get<int>("controller", "health-monitor-priority-level") == 2);
    BOOST_CHECK(config->get<int>("controller", "ingest-priority-level") == 3);
    BOOST_CHECK(config->get<int>("controller", "catalog-management-priority-level") == 4);
    BOOST_CHECK(config->get<unsigned int>("controller", "auto-register-workers") == 1);
    BOOST_CHECK(config->get<unsigned int>("controller", "auto-register-czars") == 0);
    BOOST_CHECK(config->get<unsigned int>("controller", "ingest-job-monitor-ival-sec") == 5);
    BOOST_CHECK(config->get<unsigned int>("controller", "num-director-index-connections") == 6);
    BOOST_CHECK(config->get<string>("controller", "director-index-engine") == "MyISAM");

    BOOST_CHECK(config->get<unsigned int>("xrootd", "auto-notify") == 0);
    BOOST_CHECK(config->get<string>("xrootd", "host") == "localhost");
    BOOST_CHECK(config->get<uint16_t>("xrootd", "port") == 1104);
    BOOST_CHECK(config->get<unsigned int>("xrootd", "request-timeout-sec") == 400);
    BOOST_CHECK(config->get<unsigned int>("xrootd", "allow-reconnect") == 0);
    BOOST_CHECK(config->get<unsigned int>("xrootd", "reconnect-timeout") == 500);

    BOOST_CHECK(config->get<string>("database", "host") == "localhost");
    BOOST_CHECK(config->get<uint16_t>("database", "port") == 13306);
    BOOST_CHECK(config->get<string>("database", "user") == "qsreplica");
    BOOST_CHECK(config->get<string>("database", "password") == "changeme");
    BOOST_CHECK(config->get<string>("database", "name") == "qservReplica");

    BOOST_CHECK(config->get<string>("database", "qserv-master-user") == "qsmaster");
    BOOST_CHECK(config->qservCzarDbUrl() == "mysql://qsmaster@localhost:3306/qservMeta");
    BOOST_CHECK(config->qservWorkerDbUrl() == "mysql://qsmaster@localhost:3306/qservw_worker");

    BOOST_CHECK(config->get<size_t>("database", "services-pool-size") == 2);

    BOOST_CHECK(config->get<size_t>("worker", "num-threads") == 3);
    BOOST_CHECK(config->get<size_t>("worker", "num-svc-processing-threads") == 4);
    BOOST_CHECK(config->get<size_t>("worker", "num-http-svc-threads") == 10);
    BOOST_CHECK(config->get<size_t>("worker", "num-fs-processing-threads") == 5);
    BOOST_CHECK(config->get<size_t>("worker", "fs-buf-size-bytes") == 1024);
    BOOST_CHECK(config->get<size_t>("worker", "num-loader-processing-threads") == 6);
    BOOST_CHECK(config->get<size_t>("worker", "num-exporter-processing-threads") == 7);
    BOOST_CHECK(config->get<size_t>("worker", "num-http-loader-processing-threads") == 8);
    BOOST_CHECK(config->get<size_t>("worker", "num-async-loader-processing-threads") == 9);
    BOOST_CHECK(config->get<size_t>("worker", "async-loader-auto-resume") == 0);
    BOOST_CHECK(config->get<size_t>("worker", "async-loader-cleanup-on-resume") == 0);
    BOOST_CHECK(config->get<unsigned int>("worker", "http-max-listen-conn") == 512);
    BOOST_CHECK(config->get<size_t>("worker", "http-max-queued-requests") == 1024);
    BOOST_CHECK(config->get<size_t>("worker", "http-svc-max-queued-requests") == 2048);
    BOOST_CHECK(config->get<unsigned int>("worker", "loader-max-warnings") == 2);
    BOOST_CHECK(config->get<string>("worker", "ingest-charset-name") == "latin1");
    BOOST_CHECK(config->get<unsigned int>("worker", "ingest-num-retries") == 1);
    BOOST_CHECK(config->get<unsigned int>("worker", "ingest-max-retries") == 10);
    BOOST_CHECK(config->get<size_t>("worker", "director-index-record-size") == 16 * 1024 * 1024);
}

BOOST_AUTO_TEST_CASE(ConfigurationTestModifyingGeneralParameters) {
    LOGS_INFO("Testing modifying general parameters");

    BOOST_CHECK_THROW(config->set<size_t>("common", "request-buf-size-bytes", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("common", "request-buf-size-bytes", 8193));
    BOOST_CHECK(config->get<size_t>("common", "request-buf-size-bytes") == 8193);

    BOOST_CHECK_THROW(config->set<unsigned int>("common", "request-retry-interval-sec", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("common", "request-retry-interval-sec", 2));
    BOOST_CHECK(config->get<unsigned int>("common", "request-retry-interval-sec") == 2);

    BOOST_CHECK_THROW(config->set<string>("registry", "host", string()), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<string>("registry", "host", "localhost"));
    BOOST_CHECK(config->get<string>("registry", "host") == "localhost");

    BOOST_CHECK_THROW(config->set<uint16_t>("registry", "port", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<uint16_t>("registry", "port", 8083));
    BOOST_CHECK(config->get<uint16_t>("registry", "port") == 8083);

    BOOST_CHECK_THROW(config->set<unsigned int>("registry", "max-listen-conn", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("registry", "max-listen-conn", 1024));
    BOOST_CHECK(config->get<unsigned int>("registry", "max-listen-conn") == 1024);

    BOOST_CHECK_THROW(config->set<size_t>("registry", "threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("registry", "threads", 5));
    BOOST_CHECK(config->get<size_t>("registry", "threads") == 5);

    BOOST_CHECK_THROW(config->set<unsigned int>("registry", "heartbeat-ival-sec", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("registry", "heartbeat-ival-sec", 11));
    BOOST_CHECK(config->get<unsigned int>("registry", "heartbeat-ival-sec") == 11);

    BOOST_CHECK_THROW(config->set<size_t>("controller", "num-threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("controller", "num-threads", 3));
    BOOST_CHECK(config->get<size_t>("controller", "num-threads") == 3);

    BOOST_CHECK_THROW(config->set<uint16_t>("controller", "http-server-port", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<uint16_t>("controller", "http-server-port", 8081));
    BOOST_CHECK(config->get<uint16_t>("controller", "http-server-port") == 8081);

    BOOST_CHECK_THROW(config->set<unsigned int>("controller", "http-max-listen-conn", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "http-max-listen-conn", 1024));
    BOOST_CHECK(config->get<unsigned int>("controller", "http-max-listen-conn") == 1024);

    BOOST_CHECK_THROW(config->set<size_t>("controller", "http-server-threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("controller", "http-server-threads", 4));
    BOOST_CHECK(config->get<size_t>("controller", "http-server-threads") == 4);

    BOOST_CHECK_THROW(config->set<unsigned int>("controller", "request-timeout-sec", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "request-timeout-sec", 101));
    BOOST_CHECK(config->get<unsigned int>("controller", "request-timeout-sec") == 101);

    BOOST_CHECK_THROW(config->set<unsigned int>("controller", "job-timeout-sec", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "job-timeout-sec", 201));
    BOOST_CHECK(config->get<unsigned int>("controller", "job-timeout-sec") == 201);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "job-heartbeat-sec", 301));
    BOOST_CHECK(config->get<unsigned int>("controller", "job-heartbeat-sec") == 301);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "job-heartbeat-sec", 0));
    BOOST_CHECK(config->get<unsigned int>("controller", "job-heartbeat-sec") == 0);

    BOOST_CHECK_THROW(config->set<unsigned int>("controller", "max-repl-level", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "max-repl-level", 3));
    BOOST_CHECK(config->get<unsigned int>("controller", "max-repl-level") == 3);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "worker-evict-priority-level", 1));
    BOOST_CHECK(config->get<int>("controller", "worker-evict-priority-level") == 1);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "worker-evict-priority-level", 0));
    BOOST_CHECK(config->get<int>("controller", "worker-evict-priority-level") == 0);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "health-monitor-priority-level", 2));
    BOOST_CHECK(config->get<int>("controller", "health-monitor-priority-level") == 2);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "health-monitor-priority-level", 0));
    BOOST_CHECK(config->get<int>("controller", "health-monitor-priority-level") == 0);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "ingest-priority-level", 3));
    BOOST_CHECK(config->get<int>("controller", "ingest-priority-level") == 3);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "ingest-priority-level", 0));
    BOOST_CHECK(config->get<int>("controller", "ingest-priority-level") == 0);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "catalog-management-priority-level", 4));
    BOOST_CHECK(config->get<int>("controller", "catalog-management-priority-level") == 4);

    BOOST_REQUIRE_NO_THROW(config->set<int>("controller", "catalog-management-priority-level", 0));
    BOOST_CHECK(config->get<int>("controller", "catalog-management-priority-level") == 0);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "auto-register-workers", 0));
    BOOST_CHECK(config->get<unsigned int>("controller", "auto-register-workers") == 0);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "auto-register-czars", 1));
    BOOST_CHECK(config->get<unsigned int>("controller", "auto-register-czars") == 1);

    BOOST_CHECK_THROW(config->set<uint16_t>("controller", "ingest-job-monitor-ival-sec", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "ingest-job-monitor-ival-sec", 6));
    BOOST_CHECK(config->get<unsigned int>("controller", "ingest-job-monitor-ival-sec") == 6);

    BOOST_CHECK_THROW(config->set<uint16_t>("controller", "num-director-index-connections", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("controller", "num-director-index-connections", 7));
    BOOST_CHECK(config->get<unsigned int>("controller", "num-director-index-connections") == 7);

    BOOST_CHECK_THROW(config->set<string>("controller", "director-index-engine", ""), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<string>("controller", "director-index-engine", "InnoDB"));
    BOOST_CHECK(config->get<string>("controller", "director-index-engine") == "InnoDB");

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("xrootd", "auto-notify", 1));
    BOOST_CHECK(config->get<unsigned int>("xrootd", "auto-notify") != 0);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("xrootd", "auto-notify", 0));
    BOOST_CHECK(config->get<unsigned int>("xrootd", "auto-notify") == 0);

    BOOST_CHECK_THROW(config->set<string>("xrootd", "host", ""), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<string>("xrootd", "host", "localhost"));
    BOOST_CHECK(config->get<string>("xrootd", "host") == "localhost");

    BOOST_CHECK_THROW(config->set<uint16_t>("xrootd", "port", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<uint16_t>("xrootd", "port", 1105));
    BOOST_CHECK(config->get<uint16_t>("xrootd", "port") == 1105);

    BOOST_CHECK_THROW(config->set<unsigned int>("xrootd", "request-timeout-sec", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("xrootd", "request-timeout-sec", 401));
    BOOST_CHECK(config->get<unsigned int>("xrootd", "request-timeout-sec") == 401);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("xrootd", "allow-reconnect", 1));
    BOOST_CHECK(config->get<unsigned int>("xrootd", "allow-reconnect") != 0);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("xrootd", "allow-reconnect", 0));
    BOOST_CHECK(config->get<unsigned int>("xrootd", "allow-reconnect") == 0);

    BOOST_CHECK_THROW(config->set<unsigned int>("xrootd", "reconnect-timeout", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("xrootd", "reconnect-timeout", 403));
    BOOST_CHECK(config->get<unsigned int>("xrootd", "reconnect-timeout") == 403);

    BOOST_CHECK_THROW(config->set<size_t>("database", "services-pool-size", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("database", "services-pool-size", 3));
    BOOST_CHECK(config->get<size_t>("database", "services-pool-size") == 3);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-threads", 4));
    BOOST_CHECK(config->get<size_t>("worker", "num-threads") == 4);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-svc-processing-threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-svc-processing-threads", 5));
    BOOST_CHECK(config->get<size_t>("worker", "num-svc-processing-threads") == 5);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-http-svc-threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-http-svc-threads", 11));
    BOOST_CHECK(config->get<size_t>("worker", "num-http-svc-threads") == 11);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-fs-processing-threads", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-fs-processing-threads", 6));
    BOOST_CHECK(config->get<size_t>("worker", "num-fs-processing-threads") == 6);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "fs-buf-size-bytes", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "fs-buf-size-bytes", 1025));
    BOOST_CHECK(config->get<size_t>("worker", "fs-buf-size-bytes") == 1025);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-loader-processing-threads", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-loader-processing-threads", 7));
    BOOST_CHECK(config->get<size_t>("worker", "num-loader-processing-threads") == 7);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-exporter-processing-threads", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-exporter-processing-threads", 8));
    BOOST_CHECK(config->get<size_t>("worker", "num-exporter-processing-threads") == 8);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-http-loader-processing-threads", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-http-loader-processing-threads", 9));
    BOOST_CHECK(config->get<size_t>("worker", "num-http-loader-processing-threads") == 9);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "num-async-loader-processing-threads", 0),
                      std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "num-async-loader-processing-threads", 10));
    BOOST_CHECK(config->get<size_t>("worker", "num-async-loader-processing-threads") == 10);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "async-loader-auto-resume", 1));
    BOOST_CHECK(config->get<unsigned int>("worker", "async-loader-auto-resume") != 0);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "async-loader-auto-resume", 0));
    BOOST_CHECK(config->get<unsigned int>("worker", "async-loader-auto-resume") == 0);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "async-loader-cleanup-on-resume", 1));
    BOOST_CHECK(config->get<unsigned int>("worker", "async-loader-cleanup-on-resume") != 0);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "async-loader-cleanup-on-resume", 0));
    BOOST_CHECK(config->get<unsigned int>("worker", "async-loader-cleanup-on-resume") == 0);

    BOOST_CHECK_THROW(config->set<unsigned int>("worker", "http-max-listen-conn", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "http-max-listen-conn", 2048));
    BOOST_CHECK(config->get<unsigned int>("worker", "http-max-listen-conn") == 2048);

    BOOST_CHECK(config->get<size_t>("worker", "http-max-queued-requests") == 1024);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "http-max-queued-requests", 2048));
    BOOST_CHECK(config->get<size_t>("worker", "http-max-queued-requests") == 2048);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "http-max-queued-requests", 0));
    BOOST_CHECK(config->get<size_t>("worker", "http-max-queued-requests") == 0);

    BOOST_CHECK(config->get<size_t>("worker", "http-svc-max-queued-requests") == 2048);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "http-svc-max-queued-requests", 4096));
    BOOST_CHECK(config->get<size_t>("worker", "http-svc-max-queued-requests") == 4096);
    BOOST_REQUIRE_NO_THROW(config->set<size_t>("worker", "http-svc-max-queued-requests", 0));
    BOOST_CHECK(config->get<size_t>("worker", "http-svc-max-queued-requests") == 0);

    BOOST_CHECK_THROW(config->set<unsigned int>("worker", "loader-max-warnings", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "loader-max-warnings", 100));
    BOOST_CHECK(config->get<unsigned int>("worker", "loader-max-warnings") == 100);

    BOOST_CHECK_THROW(config->set<string>("worker", "ingest-charset-name", ""), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->set<string>("worker", "ingest-charset-name", "utf8mb3"));
    BOOST_CHECK(config->get<string>("worker", "ingest-charset-name") == "utf8mb3");

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "ingest-num-retries", 0));
    BOOST_CHECK(config->get<unsigned int>("worker", "ingest-num-retries") == 0);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "ingest-num-retries", 2));
    BOOST_CHECK(config->get<unsigned int>("worker", "ingest-num-retries") == 2);

    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "ingest-max-retries", 0));
    BOOST_CHECK(config->get<unsigned int>("worker", "ingest-max-retries") == 0);
    BOOST_REQUIRE_NO_THROW(config->set<unsigned int>("worker", "ingest-max-retries", 100));
    BOOST_CHECK(config->get<unsigned int>("worker", "ingest-max-retries") == 100);

    BOOST_CHECK_THROW(config->set<size_t>("worker", "director-index-record-size", 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(
            config->set<size_t>("worker", "director-index-record-size", ProtocolBuffer::HARD_LIMIT));
    BOOST_CHECK(config->get<size_t>("worker", "director-index-record-size") == ProtocolBuffer::HARD_LIMIT);
}

BOOST_AUTO_TEST_CASE(ConfigurationTestWorkerOperators) {
    LOGS_INFO("Testing worker comparison operators");

    ConfigWorker w1;
    ConfigWorker w2;
    BOOST_CHECK(w1 == w2);
    BOOST_CHECK(!(w1 != w2));

    w1.name = "w1";
    w2.name = "w2";
    BOOST_CHECK(w1 != w2);
    BOOST_CHECK(!(w1 == w2));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestWorkers) {
    LOGS_INFO("Testing worker services");

    // Default assumptions for optional parameters of the workers selector.
    vector<string> workers1;
    BOOST_REQUIRE_NO_THROW(workers1 = config->workers());
    BOOST_CHECK(workers1.size() == 1);
    BOOST_CHECK(workers1 == vector<string>({"worker-A"}));
    BOOST_CHECK(config->numWorkers() == 1);

    // Explicit values of the worker selectors.
    bool isEnabled = true;
    bool isReadOnly = false;
    vector<string> workers2;
    BOOST_REQUIRE_NO_THROW(workers2 = config->workers(isEnabled, isReadOnly));
    sort(workers2.begin(), workers2.end());
    BOOST_CHECK(workers2.size() == 1);
    BOOST_CHECK(workers2 == workers1);

    // Fetch names of all the read-only workers.
    isEnabled = true;
    isReadOnly = true;
    vector<string> workers3;
    BOOST_REQUIRE_NO_THROW(workers3 = config->workers(isEnabled, isReadOnly));
    sort(workers3.begin(), workers3.end());
    BOOST_CHECK(workers3.size() == 1);
    BOOST_CHECK(workers3 == vector<string>({"worker-B"}));

    // Fetch names of all the disabled workers.
    isEnabled = false;
    vector<string> workers4;
    BOOST_REQUIRE_NO_THROW(workers4 = config->workers(isEnabled));
    sort(workers4.begin(), workers4.end());
    BOOST_CHECK(workers4.size() == 1);
    BOOST_CHECK(workers4 == vector<string>({"worker-C"}));

    for (auto&& name : vector<string>({"worker-A", "worker-B", "worker-C"})) {
        BOOST_CHECK(config->isKnownWorker(name));
    }
}

BOOST_AUTO_TEST_CASE(ConfigurationTestWorkerParameters) {
    LOGS_INFO("Testing worker parameters");

    ConfigHost const hostA({"127.0.0.1", "host-A"});
    BOOST_CHECK_EQUAL(hostA.addr, "127.0.0.1");
    BOOST_CHECK_EQUAL(hostA.name, "host-A");

    ConfigWorker workerA;
    BOOST_REQUIRE_NO_THROW(workerA = config->worker("worker-A"));
    BOOST_CHECK(workerA.name == "worker-A");
    BOOST_CHECK(workerA.isEnabled);
    BOOST_CHECK(!workerA.isReadOnly);
    BOOST_CHECK_EQUAL(workerA.svcHost, hostA);
    BOOST_CHECK_EQUAL(workerA.httpSvcHost, hostA);
    BOOST_CHECK_EQUAL(workerA.fsHost, hostA);
    BOOST_CHECK_EQUAL(workerA.loaderHost, hostA);
    BOOST_CHECK_EQUAL(workerA.exporterHost, hostA);
    BOOST_CHECK_EQUAL(workerA.httpLoaderHost, hostA);

    ConfigHost const hostB({"168.1.1.1", "host-B"});
    ConfigWorker workerB;
    BOOST_REQUIRE_NO_THROW(workerB = config->worker("worker-B"));
    BOOST_CHECK(workerB.name == "worker-B");
    BOOST_CHECK(workerB.isEnabled);
    BOOST_CHECK(workerB.isReadOnly);
    BOOST_CHECK_EQUAL(workerB.svcHost, hostB);
    BOOST_CHECK_EQUAL(workerB.httpSvcHost, hostB);
    BOOST_CHECK_EQUAL(workerB.fsHost, hostB);
    BOOST_CHECK_EQUAL(workerB.loaderHost, hostB);
    BOOST_CHECK_EQUAL(workerB.exporterHost, hostB);
    BOOST_CHECK_EQUAL(workerB.httpLoaderHost, hostB);
    BOOST_CHECK_EQUAL(workerB.qservWorker.host, hostB);
    BOOST_CHECK_EQUAL(workerB.qservWorker.port, 53004U);

    ConfigWorker workerC;
    BOOST_REQUIRE_NO_THROW(workerC = config->worker("worker-C"));
    BOOST_CHECK(workerC.name == "worker-C");
    BOOST_CHECK(!workerC.isEnabled);
    BOOST_CHECK_EQUAL(workerC.svcHost, ConfigHost({"168.1.1.1", "host-C1"}));
    BOOST_CHECK_EQUAL(workerC.httpSvcHost, ConfigHost({"168.1.1.7", "host-C7"}));
    BOOST_CHECK_EQUAL(workerC.fsHost, ConfigHost({"168.1.1.2", "host-C2"}));
    BOOST_CHECK_EQUAL(workerC.loaderHost, ConfigHost({"168.1.1.3", "host-C3"}));
    BOOST_CHECK_EQUAL(workerC.exporterHost, ConfigHost({"168.1.1.4", "host-C4"}));
    BOOST_CHECK_EQUAL(workerC.httpLoaderHost, ConfigHost({"168.1.1.5", "host-C5"}));
    BOOST_CHECK_EQUAL(workerC.qservWorker.host, ConfigHost({"168.1.1.6", "host-C6"}));
    BOOST_CHECK_EQUAL(workerC.qservWorker.port, 53005U);

    // Adding a new worker with well formed and unique parameters.
    ConfigWorker workerD;
    workerD.name = "worker-D";
    workerD.isEnabled = true;
    workerD.isReadOnly = true;

    BOOST_REQUIRE_NO_THROW(config->addWorker(workerD));
    BOOST_CHECK_THROW(config->addWorker(workerD), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(workerD = config->worker("worker-D"));
    BOOST_CHECK(workerD.name == "worker-D");
    BOOST_CHECK(workerD.isEnabled);
    BOOST_CHECK(workerD.isReadOnly);

    // Adding a new worker with incomplete set of specs. The only required
    // attribute is the name of the worker.
    ConfigWorker workerE;
    workerE.name = "worker-E";
    ConfigWorker addedWorkerE;
    BOOST_REQUIRE_NO_THROW(addedWorkerE = config->addWorker(workerE));
    BOOST_CHECK(addedWorkerE.name == workerE.name);
    BOOST_CHECK(addedWorkerE.isEnabled == workerE.isEnabled);
    BOOST_CHECK(addedWorkerE.isReadOnly == workerE.isReadOnly);

    // Deleting workers.
    BOOST_REQUIRE_NO_THROW(config->deleteWorker("worker-C"));
    BOOST_CHECK(!config->isKnownWorker("worker-C"));
    BOOST_CHECK_THROW(config->deleteWorker("worker-C"), std::invalid_argument);

    // Updating worker's status.
    ConfigWorker disabledWorker;
    BOOST_REQUIRE_NO_THROW(disabledWorker = config->worker("worker-B"));
    disabledWorker.isEnabled = false;
    BOOST_REQUIRE_NO_THROW(disabledWorker = config->updateWorker(disabledWorker));
    BOOST_CHECK(disabledWorker.name == "worker-B");
    BOOST_CHECK(!disabledWorker.isEnabled);

    ConfigWorker enabledWorker;
    BOOST_REQUIRE_NO_THROW(enabledWorker = config->worker("worker-B"));
    enabledWorker.isEnabled = true;
    BOOST_REQUIRE_NO_THROW(enabledWorker = config->updateWorker(enabledWorker));
    BOOST_CHECK(enabledWorker.name == "worker-B");
    BOOST_CHECK(enabledWorker.isEnabled);

    BOOST_REQUIRE_NO_THROW(disabledWorker = config->disableWorker("worker-B"));
    BOOST_CHECK(disabledWorker.name == "worker-B");
    BOOST_CHECK(!disabledWorker.isEnabled);

    ConfigWorker readOnlyWorker;
    BOOST_REQUIRE_NO_THROW(readOnlyWorker = config->worker("worker-B"));
    readOnlyWorker.isReadOnly = true;
    BOOST_REQUIRE_NO_THROW(readOnlyWorker = config->updateWorker(readOnlyWorker));
    BOOST_CHECK(readOnlyWorker.name == "worker-B");
    BOOST_CHECK(readOnlyWorker.isReadOnly);

    ConfigWorker readWriteWorker;
    BOOST_REQUIRE_NO_THROW(readWriteWorker = config->worker("worker-B"));
    readWriteWorker.isReadOnly = false;
    BOOST_REQUIRE_NO_THROW(readWriteWorker = config->updateWorker(readWriteWorker));
    BOOST_CHECK(readWriteWorker.name == "worker-B");
    BOOST_CHECK(!readWriteWorker.isReadOnly);

    ConfigWorker updatedWorker;
    BOOST_REQUIRE_NO_THROW(updatedWorker = config->worker("worker-A"));
    BOOST_REQUIRE_NO_THROW(updatedWorker = config->updateWorker(updatedWorker));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestFamilies) {
    LOGS_INFO("Testing database families");

    // Selecting and probing database families.
    vector<string> families;
    BOOST_REQUIRE_NO_THROW(families = config->databaseFamilies());
    sort(families.begin(), families.end());
    BOOST_CHECK(families.size() == 2);
    BOOST_CHECK(families == vector<string>({"production", "test"}));
    for (auto&& name : families) {
        BOOST_CHECK(config->isKnownDatabaseFamily(name));
    }
    DatabaseFamilyInfo production;
    BOOST_REQUIRE_NO_THROW(production = config->databaseFamilyInfo("production"));
    BOOST_CHECK(production.name == "production");
    BOOST_CHECK(production.replicationLevel == 1);
    BOOST_CHECK(production.numStripes == 11);
    BOOST_CHECK(production.numSubStripes == 12);
    BOOST_CHECK(abs(production.overlap - 0.01667) <= numeric_limits<double>::epsilon());
    DatabaseFamilyInfo test;
    BOOST_REQUIRE_NO_THROW(test = config->databaseFamilyInfo("test"));
    BOOST_CHECK(test.name == "test");
    BOOST_CHECK(test.replicationLevel == 2);
    BOOST_CHECK(test.numStripes == 14);
    BOOST_CHECK(test.numSubStripes == 15);
    BOOST_CHECK(abs(test.overlap - 0.001) <= numeric_limits<double>::epsilon());
    BOOST_CHECK(config->replicationLevel("production") == 1);
    BOOST_CHECK(config->replicationLevel("test") == 2);

    // Adding new families.
    DatabaseFamilyInfo newFamily;
    newFamily.name = "new";
    newFamily.replicationLevel = 3;
    newFamily.numStripes = 301;
    newFamily.numSubStripes = 302;
    newFamily.overlap = 0.001;
    DatabaseFamilyInfo newFamilyAdded;
    BOOST_CHECK(!config->isKnownDatabaseFamily("new"));
    BOOST_REQUIRE_NO_THROW(newFamilyAdded = config->addDatabaseFamily(newFamily));
    BOOST_CHECK(config->isKnownDatabaseFamily("new"));
    BOOST_CHECK(newFamilyAdded.name == "new");
    BOOST_CHECK(newFamilyAdded.replicationLevel == 3);
    BOOST_CHECK(newFamilyAdded.numStripes == 301);
    BOOST_CHECK(newFamilyAdded.numSubStripes == 302);
    BOOST_CHECK(abs(newFamilyAdded.overlap - 0.001) <= numeric_limits<double>::epsilon());

    // Modify the replication level
    BOOST_REQUIRE_THROW(config->setReplicationLevel("", 5), std::invalid_argument);
    BOOST_REQUIRE_THROW(config->setReplicationLevel(newFamilyAdded.name, 0), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->setReplicationLevel(newFamilyAdded.name, 5));
    BOOST_CHECK(config->databaseFamilyInfo(newFamilyAdded.name).replicationLevel == 5);

    // Test the effictive level
    BOOST_CHECK(config->effectiveReplicationLevel(newFamilyAdded.name) <= newFamilyAdded.replicationLevel);
    BOOST_CHECK(config->effectiveReplicationLevel(newFamilyAdded.name) <= config->numWorkers());
    BOOST_CHECK(config->effectiveReplicationLevel(newFamilyAdded.name) <=
                config->get<size_t>("controller", "max-repl-level"));
    BOOST_CHECK(config->effectiveReplicationLevel(newFamilyAdded.name, 6) <= 6);
    BOOST_CHECK(config->effectiveReplicationLevel(newFamilyAdded.name, 6) <= config->numWorkers());
    BOOST_CHECK(config->effectiveReplicationLevel(newFamilyAdded.name, 6) <=
                config->get<size_t>("controller", "max-repl-level"));

    // Deleting existing families,
    BOOST_REQUIRE_NO_THROW(config->deleteDatabaseFamily("new"));
    BOOST_CHECK(!config->isKnownDatabaseFamily("new"));

    // Deleting non-existing families.
    BOOST_REQUIRE_THROW(config->deleteDatabaseFamily(""), std::invalid_argument);
    BOOST_REQUIRE_THROW(config->deleteDatabaseFamily("non-existing"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ConfigurationTestReadingDatabases) {
    LOGS_INFO("Testing reading databases");

    // Database selectors.
    vector<string> databases1;
    BOOST_REQUIRE_NO_THROW(databases1 = config->databases());
    sort(databases1.begin(), databases1.end());
    BOOST_CHECK(databases1.size() == 5);
    BOOST_CHECK(databases1 == vector<string>({"db1", "db2", "db3", "db4", "db5"}));
    vector<string> databases2;
    BOOST_REQUIRE_NO_THROW(databases2 = config->databases("production"));
    sort(databases2.begin(), databases2.end());
    BOOST_CHECK(databases2.size() == 3);
    BOOST_CHECK(databases2 == vector<string>({"db1", "db2", "db3"}));
    vector<string> databases3;
    BOOST_REQUIRE_NO_THROW(databases3 = config->databases("test"));
    sort(databases3.begin(), databases3.end());
    BOOST_CHECK(databases3.size() == 2);
    BOOST_CHECK(databases3 == vector<string>({"db4", "db5"}));
    bool allDatabases = false;
    bool isPublished = true;
    vector<string> databases4;
    BOOST_REQUIRE_NO_THROW(databases4 = config->databases("test", allDatabases, isPublished));
    sort(databases4.begin(), databases4.end());
    BOOST_CHECK(databases4.size() == 2);
    BOOST_CHECK(databases4 == vector<string>({"db4", "db5"}));
    isPublished = false;
    vector<string> databases5;
    BOOST_REQUIRE_NO_THROW(databases5 = config->databases("test", allDatabases, isPublished));
    sort(databases5.begin(), databases5.end());
    BOOST_CHECK(databases5.size() == 1);
    BOOST_CHECK(databases5 == vector<string>({"db6"}));
    allDatabases = true;
    vector<string> databases6;
    BOOST_REQUIRE_NO_THROW(databases6 = config->databases("test", allDatabases));
    sort(databases6.begin(), databases6.end());
    BOOST_CHECK(databases6.size() == 3);
    BOOST_CHECK(databases6 == vector<string>({"db4", "db5", "db6"}));
    isPublished = true;
    vector<string> databases7;
    BOOST_REQUIRE_NO_THROW(databases7 = config->databases("test", allDatabases, isPublished));
    sort(databases7.begin(), databases7.end());
    BOOST_CHECK(databases7.size() == 3);
    BOOST_CHECK(databases7 == vector<string>({"db4", "db5", "db6"}));
    isPublished = false;
    vector<string> databases8;
    BOOST_REQUIRE_NO_THROW(databases8 = config->databases("test", allDatabases, isPublished));
    sort(databases8.begin(), databases8.end());
    BOOST_CHECK(databases8.size() == 3);
    BOOST_CHECK(databases8 == vector<string>({"db4", "db5", "db6"}));
    for (auto&& name : vector<string>({"db1", "db2", "db3", "db4", "db5", "db6"})) {
        BOOST_CHECK(config->isKnownDatabase(name));
    }
}

BOOST_AUTO_TEST_CASE(ConfigurationTestReadingTables) {
    LOGS_INFO("Testing reading tables");

    DirectorTableRef tableRef1;
    BOOST_CHECK(tableRef1.empty());
    BOOST_CHECK(tableRef1.databaseName().empty());
    BOOST_CHECK(tableRef1.tableName().empty());
    BOOST_CHECK(tableRef1.primaryKeyColumn().empty());

    DirectorTableRef tableRef2("", "id");
    BOOST_CHECK(!tableRef2.empty());
    BOOST_CHECK(tableRef2.databaseName().empty());
    BOOST_CHECK(tableRef2.tableName().empty());
    BOOST_CHECK(tableRef2.primaryKeyColumn() == "id");

    DirectorTableRef tableRef3("table", "id");
    BOOST_CHECK(!tableRef3.empty());
    BOOST_CHECK(tableRef3.databaseName().empty());
    BOOST_CHECK(tableRef3.tableName() == "table");
    BOOST_CHECK(tableRef3.primaryKeyColumn() == "id");

    DirectorTableRef tableRef4("db.table", "id");
    BOOST_CHECK(!tableRef4.empty());
    BOOST_CHECK(tableRef4.databaseName() == "db");
    BOOST_CHECK(tableRef4.tableName() == "table");
    BOOST_CHECK(tableRef4.primaryKeyColumn() == "id");

    DirectorTableRef tableRef5 = tableRef4;
    BOOST_CHECK_EQUAL(tableRef5, tableRef4);
    BOOST_CHECK(!tableRef5.empty());

    DatabaseInfo database;
    vector<string> tables;
    TableInfo table;
    BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("db1"));
    BOOST_CHECK(database.name == "db1");
    BOOST_CHECK(database.family == "production");
    BOOST_CHECK(database.isPublished);
    BOOST_CHECK(database.createTime == 10);
    BOOST_CHECK(database.publishTime == 11);

    tables = database.tables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 2);
    BOOST_CHECK(tables == vector<string>({"MetaTable11", "Table11"}));

    tables = database.partitionedTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table11"}));

    tables = database.directorTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table11"}));

    tables = database.refMatchTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 0);

    tables = database.regularTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"MetaTable11"}));

    BOOST_CHECK(database.tableExists("Table11"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table11"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id11"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName == "decl11");
    BOOST_CHECK(table.longitudeColName == "ra11");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id11", "decl11", "ra11", "subChunkId"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 110);
    BOOST_CHECK(table.publishTime == 111);

    BOOST_CHECK(database.tableExists("MetaTable11"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable11"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 120);
    BOOST_CHECK(table.publishTime == 121);

    BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("db2"));
    BOOST_CHECK(database.name == "db2");
    BOOST_CHECK(database.family == "production");
    BOOST_CHECK(database.isPublished);
    BOOST_CHECK(database.createTime == 20);
    BOOST_CHECK(database.publishTime == 21);

    tables = database.tables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 4);
    BOOST_CHECK(tables == vector<string>({"MetaTable21", "MetaTable22", "Table21", "Table22"}));

    tables = database.partitionedTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 2);
    BOOST_CHECK(tables == vector<string>({"Table21", "Table22"}));

    tables = database.directorTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table21"}));

    tables = database.refMatchTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 0);

    tables = database.regularTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 2);
    BOOST_CHECK(tables == vector<string>({"MetaTable21", "MetaTable22"}));

    BOOST_CHECK(database.tableExists("Table21"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table21"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id21"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, false);
    BOOST_CHECK(table.latitudeColName == "decl21");
    BOOST_CHECK(table.longitudeColName == "ra21");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id21", "decl21", "ra21", "subChunkId"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 210);
    BOOST_CHECK(table.publishTime == 211);

    BOOST_CHECK(database.tableExists("Table22"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table22"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("Table21", "id22"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName == "decl22");
    BOOST_CHECK(table.longitudeColName == "ra22");
    BOOST_CHECK(table.columns.size() == 3);
    BOOST_CHECK(table.columnNames() == vector<string>({"id22", "decl22", "ra22"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 220);
    BOOST_CHECK(table.publishTime == 221);

    BOOST_CHECK(database.tableExists("MetaTable21"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable21"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 2210);
    BOOST_CHECK(table.publishTime == 2211);

    BOOST_CHECK(database.tableExists("MetaTable22"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable22"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 2220);
    BOOST_CHECK(table.publishTime == 2221);

    BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("db3"));
    BOOST_CHECK(database.name == "db3");
    BOOST_CHECK(database.family == "production");
    BOOST_CHECK(database.isPublished);
    BOOST_CHECK(database.createTime == 30);
    BOOST_CHECK(database.publishTime == 31);

    tables = database.tables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 6);
    BOOST_CHECK(tables == vector<string>({"MetaTable31", "MetaTable32", "MetaTable33", "Table31", "Table32",
                                          "Table33"}));

    tables = database.partitionedTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 3);
    BOOST_CHECK(tables == vector<string>({"Table31", "Table32", "Table33"}));

    tables = database.directorTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table31"}));

    tables = database.refMatchTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 0);

    tables = database.regularTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 3);
    BOOST_CHECK(tables == vector<string>({"MetaTable31", "MetaTable32", "MetaTable33"}));

    BOOST_CHECK(database.tableExists("Table31"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table31"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id31"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName == "decl31");
    BOOST_CHECK(table.longitudeColName == "ra31");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id31", "decl31", "ra31", "subChunkId"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 310);
    BOOST_CHECK(table.publishTime == 311);

    BOOST_CHECK(database.tableExists("Table32"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table32"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("Table31", "id32"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName == "decl32");
    BOOST_CHECK(table.longitudeColName == "ra32");
    BOOST_CHECK(table.columns.size() == 3);
    BOOST_CHECK(table.columnNames() == vector<string>({"id32", "decl32", "ra32"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 320);
    BOOST_CHECK(table.publishTime == 321);

    BOOST_CHECK(database.tableExists("Table33"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table33"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("Table31", "id33"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.size() == 1);
    BOOST_CHECK(table.columnNames() == vector<string>({"id33"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 330);
    BOOST_CHECK(table.publishTime == 331);

    BOOST_CHECK(database.tableExists("MetaTable31"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable31"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 3310);
    BOOST_CHECK(table.publishTime == 3311);

    BOOST_CHECK(database.tableExists("MetaTable32"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable32"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 3320);
    BOOST_CHECK(table.publishTime == 3321);

    BOOST_CHECK(database.tableExists("MetaTable33"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable33"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(!table.isPublished);
    BOOST_CHECK(table.createTime == 3330);
    BOOST_CHECK(table.publishTime == 0);

    BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("db4"));
    BOOST_CHECK(database.name == "db4");
    BOOST_CHECK(database.family == "test");
    BOOST_CHECK(database.isPublished);
    BOOST_CHECK(database.createTime == 40);
    BOOST_CHECK(database.publishTime == 41);

    tables = database.tables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 4);
    BOOST_CHECK(tables == vector<string>({"RefMatch43", "RefMatch44", "Table41", "Table42"}));

    tables = database.partitionedTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 4);
    BOOST_CHECK(tables == vector<string>({"RefMatch43", "RefMatch44", "Table41", "Table42"}));

    tables = database.directorTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 2);
    BOOST_CHECK(tables == vector<string>({"Table41", "Table42"}));

    tables = database.refMatchTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 2);
    BOOST_CHECK(tables == vector<string>({"RefMatch43", "RefMatch44"}));

    tables = database.regularTables();
    BOOST_CHECK(tables.size() == 0);

    BOOST_CHECK(database.tableExists("Table41"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table41"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id41"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName == "decl41");
    BOOST_CHECK(table.longitudeColName == "ra41");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id41", "decl41", "ra41", "subChunkId"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 410);
    BOOST_CHECK(table.publishTime == 411);

    BOOST_CHECK(database.tableExists("Table42"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table42"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id42"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName == "decl42");
    BOOST_CHECK(table.longitudeColName == "ra42");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id42", "decl42", "ra42", "subChunkId"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 420);
    BOOST_CHECK(table.publishTime == 421);

    BOOST_CHECK(database.tableExists("RefMatch43"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("RefMatch43"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("Table41", "Table41_id"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("Table42", "Table42_id"));
    BOOST_CHECK(table.flagColName == "flag");
    BOOST_CHECK(table.angSep == 0.01);
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.size() == 3);
    BOOST_CHECK(table.columnNames() == vector<string>({"Table41_id", "Table42_id", "flag"}));
    BOOST_CHECK(!table.isPublished);
    BOOST_CHECK(table.createTime == 430);
    BOOST_CHECK(table.publishTime == 0);

    BOOST_CHECK(database.tableExists("RefMatch44"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("RefMatch44"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("db2.Table21", "Table21_id"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("db3.Table31", "Table31_id"));
    BOOST_CHECK(table.flagColName == "flag");
    BOOST_CHECK(table.angSep == 0.01667);
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.size() == 3);
    BOOST_CHECK(table.columnNames() == vector<string>({"Table21_id", "Table31_id", "flag"}));
    BOOST_CHECK(!table.isPublished);
    BOOST_CHECK(table.createTime == 440);
    BOOST_CHECK(table.publishTime == 0);

    BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("db5"));
    BOOST_CHECK(database.name == "db5");
    BOOST_CHECK(database.family == "test");
    BOOST_CHECK(database.isPublished);
    BOOST_CHECK(database.createTime == 50);
    BOOST_CHECK(database.publishTime == 51);

    tables = database.tables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table51"}));

    tables = database.partitionedTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table51"}));

    tables = database.directorTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table51"}));

    tables = database.refMatchTables();
    BOOST_CHECK(tables.empty());

    tables = database.regularTables();
    BOOST_CHECK(tables.size() == 0);

    BOOST_CHECK(database.tableExists("Table51"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table51"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id51"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName == "decl51");
    BOOST_CHECK(table.longitudeColName == "ra51");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id51", "decl51", "ra51", "subChunkId"}));
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 510);
    BOOST_CHECK(table.publishTime == 511);

    BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("db6"));
    BOOST_CHECK(database.name == "db6");
    BOOST_CHECK(database.family == "test");
    BOOST_CHECK(!database.isPublished);
    BOOST_CHECK(database.createTime == 60);
    BOOST_CHECK(database.publishTime == 0);

    tables = database.tables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 2);
    BOOST_CHECK(tables == vector<string>({"MetaTable61", "Table61"}));

    tables = database.partitionedTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table61"}));

    tables = database.directorTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"Table61"}));

    tables = database.refMatchTables();
    sort(tables.begin(), tables.end());
    BOOST_CHECK(tables.empty());

    tables = database.regularTables();
    BOOST_CHECK(tables.size() == 1);
    BOOST_CHECK(tables == vector<string>({"MetaTable61"}));

    BOOST_CHECK(database.tableExists("Table61"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("Table61"));
    BOOST_CHECK(table.isPartitioned);
    BOOST_CHECK(table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", "id61"));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName == "decl61");
    BOOST_CHECK(table.longitudeColName == "ra61");
    BOOST_CHECK(table.columns.size() == 4);
    BOOST_CHECK(table.columnNames() == vector<string>({"id61", "decl61", "ra61", "subChunkId"}));
    BOOST_CHECK(!table.isPublished);
    BOOST_CHECK(table.createTime == 610);
    BOOST_CHECK(table.publishTime == 0);

    BOOST_CHECK(database.tableExists("MetaTable61"));
    BOOST_REQUIRE_NO_THROW(table = database.findTable("MetaTable61"));
    BOOST_CHECK(!table.isPartitioned);
    BOOST_CHECK(!table.isDirector());
    BOOST_CHECK(!table.isRefMatch());
    BOOST_CHECK_EQUAL(table.directorTable, DirectorTableRef("", ""));
    BOOST_CHECK_EQUAL(table.directorTable2, DirectorTableRef("", ""));
    BOOST_CHECK(table.flagColName.empty());
    BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
    BOOST_CHECK(table.latitudeColName.empty());
    BOOST_CHECK(table.longitudeColName.empty());
    BOOST_CHECK(table.columns.empty());
    BOOST_CHECK(table.isPublished);
    BOOST_CHECK(table.createTime == 6610);
    BOOST_CHECK(table.publishTime == 6611);
}

BOOST_AUTO_TEST_CASE(ConfigurationTestAddingDatabases) {
    LOGS_INFO("Testing adding databases");

    // Adding new databases.
    {
        string const databaseName = "new";
        string const familyName = "test";
        DatabaseInfo database;
        BOOST_REQUIRE_NO_THROW(database = config->addDatabase(databaseName, familyName));
        BOOST_CHECK(database.name == databaseName);
        BOOST_CHECK(database.family == familyName);
        BOOST_CHECK(!database.isPublished);
        BOOST_CHECK(database.createTime != 0);
        BOOST_CHECK(database.publishTime == 0);
        BOOST_CHECK(database.tables().empty());
        BOOST_CHECK(database.partitionedTables().empty());
        BOOST_CHECK(database.directorTables().empty());
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().empty());
        BOOST_CHECK_THROW(database.findTable("NonExistingTable"), std::invalid_argument);
        BOOST_CHECK_THROW(config->addDatabase(databaseName, familyName), std::invalid_argument);
    }
    BOOST_CHECK_THROW(config->addDatabase("", ""), std::invalid_argument);
    BOOST_CHECK_THROW(config->addDatabase("", "unknown"), std::invalid_argument);
    BOOST_CHECK_THROW(config->addDatabase("another", ""), std::invalid_argument);
    BOOST_CHECK_THROW(config->addDatabase("another", "unknown"), std::invalid_argument);
    {
        SqlColDef const emptyColumn;
        BOOST_CHECK(emptyColumn.name.empty());
        BOOST_CHECK(emptyColumn.type.empty());

        SqlColDef const column("itsName", "itsType");
        BOOST_CHECK(column.name == "itsName");
        BOOST_CHECK(column.type == "itsType");

        SqlColDef const copiedColumn(column);
        BOOST_CHECK(copiedColumn.name == "itsName");
        BOOST_CHECK(copiedColumn.type == "itsType");

        SqlColDef const assignedColumn = column;
        BOOST_CHECK(assignedColumn.name == "itsName");
        BOOST_CHECK(assignedColumn.type == "itsType");
    }
}

BOOST_AUTO_TEST_CASE(ConfigurationTestModifyingTables) {
    LOGS_INFO("Testing modifying tables");
    {
        DatabaseInfo database;
        BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("new"));
        BOOST_CHECK(database.tables().empty());
        BOOST_CHECK(database.directorTables().empty());
        BOOST_CHECK(database.partitionedTables().empty());
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().empty());

        TableInfo inTable;
        inTable.name = "T1";
        inTable.database = database.name;
        inTable.isPartitioned = true;
        inTable.directorTable = DirectorTableRef("", "objectId");
        inTable.latitudeColName = "lat";
        inTable.longitudeColName = "lon";
        inTable.columns.emplace_back(inTable.directorTable.primaryKeyColumn(), "INT UNSIGNED");
        inTable.columns.emplace_back(inTable.latitudeColName, "DOUBLE");
        inTable.columns.emplace_back(inTable.longitudeColName, "DOUBLE");
        inTable.columns.emplace_back(lsst::qserv::SUB_CHUNK_COLUMN, "INT");

        BOOST_REQUIRE_NO_THROW(database = config->addTable(inTable));
        BOOST_CHECK(database.tables().size() == 1);
        BOOST_CHECK(database.partitionedTables().size() == 1);
        BOOST_CHECK(database.directorTables().size() == 1);
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().empty());

        TableInfo table;
        BOOST_REQUIRE_NO_THROW(table = database.findTable(inTable.name));
        BOOST_CHECK_EQUAL(table, inTable);
        BOOST_CHECK(!table.isPublished);
        BOOST_CHECK(table.isDirector());
        BOOST_CHECK_EQUAL(table.uniquePrimaryKey, true);
        BOOST_CHECK(table.createTime != 0);
        BOOST_CHECK(table.publishTime == 0);
        BOOST_CHECK_THROW(config->addTable(inTable), std::invalid_argument);
    }
    {
        DatabaseInfo database;
        BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("new"));
        BOOST_CHECK(database.tables().size() == 1);
        BOOST_CHECK(database.partitionedTables().size() == 1);
        BOOST_CHECK(database.directorTables().size() == 1);
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().empty());

        TableInfo inTable;
        inTable.name = "T2";
        inTable.database = database.name;
        inTable.isPartitioned = true;
        inTable.directorTable = DirectorTableRef("T1", "idT2");
        inTable.latitudeColName = "declT2";
        inTable.longitudeColName = "raT2";
        inTable.columns.emplace_back(inTable.directorTable.primaryKeyColumn(), "INT UNSIGNED");
        inTable.columns.emplace_back(inTable.latitudeColName, "DOUBLE");
        inTable.columns.emplace_back(inTable.longitudeColName, "DOUBLE");

        BOOST_REQUIRE_NO_THROW(database = config->addTable(inTable));
        BOOST_CHECK(database.tables().size() == 2);
        BOOST_CHECK(database.partitionedTables().size() == 2);
        BOOST_CHECK(database.directorTables().size() == 1);
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().empty());

        TableInfo table;
        BOOST_REQUIRE_NO_THROW(table = database.findTable(inTable.name));
        BOOST_CHECK_EQUAL(table, inTable);
        BOOST_CHECK(!table.isPublished);
        BOOST_CHECK(table.createTime != 0);
        BOOST_CHECK(table.publishTime == 0);
        BOOST_CHECK_THROW(config->addTable(inTable), std::invalid_argument);
    }
    {
        DatabaseInfo database;
        BOOST_REQUIRE_NO_THROW(database = config->databaseInfo("new"));
        BOOST_CHECK(database.tables().size() == 2);
        BOOST_CHECK(database.partitionedTables().size() == 2);
        BOOST_CHECK(database.directorTables().size() == 1);
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().empty());

        TableInfo inTable;
        inTable.name = "T3";
        inTable.database = database.name;

        BOOST_REQUIRE_NO_THROW(database = config->addTable(inTable));
        BOOST_CHECK(database.tables().size() == 3);
        BOOST_CHECK(database.partitionedTables().size() == 2);
        BOOST_CHECK(database.directorTables().size() == 1);
        BOOST_CHECK(database.refMatchTables().empty());
        BOOST_CHECK(database.regularTables().size() == 1);

        TableInfo table;
        BOOST_REQUIRE_NO_THROW(table = database.findTable(inTable.name));
        BOOST_CHECK_EQUAL(table, inTable);
        BOOST_CHECK(!table.isPublished);
        BOOST_CHECK(table.createTime != 0);
        BOOST_CHECK(table.publishTime == 0);

        // Unsuccessful deletion attempt should leave the table intact
        BOOST_CHECK_THROW(config->addTable(inTable), std::invalid_argument);
        BOOST_CHECK(config->databaseInfo("new").tableExists(inTable.name));
    }
    BOOST_REQUIRE_NO_THROW(config->deleteTable("new", "T3"));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestPublishingDatabases) {
    LOGS_INFO("Testing publishing databases");
    {
        DatabaseInfo database;
        BOOST_REQUIRE_NO_THROW(database = config->publishDatabase("new"));
        BOOST_CHECK(database.name == "new");
        BOOST_CHECK(database.family == "test");
        BOOST_CHECK(database.isPublished == true);
        BOOST_CHECK(database.tables().size() == 2);
        BOOST_CHECK_THROW(database = config->publishDatabase("new"), std::logic_error);
    }

    // Adding tables to the database after it's published isn't allowed.
    TableInfo inTable;
    inTable.name = "T4";
    inTable.database = "new";
    BOOST_CHECK_THROW(config->addTable(inTable), std::invalid_argument);

    // Deleting director tables which may still have dependent ones is not allowed
    BOOST_CHECK_THROW(config->deleteTable("new", "T1"), std::invalid_argument);
    BOOST_REQUIRE_NO_THROW(config->deleteTable("new", "T2"));
    // Now we can do this, after deleting the dependent one.
    BOOST_REQUIRE_NO_THROW(config->deleteTable("new", "T1"));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestUnPublishingDatabases) {
    LOGS_INFO("Testing un-publishing databases");
    {
        DatabaseInfo database;
        BOOST_REQUIRE_NO_THROW(database = config->unPublishDatabase("new"));
        BOOST_CHECK(database.name == "new");
        BOOST_CHECK(database.family == "test");
        BOOST_CHECK(!database.isPublished);
        BOOST_CHECK_THROW(database = config->unPublishDatabase("new"), std::logic_error);
    }

    // Adding tables to the database should be now allowed.
    TableInfo inTable;
    inTable.name = "T4";
    inTable.database = "new";
    BOOST_CHECK_NO_THROW(config->addTable(inTable));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestDeletingDatabases) {
    LOGS_INFO("Testing deleting databases");
    BOOST_REQUIRE_NO_THROW(config->deleteDatabase("new"));
    BOOST_CHECK_THROW(config->deleteDatabase("new"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ConfigurationTestDeletingFamilies) {
    LOGS_INFO("Testing deleting families");

    // Test if deleting a family would also eliminate the dependent databases
    BOOST_REQUIRE_NO_THROW(config->deleteDatabaseFamily("production"));
    BOOST_CHECK(!config->isKnownDatabaseFamily("production"));
    BOOST_CHECK(!config->isKnownDatabase("db1"));
    BOOST_CHECK(!config->isKnownDatabase("db2"));
    BOOST_CHECK(!config->isKnownDatabase("db3"));
    // Databases of the family "test" should not have been affected by the operation.
    BOOST_CHECK(config->isKnownDatabase("db4"));
    BOOST_CHECK(config->isKnownDatabase("db5"));
    BOOST_CHECK(config->isKnownDatabase("db6"));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestCzarOperators) {
    LOGS_INFO("Testing Czar comparison operators");

    ConfigCzar c1;
    ConfigCzar c2;
    BOOST_CHECK(c1 == c2);
    BOOST_CHECK(!(c1 != c2));

    c1.name = "c1";
    c2.name = "c2";
    BOOST_CHECK(c1 != c2);
    BOOST_CHECK(!(c1 == c2));
}

BOOST_AUTO_TEST_CASE(ConfigurationTestCzars) {
    LOGS_INFO("Testing Czar collection");

    vector<string> czars1;
    BOOST_REQUIRE_NO_THROW(czars1 = config->allCzars());
    BOOST_CHECK_EQUAL(czars1.size(), 1U);
    BOOST_CHECK(czars1 == vector<string>({"default"}));
    BOOST_CHECK_EQUAL(config->numCzars(), 1U);

    for (auto&& name : vector<string>({"default"})) {
        BOOST_CHECK(config->isKnownCzar(name));
    }
}

BOOST_AUTO_TEST_CASE(ConfigurationTestCzarParameters) {
    LOGS_INFO("Testing Czar parameters");

    ConfigHost const hostA({"127.0.0.1", "host-A"});
    BOOST_CHECK_EQUAL(hostA.addr, "127.0.0.1");
    BOOST_CHECK_EQUAL(hostA.name, "host-A");

    ConfigCzar czarDefault;
    BOOST_REQUIRE_NO_THROW(czarDefault = config->czar("default"));
    BOOST_CHECK_EQUAL(czarDefault.name, "default");
    BOOST_CHECK_EQUAL(czarDefault.id, 123);
    BOOST_CHECK_EQUAL(czarDefault.host, hostA);
    BOOST_CHECK_EQUAL(czarDefault.port, 59001U);

    // Adding a new Czar with well formed and unique parameters.
    ConfigHost const hostB({"192.10.10.12", "host-B"});
    ConfigCzar czarSecond;
    czarSecond.name = "second";
    czarSecond.id = 456;
    czarSecond.host = hostB;
    czarSecond.port = 59002U;

    BOOST_REQUIRE_NO_THROW(config->addCzar(czarSecond));
    BOOST_REQUIRE_NO_THROW(czarSecond = config->czar("second"));
    BOOST_CHECK(czarSecond.name == "second");
    BOOST_CHECK(czarSecond.id == 456);
    BOOST_CHECK_EQUAL(czarSecond.host, hostB);
    BOOST_CHECK_EQUAL(czarSecond.port, 59002U);
    BOOST_CHECK_EQUAL(config->numCzars(), 2U);

    BOOST_CHECK_THROW(config->addCzar(czarSecond), std::invalid_argument);
    BOOST_CHECK_EQUAL(config->numCzars(), 2U);

    // Adding a new Czar with incomplete set of specs. The only required
    // attribute is the name of the Czar.
    ConfigCzar czarIncomplete;
    czarIncomplete.name = "incomplete";
    ConfigCzar addedCzarIncomplete;
    BOOST_REQUIRE_NO_THROW(addedCzarIncomplete = config->addCzar(czarIncomplete));
    BOOST_CHECK(addedCzarIncomplete.name == czarIncomplete.name);
    BOOST_CHECK(addedCzarIncomplete.id == czarIncomplete.id);
    BOOST_CHECK_EQUAL(addedCzarIncomplete.host, czarIncomplete.host);
    BOOST_CHECK_EQUAL(addedCzarIncomplete.port, czarIncomplete.port);
    BOOST_CHECK_EQUAL(config->numCzars(), 3U);

    // Make sure the following attempt is blocked and it has no side effects.
    ConfigCzar czarUnknown;
    czarUnknown.name = "unknown";
    BOOST_CHECK_THROW(config->updateCzar(czarUnknown), std::invalid_argument);
    BOOST_CHECK_EQUAL(config->numCzars(), 3U);

    // Updating an existing Czar
    ConfigHost const hostC({"192.1.1.1", "host-C"});
    addedCzarIncomplete.host = hostC;
    addedCzarIncomplete.port = 59003U;
    ConfigCzar updatedIncompleteCzar;
    BOOST_REQUIRE_NO_THROW(updatedIncompleteCzar = config->updateCzar(addedCzarIncomplete));
    BOOST_CHECK_EQUAL(updatedIncompleteCzar.name, "incomplete");
    BOOST_CHECK_EQUAL(updatedIncompleteCzar.host, hostC);
    BOOST_CHECK_EQUAL(updatedIncompleteCzar.port, 59003U);
    BOOST_CHECK_EQUAL(config->numCzars(), 3U);

    // Adding a new Czar with incorrect spec (missing mandatory name).
    ConfigCzar czarEmpty;
    BOOST_CHECK_THROW(config->addCzar(czarEmpty), std::invalid_argument);
    BOOST_CHECK_EQUAL(config->numCzars(), 3U);

    // Deleting Czars.
    BOOST_REQUIRE_NO_THROW(config->deleteCzar("second"));
    BOOST_CHECK(!config->isKnownCzar("second"));

    BOOST_CHECK_EQUAL(config->numCzars(), 2U);
    BOOST_CHECK_THROW(config->deleteCzar("second"), std::invalid_argument);
    BOOST_CHECK_EQUAL(config->numCzars(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()
