// -*- LSST-C++ -*-
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

#ifndef LSST_QSERV_WCONFIG_WORKERCONFIG_H
#define LSST_QSERV_WCONFIG_WORKERCONFIG_H

// System headers
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>

// Third party headers
#include <nlohmann/json.hpp>

// Qserv headers
#include "mysql/MySqlConfig.h"
#include "util/ConfigStore.h"
#include "util/ConfigValMap.h"
#include "util/Issue.h"

namespace lsst::qserv::wconfig {

class ConfigValResultDeliveryProtocol : public util::ConfigVal {
public:
    using CvrdpPtr = std::shared_ptr<ConfigValResultDeliveryProtocol>;
    enum TEnum {
        HTTP = 0,  ///< Use HTTP protocol
        XROOT = 1  ///< Use XROOTD file protocol
    };

    ConfigValResultDeliveryProtocol() = delete;
    virtual ~ConfigValResultDeliveryProtocol() = default;

    static CvrdpPtr create(std::string const& section, std::string const& name, bool required,
                           std::string const& defVal, util::ConfigValMap& configValMap) {
        auto newPtr = CvrdpPtr(new ConfigValResultDeliveryProtocol(section, name, required, defVal));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    /// &&& doc
    /// @throws ConfigException
    static TEnum parse(std::string const& str);

    /// &&& doc
    static std::string toString(TEnum protocol);

    /// Return the string value of non-hidden config values. (setH
    std::string getValStrDanger() const override { return toString(_val); }

    void setValFromConfigStoreChild(util::ConfigStore const& configStore) override;
    TEnum getVal() const { return _val; }
    TEnum getDefVal() const { return _defVal; }

    void setVal(TEnum val) {
        _val = val;
        logValSet();
    }

private:
    ConfigValResultDeliveryProtocol(std::string const& section, std::string const& name, bool required,
                                    std::string const& defVal)
            : ConfigVal(section, name, required), _val(parse(defVal)), _defVal(_val) {}
    TEnum _val;
    TEnum _defVal;
};

/**
 * Provide all configuration parameters for a Qserv worker instance.
 * Parse an INI configuration file, identify required parameters and ignore
 * others, analyze and store them inside private member variables, use default
 * values for missing parameters, provide accessor for each of these variable.
 * This class hides configuration complexity
 * from other part of the code. All private member variables are related to INI
 * parameters and are immutables.
 *
 * @note the class has a thread-safe API.
 */

class WorkerConfig {
public:
    /// Create an instance of WorkerConfig and if a configuration file is provided then
    /// load parameters from the file. Otherwise create an object with default values
    /// of the parameters.
    /// @note One has to call this method at least once before trying to obtain
    ///  a pointer of the instance by calling 'instnce()'. The method 'create()'
    ///  can be called many times. A new instance would be created each time and
    ///  stored witin the class.
    /// @param configFileName - (optional) path to worker INI configuration file
    /// @return the shared pointer to the configuration object
    static std::shared_ptr<WorkerConfig> create(std::string const& configFileName = std::string());

    /// Get a pointer to an instance that was created by a last call to
    /// the method 'create'.
    /// @return the shared pointer to the configuration object
    /// @throws std::logic_error when attempting to call the bethod before creating an instance.
    static std::shared_ptr<WorkerConfig> instance();

    WorkerConfig(WorkerConfig const&) = delete;
    WorkerConfig& operator=(WorkerConfig const&) = delete;

    /// @return thread pool size for shared scans
    unsigned int getThreadPoolSize() const { return _threadPoolSize->getVal(); }

    /// @return maximum number of threads the pool can have in existence at any given time
    unsigned int getMaxPoolThreads() const { return _maxPoolThreads->getVal(); }

    /// @return required number of tasks for table in a chunk for the average to be valid
    unsigned int getRequiredTasksCompleted() const { return _requiredTasksCompleted->getVal(); }

    /// @return maximum number of tasks that can be booted from a single user query
    unsigned int getMaxTasksBootedPerUserQuery() const { return _maxTasksBootedPerUserQuery->getVal(); }

    /// @return maximum number of tasks that can be booted from a single user query
    unsigned int getMaxConcurrentBootedTasks() const { return _maxConcurrentBootedTasks->getVal(); }

    /// @return maximum time for a user query to complete  all tasks on the fast scan
    unsigned int getScanMaxMinutesFast() const { return _scanMaxMinutesFast->getVal(); }

    /// @return maximum time for a user query to complete all tasks on the medium scan
    unsigned int getScanMaxMinutesMed() const { return _scanMaxMinutesMed->getVal(); }

    /// @return maximum time for a user query to complete all tasks on the slow scan
    unsigned int getScanMaxMinutesSlow() const { return _scanMaxMinutesSlow->getVal(); }

    /// @return maximum time for a user query to complete all tasks on the snail scan
    unsigned int getScanMaxMinutesSnail() const { return _scanMaxMinutesSnail->getVal(); }

    /// @return maximum number of task accepted in a group queue
    unsigned int getMaxGroupSize() const { return _maxGroupSize->getVal(); }

    /// @return max thread reserve for fast shared scan
    unsigned int getMaxReserveFast() const { return _maxReserveFast->getVal(); }

    /// @return max thread reserve for medium shared scan
    unsigned int getMaxReserveMed() const { return _maxReserveMed->getVal(); }

    /// @return max thread reserve for slow shared scan
    unsigned int getMaxReserveSlow() const { return _maxReserveSlow->getVal(); }

    /// @return max thread reserve for snail shared scan
    unsigned int getMaxReserveSnail() const { return _maxReserveSnail->getVal(); }

    /// @return class name implementing selected memory management
    std::string const getMemManClass() const { return _memManClass->getVal(); }

    /// @return path to directory where the Memory Manager database resides
    std::string const getMemManLocation() const { return _memManLocation->getVal(); }

    /// @return maximum amount of memory that can be used by Memory Manager
    uint64_t getMemManSizeMb() const { return _memManSizeMb->getVal(); }

    /// @return a configuration for worker MySQL instance.
    mysql::MySqlConfig const& getMySqlConfig() const { return _mySqlConfig; }

    /// @return fast shared scan priority
    unsigned int getPriorityFast() const { return _priorityFast->getVal(); }

    /// @return medium shared scan priority
    unsigned int getPriorityMed() const { return _priorityMed->getVal(); }

    /// @return slow shared scan priority
    unsigned int getPrioritySlow() const { return _prioritySlow->getVal(); }

    /// @return slow shared scan priority
    unsigned int getPrioritySnail() const { return _prioritySnail->getVal(); }

    /// @return maximum concurrent chunks for fast shared scan
    unsigned int getMaxActiveChunksFast() const { return _maxActiveChunksFast->getVal(); }

    /// @return maximum concurrent chunks for medium shared scan
    unsigned int getMaxActiveChunksMed() const { return _maxActiveChunksMed->getVal(); }

    /// @return maximum concurrent chunks for slow shared scan
    unsigned int getMaxActiveChunksSlow() const { return _maxActiveChunksSlow->getVal(); }

    /// @return maximum concurrent chunks for snail shared scan
    unsigned int getMaxActiveChunksSnail() const { return _maxActiveChunksSnail->getVal(); }

    /// @return the maximum number of SQL connections for tasks
    unsigned int getMaxSqlConnections() const { return _maxSqlConnections->getVal(); }

    /// @return the number of SQL connections reserved for interactive tasks
    unsigned int getReservedInteractiveSqlConnections() const {
        return _ReservedInteractiveSqlConnections->getVal();
    }

    /// @return the maximum number of gigabytes that can be used by StreamBuffers
    unsigned int getBufferMaxTotalGB() const { return _bufferMaxTotalGB->getVal(); }

    /// @return the maximum number of concurrent transmits to a czar
    unsigned int getMaxTransmits() const { return _maxTransmits->getVal(); }

    int getMaxPerQid() const { return _maxPerQid->getVal(); }

    /// @return the name of a folder where query results will be stored
    std::string const resultsDirname() const { return _resultsDirname->getVal(); }

    /// @return the port number of the worker XROOTD service for serving result files
    uint16_t resultsXrootdPort() const { return _resultsXrootdPort->getVal(); }

    /// @return the number of the BOOST ASIO threads for servicing HTGTP requests
    size_t resultsNumHttpThreads() const { return _resultsNumHttpThreads->getVal(); }

    /// @return the result delivery method
    ConfigValResultDeliveryProtocol::TEnum resultDeliveryProtocol() const {
        return _resultDeliveryProtocol->getVal();
    }

    /// @return 'true' if result files (if any) left after the previous run of the worker
    /// had to be deleted from the corresponding folder.
    bool resultsCleanUpOnStart() const { return _resultsCleanUpOnStart->getVal(); }

    // Parameters of the worker management service

    std::string const& replicationInstanceId() const { return _replicationInstanceId->getVal(); }
    std::string const& replicationAuthKey() const { return _replicationAuthKey->getVal(); }
    std::string const& replicationAdminAuthKey() const { return _replicationAdminAuthKey->getVal(); }
    std::string const& replicationRegistryHost() const { return _replicationRegistryHost->getVal(); }
    uint16_t replicationRegistryPort() const { return _replicationRegistryPort->getVal(); }
    unsigned int replicationRegistryHearbeatIvalSec() const {
        return _replicationRegistryHearbeatIvalSec->getVal();
    }
    uint16_t replicationHttpPort() const { return _replicationHttpPort->getVal(); }
    size_t replicationNumHttpThreads() const { return _replicationNumHttpThreads->getVal(); }

    /// The actual port number is set at run time after starting the service on
    /// the dynamically allocated port (in case when the port number was set
    /// to 0 in the initial configuration).
    /// @param port The actual port number.
    void setReplicationHttpPort(uint16_t port);

    /// @return the JSON representation of the configuration parameters.
    /// @note The object has two collections of the parameters: 'input' - for
    /// parameters that were proided to the construction of the class, and
    /// 'actual' - for parameters that were expected (and set in the transient
    /// state). These collection may not be the same. For example, some older
    /// parameters may be phased out while still being present in the configuration
    /// files. Or, new actual parameters (with some reasonable defaults) might be
    /// introduced while not being set in the configuration file.
    nlohmann::json toJson() const { return _jsonConfig; }

    /**
     * Dump the configuration object onto the output stream.
     * @param out - the output stream object
     * @param workerConfig - worker configuration object
     * @return the output stream object
     */
    friend std::ostream& operator<<(std::ostream& out, WorkerConfig const& workerConfig);

private:
    /// Initialize parameters with default values
    WorkerConfig();

    /// Initialize parameters from the configuration store
    /// @param configStore
    WorkerConfig(util::ConfigStore const& configStore);

    /// This method is called by both c-tors to populate the JSON configuration with actual
    /// parameters of the object.
    /// @param coll The name of a collection to be populated.
    void _populateJsonConfig(std::string const& coll);

    /// This mutex protects the static member _instance.
    static std::mutex _mtxOnInstance;

    /// The configuratoon object created by the last call to the method 'test'.
    static std::shared_ptr<WorkerConfig> _instance;

    nlohmann::json _jsonConfig;  ///< JSON-ified configuration

    mysql::MySqlConfig _mySqlConfig;

    util::ConfigValMap _configValMap;  ///< Map of all configuration entries

    using CVTIntPtr = util::ConfigValTInt::IntPtr const;
    using CVTUIntPtr = util::ConfigValTUInt::UIntPtr const;
    using CVTBoolPtr = util::ConfigValTBool::BoolPtr const;
    using CVTStrPtr = util::ConfigValTStr::StrPtr const;

    bool const required = true;
    bool const notReq = false;

    CVTStrPtr _memManClass =
            util::ConfigValTStr::create("memman", "class", notReq, "MemManReal", _configValMap);
    CVTUIntPtr _memManSizeMb = util::ConfigValTUInt::create("memman", "memory", notReq, 1000, _configValMap);
    CVTStrPtr _memManLocation =
            util::ConfigValTStr::create("memman", "location", required, "/qserv/data/mysql", _configValMap);
    CVTUIntPtr _threadPoolSize =
            util::ConfigValTUInt::create("scheduler", "thread_pool_size", notReq, 0, _configValMap);
    CVTUIntPtr _maxPoolThreads =
            util::ConfigValTUInt::create("scheduler", "max_pool_threads", notReq, 5000, _configValMap);
    CVTUIntPtr _maxGroupSize =
            util::ConfigValTUInt::create("scheduler", "group_size", notReq, 1, _configValMap);
    CVTUIntPtr _requiredTasksCompleted =
            util::ConfigValTUInt::create("scheduler", "required_tasks_completed", notReq, 25, _configValMap);
    CVTUIntPtr _prioritySlow =
            util::ConfigValTUInt::create("scheduler", "priority_slow", notReq, 2, _configValMap);
    CVTUIntPtr _prioritySnail =
            util::ConfigValTUInt::create("scheduler", "priority_snail", notReq, 1, _configValMap);
    CVTUIntPtr _priorityMed =
            util::ConfigValTUInt::create("scheduler", "priority_med", notReq, 3, _configValMap);
    CVTUIntPtr _priorityFast =
            util::ConfigValTUInt::create("scheduler", "priority_fast", notReq, 4, _configValMap);
    CVTUIntPtr _maxReserveSlow =
            util::ConfigValTUInt::create("scheduler", "reserve_slow", notReq, 2, _configValMap);
    CVTUIntPtr _maxReserveSnail =
            util::ConfigValTUInt::create("scheduler", "reserve_snail", notReq, 2, _configValMap);
    CVTUIntPtr _maxReserveMed =
            util::ConfigValTUInt::create("scheduler", "reserve_med", notReq, 2, _configValMap);
    CVTUIntPtr _maxReserveFast =
            util::ConfigValTUInt::create("scheduler", "reserve_fast", notReq, 2, _configValMap);
    CVTUIntPtr _maxActiveChunksSlow =
            util::ConfigValTUInt::create("scheduler", "maxactivechunks_slow", notReq, 2, _configValMap);
    CVTUIntPtr _maxActiveChunksSnail =
            util::ConfigValTUInt::create("scheduler", "maxactivechunks_snail", notReq, 1, _configValMap);
    CVTUIntPtr _maxActiveChunksMed =
            util::ConfigValTUInt::create("scheduler", "maxactivechunks_med", notReq, 4, _configValMap);
    CVTUIntPtr _maxActiveChunksFast =
            util::ConfigValTUInt::create("scheduler", "maxactivechunks_fast", notReq, 4, _configValMap);
    CVTUIntPtr _scanMaxMinutesFast =
            util::ConfigValTUInt::create("scheduler", "scanmaxminutes_fast", notReq, 60, _configValMap);
    CVTUIntPtr _scanMaxMinutesMed =
            util::ConfigValTUInt::create("scheduler", "scanmaxminutes_med", notReq, 60 * 8, _configValMap);
    CVTUIntPtr _scanMaxMinutesSlow =
            util::ConfigValTUInt::create("scheduler", "scanmaxminutes_slow", notReq, 60 * 12, _configValMap);
    CVTUIntPtr _scanMaxMinutesSnail =
            util::ConfigValTUInt::create("scheduler", "scanmaxminutes_snail", notReq, 60 * 24, _configValMap);
    CVTUIntPtr _maxTasksBootedPerUserQuery =
            util::ConfigValTUInt::create("scheduler", "maxtasksbootedperuserquery", notReq, 5, _configValMap);
    CVTUIntPtr _maxConcurrentBootedTasks =
            util::ConfigValTUInt::create("scheduler", "maxconcurrentbootedtasks", notReq, 25, _configValMap);
    CVTUIntPtr _maxSqlConnections =
            util::ConfigValTUInt::create("sqlconnections", "maxsqlconn", notReq, 800, _configValMap);
    CVTUIntPtr _ReservedInteractiveSqlConnections = util::ConfigValTUInt::create(
            "sqlconnections", "reservedinteractivesqlconn", notReq, 50, _configValMap);
    CVTUIntPtr _bufferMaxTotalGB =
            util::ConfigValTUInt::create("transmit", "buffermaxtotalgb", notReq, 41, _configValMap);
    CVTUIntPtr _maxTransmits =
            util::ConfigValTUInt::create("transmit", "maxtransmits", notReq, 40, _configValMap);
    CVTIntPtr _maxPerQid = util::ConfigValTInt::create("transmit", "maxperqid", notReq, 3, _configValMap);
    CVTStrPtr _resultsDirname =
            util::ConfigValTStr::create("results", "dirname", notReq, "/qserv/data/results", _configValMap);
    CVTUIntPtr _resultsXrootdPort =
            util::ConfigValTUInt::create("results", "xrootd_port", notReq, 1094, _configValMap);
    CVTUIntPtr _resultsNumHttpThreads =
            util::ConfigValTUInt::create("results", "num_http_threads", notReq, 1, _configValMap);
    ConfigValResultDeliveryProtocol::CvrdpPtr _resultDeliveryProtocol =
            ConfigValResultDeliveryProtocol::create("results", "protocol", notReq, "HTTP", _configValMap);
    CVTBoolPtr _resultsCleanUpOnStart =
            util::ConfigValTBool::create("results", "clean_up_on_start", notReq, true, _configValMap);

    CVTStrPtr _replicationInstanceId =
            util::ConfigValTStr::create("replication", "instance_id", notReq, "", _configValMap);
    CVTStrPtr _replicationAuthKey =
            util::ConfigValTStr::create("replication", "auth_key", notReq, "", _configValMap);
    CVTStrPtr _replicationAdminAuthKey =
            util::ConfigValTStr::create("replication", "admin_auth_key", notReq, "", _configValMap);
    CVTStrPtr _replicationRegistryHost =
            util::ConfigValTStr::create("replication", "registry_host", required, "", _configValMap);
    CVTUIntPtr _replicationRegistryPort =
            util::ConfigValTUInt::create("replication", "registry_port", required, 0, _configValMap);
    CVTUIntPtr _replicationRegistryHearbeatIvalSec = util::ConfigValTUInt::create(
            "replication", "registry_heartbeat_ival_sec", notReq, 1, _configValMap);
    CVTUIntPtr _replicationHttpPort =
            util::ConfigValTUInt::create("replication", "http_port", required, 0, _configValMap);
    CVTUIntPtr _replicationNumHttpThreads =
            util::ConfigValTUInt::create("replication", "num_http_threads", notReq, 2, _configValMap);

    CVTUIntPtr _mysqlPort = util::ConfigValTUInt::create("mysql", "port", notReq, 4048, _configValMap);
    CVTStrPtr _mysqlSocket = util::ConfigValTStr::create("mysql", "socket", notReq, "", _configValMap);
    CVTStrPtr _mysqlUsername =
            util::ConfigValTStr::create("mysql", "username", required, "qsmaster", _configValMap);
    CVTStrPtr _mysqlPassword =
            util::ConfigValTStr::create("mysql", "password", required, "not_the_password", _configValMap);
    CVTStrPtr _mysqlHostname =
            util::ConfigValTStr::create("mysql", "hostname", required, "none", _configValMap);
    CVTStrPtr _mysqlDb = util::ConfigValTStr::create("mysql", "db", notReq, "", _configValMap);
};

}  // namespace lsst::qserv::wconfig

#endif  // LSST_QSERV_WCONFIG_WORKERCONFIG_H
