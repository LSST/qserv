// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2016 AURA/LSST.
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
#include "wconfig/WorkerConfig.h"

// System headers
#include <sstream>
#include <stdexcept>

// Third party headers
#include <boost/algorithm/string/predicate.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "mysql/MySqlConfig.h"
#include "util/ConfigStoreError.h"
#include "wsched/BlendScheduler.h"

using namespace lsst::qserv::wconfig;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.wconfig.WorkerConfig");

WorkerConfig::ResultDeliveryProtocol parseResultDeliveryProtocol(std::string const& str) {
    // Using BOOST's 'iequals' for case-insensitive comparisons.
    if (str.empty() || boost::iequals(str, "HTTP")) {
        return WorkerConfig::ResultDeliveryProtocol::HTTP;
    } else if (boost::iequals(str, "HTTP")) {
        return WorkerConfig::ResultDeliveryProtocol::HTTP;
    } else if (boost::iequals(str, "XROOT")) {
        return WorkerConfig::ResultDeliveryProtocol::XROOT;
    }
    throw std::invalid_argument("WorkerConfig::" + std::string(__func__) + " unsupported method '" + str +
                                "'.");
}
}  // namespace

namespace lsst::qserv::wconfig {

std::mutex WorkerConfig::_mtxOnInstance;

std::shared_ptr<WorkerConfig> WorkerConfig::_instance;

std::shared_ptr<WorkerConfig> WorkerConfig::create(std::string const& configFileName) {
    std::lock_guard<std::mutex> const lock(_mtxOnInstance);
    if (_instance == nullptr) {
        _instance = std::shared_ptr<WorkerConfig>(
                configFileName.empty() ? new WorkerConfig()
                                       : new WorkerConfig(util::ConfigStore(configFileName)));
    }
    return _instance;
}

std::shared_ptr<WorkerConfig> WorkerConfig::instance() {
    std::lock_guard<std::mutex> const lock(_mtxOnInstance);
    if (_instance == nullptr) {
        throw std::logic_error("WorkerConfig::" + std::string(__func__) + ": instance has not been created.");
    }
    return _instance;
}

std::string WorkerConfig::protocol2str(ResultDeliveryProtocol const& p) {
    switch (p) {
        case WorkerConfig::ResultDeliveryProtocol::HTTP:
            return "HTTP";
        case WorkerConfig::ResultDeliveryProtocol::XROOT:
            return "XROOT";
    }
    throw std::invalid_argument("WorkerConfig::" + std::string(__func__) + ": unknown protocol " +
                                std::to_string(static_cast<int>(p)));
}

WorkerConfig::WorkerConfig()
        : _jsonConfig(nlohmann::json::object(
                  {{"input", nlohmann::json::object()}, {"actual", nlohmann::json::object()}})),
          _memManClass("MemManReal"),
          _memManSizeMb(1000),
          _memManLocation("/qserv/data/mysql"),
          _threadPoolSize(wsched::BlendScheduler::getMinPoolSize()),
          _maxPoolThreads(5000),
          _maxGroupSize(1),
          _requiredTasksCompleted(25),
          _prioritySlow(2),
          _prioritySnail(1),
          _priorityMed(3),
          _priorityFast(4),
          _maxReserveSlow(2),
          _maxReserveSnail(2),
          _maxReserveMed(2),
          _maxReserveFast(2),
          _maxActiveChunksSlow(2),
          _maxActiveChunksSnail(1),
          _maxActiveChunksMed(4),
          _maxActiveChunksFast(4),
          _scanMaxMinutesFast(60),
          _scanMaxMinutesMed(60 * 8),
          _scanMaxMinutesSlow(60 * 12),
          _scanMaxMinutesSnail(60 * 24),
          _maxTasksBootedPerUserQuery(5),
          _maxConcurrentBootedTasks(25),
          _maxSqlConnections(800),
          _ReservedInteractiveSqlConnections(50),
          _bufferMaxTotalGB(41),
          _maxTransmits(40),
          _maxPerQid(3),
          _resultsDirname("/qserv/data/results"),
          _resultsXrootdPort(1094),
          _resultsNumHttpThreads(1),
          _resultDeliveryProtocol(ResultDeliveryProtocol::HTTP),
          _resultsCleanUpOnStart(true),
          _replicationInstanceId(""),
          _replicationAuthKey(""),
          _replicationAdminAuthKey(""),
          _replicationRegistryHost("localhost"),
          _replicationRegistryPort(8080),
          _replicationRegistryHearbeatIvalSec(1),
          _replicationHttpPort(0),
          _replicationNumHttpThreads(2) {
    // Both collections are the same since we don't have any external configuration
    // source passed into this c-tor.
    _populateJsonConfig("input");
    _populateJsonConfig("actual");
}

WorkerConfig::WorkerConfig(const util::ConfigStore& configStore)
        : _jsonConfig(nlohmann::json::object(
                  {{"input", configStore.toJson()}, {"actual", nlohmann::json::object()}})),
          _memManClass(configStore.get("memman.class", "MemManReal")),
          _memManSizeMb(configStore.getInt("memman.memory", 1000)),
          _memManLocation(configStore.getRequired("memman.location")),
          _threadPoolSize(
                  configStore.getInt("scheduler.thread_pool_size", wsched::BlendScheduler::getMinPoolSize())),
          _maxPoolThreads(configStore.getInt("scheduler.max_pool_threads", 5000)),
          _maxGroupSize(configStore.getInt("scheduler.group_size", 1)),
          _requiredTasksCompleted(configStore.getInt("scheduler.required_tasks_completed", 25)),
          _prioritySlow(configStore.getInt("scheduler.priority_slow", 2)),
          _prioritySnail(configStore.getInt("scheduler.priority_snail", 1)),
          _priorityMed(configStore.getInt("scheduler.priority_med", 3)),
          _priorityFast(configStore.getInt("scheduler.priority_fast", 4)),
          _maxReserveSlow(configStore.getInt("scheduler.reserve_slow", 2)),
          _maxReserveSnail(configStore.getInt("scheduler.reserve_snail", 2)),
          _maxReserveMed(configStore.getInt("scheduler.reserve_med", 2)),
          _maxReserveFast(configStore.getInt("scheduler.reserve_fast", 2)),
          _maxActiveChunksSlow(configStore.getInt("scheduler.maxactivechunks_slow", 2)),
          _maxActiveChunksSnail(configStore.getInt("scheduler.maxactivechunks_snail", 1)),
          _maxActiveChunksMed(configStore.getInt("scheduler.maxactivechunks_med", 4)),
          _maxActiveChunksFast(configStore.getInt("scheduler.maxactivechunks_fast", 4)),
          _scanMaxMinutesFast(configStore.getInt("scheduler.scanmaxminutes_fast", 60)),
          _scanMaxMinutesMed(configStore.getInt("scheduler.scanmaxminutes_med", 60 * 8)),
          _scanMaxMinutesSlow(configStore.getInt("scheduler.scanmaxminutes_slow", 60 * 12)),
          _scanMaxMinutesSnail(configStore.getInt("scheduler.scanmaxminutes_snail", 60 * 24)),
          _maxTasksBootedPerUserQuery(configStore.getInt("scheduler.maxtasksbootedperuserquery", 5)),
          _maxConcurrentBootedTasks(configStore.getInt("scheduler.maxconcurrentbootedtasks", 25)),
          _maxSqlConnections(configStore.getInt("sqlconnections.maxsqlconn", 800)),
          _ReservedInteractiveSqlConnections(
                  configStore.getInt("sqlconnections.reservedinteractivesqlconn", 50)),
          _bufferMaxTotalGB(configStore.getInt("transmit.buffermaxtotalgb", 41)),
          _maxTransmits(configStore.getInt("transmit.maxtransmits", 40)),
          _maxPerQid(configStore.getInt("transmit.maxperqid", 3)),
          _resultsDirname(configStore.get("results.dirname", "/qserv/data/results")),
          _resultsXrootdPort(configStore.getInt("results.xrootd_port", 1094)),
          _resultsNumHttpThreads(configStore.getInt("results.num_http_threads", 1)),
          _resultDeliveryProtocol(::parseResultDeliveryProtocol(configStore.get("results.protocol", "HTTP"))),
          _resultsCleanUpOnStart(configStore.getInt("results.clean_up_on_start", 1) != 0),
          _replicationInstanceId(configStore.get("replication.instance_id", "")),
          _replicationAuthKey(configStore.get("replication.auth_key", "")),
          _replicationAdminAuthKey(configStore.get("replication.admin_auth_key", "")),
          _replicationRegistryHost(configStore.get("replication.registry_host", "")),
          _replicationRegistryPort(configStore.getInt("replication.registry_port", 0)),
          _replicationRegistryHearbeatIvalSec(
                  configStore.getInt("replication.registry_heartbeat_ival_sec", 1)),
          _replicationHttpPort(configStore.getInt("replication.http_port", 0)),
          _replicationNumHttpThreads(configStore.getInt("replication.num_http_threads", 2)) {
    int mysqlPort = configStore.getInt("mysql.port");
    std::string mysqlSocket = configStore.get("mysql.socket");
    if (mysqlPort == 0 && mysqlSocket.empty()) {
        throw std::runtime_error(
                "At least one of mysql.port or mysql.socket is required in the configuration file.");
    }
    _mySqlConfig =
            mysql::MySqlConfig(configStore.getRequired("mysql.username"), configStore.get("mysql.password"),
                               configStore.getRequired("mysql.hostname"), mysqlPort, mysqlSocket,
                               "");  // dbname

    if (_replicationRegistryHost.empty()) {
        throw std::invalid_argument("WorkerConfig::" + std::string(__func__) +
                                    ": 'replication.registry_host' is not set.");
    }
    if (_replicationRegistryPort == 0) {
        throw std::invalid_argument("WorkerConfig::" + std::string(__func__) +
                                    ": 'replication.registry_port' number can't be 0.");
    }
    if (_replicationRegistryHearbeatIvalSec == 0) {
        throw std::invalid_argument("WorkerConfig::" + std::string(__func__) +
                                    ": 'replication.registry_heartbeat_ival_sec' can't be 0.");
    }
    if (_replicationNumHttpThreads == 0) {
        throw std::invalid_argument("WorkerConfig::" + std::string(__func__) +
                                    ": 'replication.num_http_threads' can't be 0.");
    }

    // Note that actual collection may contain parameters not mentioned in
    // the input configuration.
    _populateJsonConfig("actual");
}

void WorkerConfig::setReplicationHttpPort(uint16_t port) {
    if (port == 0) {
        throw std::invalid_argument("WorkerConfig::" + std::string(__func__) + ": port number can't be 0.");
    }
    _replicationHttpPort = port;
    // Update the relevant section of the JSON-ified configuration.
    _jsonConfig["actual"]["replication"]["http_port"] = std::to_string(_replicationHttpPort);
}

void WorkerConfig::_populateJsonConfig(std::string const& coll) {
    nlohmann::json& jsonConfigCollection = _jsonConfig[coll];
    jsonConfigCollection["memman"] = nlohmann::json::object({{"class", _memManClass},
                                                             {"memory", std::to_string(_memManSizeMb)},
                                                             {"location", _memManLocation}});
    jsonConfigCollection["scheduler"] = nlohmann::json::object(
            {{"thread_pool_size", std::to_string(_threadPoolSize)},
             {"max_pool_threads", std::to_string(_maxPoolThreads)},
             {"group_size", std::to_string(_maxGroupSize)},
             {"required_tasks_completed", std::to_string(_requiredTasksCompleted)},
             {"priority_slow", std::to_string(_prioritySlow)},
             {"priority_snail", std::to_string(_prioritySnail)},
             {"priority_med", std::to_string(_priorityMed)},
             {"priority_fast", std::to_string(_priorityFast)},
             {"reserve_slow", std::to_string(_maxReserveSlow)},
             {"reserve_snail", std::to_string(_maxReserveSnail)},
             {"reserve_med", std::to_string(_maxReserveMed)},
             {"reserve_fast", std::to_string(_maxReserveFast)},
             {"maxactivechunks_slow", std::to_string(_maxActiveChunksSlow)},
             {"maxactivechunks_snail", std::to_string(_maxActiveChunksSnail)},
             {"maxactivechunks_med", std::to_string(_maxActiveChunksMed)},
             {"maxactivechunks_fast", std::to_string(_maxActiveChunksFast)},
             {"scanmaxminutes_fast", std::to_string(_scanMaxMinutesFast)},
             {"scanmaxminutes_med", std::to_string(_scanMaxMinutesMed)},
             {"scanmaxminutes_slow", std::to_string(_scanMaxMinutesSlow)},
             {"scanmaxminutes_snail", std::to_string(_scanMaxMinutesSnail)},
             {"maxtasksbootedperuserquery", std::to_string(_maxTasksBootedPerUserQuery)}});
    jsonConfigCollection["sqlconnections"] = nlohmann::json::object(
            {{"maxsqlconn", std::to_string(_maxSqlConnections)},
             {"reservedinteractivesqlconn", std::to_string(_ReservedInteractiveSqlConnections)}});
    jsonConfigCollection["transmit"] =
            nlohmann::json::object({{"buffermaxtotalgb", std::to_string(_bufferMaxTotalGB)},
                                    {"maxtransmits", std::to_string(_maxTransmits)},
                                    {"maxperqid", std::to_string(_maxPerQid)}});
    jsonConfigCollection["results"] =
            nlohmann::json::object({{"dirname", _resultsDirname},
                                    {"xrootd_port", std::to_string(_resultsXrootdPort)},
                                    {"num_http_threads", std::to_string(_resultsNumHttpThreads)},
                                    {"protocol", WorkerConfig::protocol2str(_resultDeliveryProtocol)},
                                    {"clean_up_on_start", _resultsCleanUpOnStart ? "1" : "0"}});
    jsonConfigCollection["mysql"] = nlohmann::json::object({{"username", _mySqlConfig.username},
                                                            {"password", "xxxxx"},
                                                            {"hostname", _mySqlConfig.hostname},
                                                            {"port", std::to_string(_mySqlConfig.port)},
                                                            {"socket", _mySqlConfig.socket},
                                                            {"db", _mySqlConfig.dbName}});
    jsonConfigCollection["replication"] = nlohmann::json::object(
            {{"instance_id", _replicationInstanceId},
             {"auth_key", "xxxxx"},
             {"admin_auth_key", "xxxxx"},
             {"registry_host", _replicationRegistryHost},
             {"registry_port", std::to_string(_replicationRegistryPort)},
             {"registry_heartbeat_ival_sec", std::to_string(_replicationRegistryHearbeatIvalSec)},
             {"http_port", std::to_string(_replicationHttpPort)},
             {"num_http_threads", std::to_string(_replicationNumHttpThreads)}});
}

std::ostream& operator<<(std::ostream& out, WorkerConfig const& workerConfig) {
    out << workerConfig._jsonConfig.dump();
    return out;
}

}  // namespace lsst::qserv::wconfig
