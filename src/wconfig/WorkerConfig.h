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

/// This class handles the special case for the configuration value representing
/// the communications protocol used which can have a text value of "HTTP" or "
/// "XROOTD", case-insenitive.
class ConfigValResultDeliveryProtocol : public util::ConfigVal {
public:
    using CvrdpPtr = std::shared_ptr<ConfigValResultDeliveryProtocol>;
    enum TEnum {
        HTTP = 0,  ///< Use HTTP protocol
        XROOT = 1  ///< Use XROOTD file protocol
    };

    ConfigValResultDeliveryProtocol() = delete;
    virtual ~ConfigValResultDeliveryProtocol() = default;

    static CvrdpPtr create(util::ConfigValMap& configValMap, std::string const& section,
                           std::string const& name, bool required, std::string const& defVal,
                           bool hidden = false) {
        auto newPtr = CvrdpPtr(new ConfigValResultDeliveryProtocol(section, name, required, defVal, hidden));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    /// Return the appropriate TEnum for the given `str`, where "" returns HTTP.
    /// @throws ConfigException
    static TEnum parse(std::string const& str);

    /// Convert the TEnum `protocol` to the appropriate string.
    static std::string toString(TEnum protocol);

    /// Return the string value of this object.
    std::string getValStrDanger() const override { return toString(_val); }

    /// Return the string default value of this object.
    std::string getDefValStrDanger() const override { return toString(_defVal); }

    void setValFromConfigStoreChild(util::ConfigStore const& configStore) override;
    TEnum getVal() const { return _val; }

    void setVal(TEnum val) {
        _val = val;
        logValSet();
    }

private:
    ConfigValResultDeliveryProtocol(std::string const& section, std::string const& name, bool required,
                                    std::string const& defVal, bool hidden)
            : ConfigVal(section, name, required, hidden), _defVal(parse(defVal)), _val(_defVal) {}
    TEnum const _defVal;  ///< Default value for the item this class is storing.
    TEnum _val;           ///< Value for the item this class is storing.
};

/// Provide all configuration parameters for a Qserv worker instance.
/// Parse an INI configuration file, identify required parameters and ignore
/// others, analyze and store them inside private member variables, use default
/// values for missing parameters, provide accessor for each of these variable.
/// This class hides configuration complexity
/// from other part of the code. All private member variables are related to INI
/// parameters and are immutables.
///
/// @note the class has a thread-safe API.
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

    /// @return Prioritize by number of inFLight tasks per scheduler.
    bool getPrioritizeByInFlight() const { return _prioritizeByInFlight->getVal(); }

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

    /// @return the name of a folder where query results will be stored
    std::string const resultsDirname() const { return _resultsDirname->getVal(); }

    /// @return the port number of the worker XROOTD service for serving result files
    uint16_t resultsXrootdPort() const { return _resultsXrootdPort->getVal(); }

    /// The size
    int getQPoolSize() const { return _qPoolSize->getVal(); }

    /// The highest priority number, such as 2, which results
    /// in queues for priorities 0, 1, 2, and 100; where 0 is the
    /// highest priority.
    /// @see util::QdispPool
    int getQPoolMaxPriority() const { return _qPoolMaxPriority->getVal(); }

    /// The maximum number of running threads at each priority,
    /// "30:20:20:10" with _qPoolMaxPriority=2 allows 30 threads
    /// at priority 0, 20 threads at priorities 1+2, and 10 threads
    /// at priority 100.
    /// @see util::QdispPool
    std::string getQPoolRunSizes() const { return _qPoolRunSizes->getVal(); }

    /// The minimum number of running threads per priority,
    /// "3:3:3:3" with _qPoolMaxPriority=2 means that a thread at priority
    /// 0 would not start if it meant that there would not be enough threads
    /// left to have running for each of priorities 1, 2, and 100.
    /// @see util::QdispPool
    std::string getQPoolMinRunningSizes() const { return _qPoolMinRunningSizes->getVal(); }

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

    /// The number of seconds a czar needs to be incommunicado before being considered
    /// dead by a worker.
    unsigned int getCzarDeadTimeSec() const { return _czarDeadTimeSec->getVal(); }

    /// Return the number of threads HttpSvc use for communicating with the czar.
    unsigned int getCzarComNumHttpThreads() const { return _czarComNumHttpThreads->getVal(); }

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
    /// @param useDefault Set to true to populate the collection with default values instead
    ///        of actual values.
    void _populateJsonConfig(std::string const& coll, bool useDefault = false);

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
    bool const hidden = true;

    CVTStrPtr _memManClass =
            util::ConfigValTStr::create(_configValMap, "memman", "class", notReq, "MemManReal");
    CVTUIntPtr _memManSizeMb = util::ConfigValTUInt::create(_configValMap, "memman", "memory", notReq, 1000);
    CVTStrPtr _memManLocation =
            util::ConfigValTStr::create(_configValMap, "memman", "location", required, "/qserv/data/mysql");
    CVTUIntPtr _threadPoolSize =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "thread_pool_size", notReq, 0);
    CVTUIntPtr _maxPoolThreads =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "max_pool_threads", notReq, 5000);
    CVTUIntPtr _maxGroupSize =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "group_size", notReq, 1);
    CVTUIntPtr _requiredTasksCompleted =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "required_tasks_completed", notReq, 25);
    CVTUIntPtr _prioritySlow =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "priority_slow", notReq, 2);
    CVTUIntPtr _prioritySnail =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "priority_snail", notReq, 1);
    CVTUIntPtr _priorityMed =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "priority_med", notReq, 3);
    CVTUIntPtr _priorityFast =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "priority_fast", notReq, 4);
    CVTBoolPtr _prioritizeByInFlight =
            util::ConfigValTBool::create(_configValMap, "results", "prioritize_by_inflight", notReq, false);

    CVTUIntPtr _maxReserveSlow =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "reserve_slow", notReq, 2);
    CVTUIntPtr _maxReserveSnail =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "reserve_snail", notReq, 2);
    CVTUIntPtr _maxReserveMed =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "reserve_med", notReq, 2);
    CVTUIntPtr _maxReserveFast =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "reserve_fast", notReq, 2);
    CVTUIntPtr _maxActiveChunksSlow =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "maxactivechunks_slow", notReq, 2);
    CVTUIntPtr _maxActiveChunksSnail =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "maxactivechunks_snail", notReq, 1);
    CVTUIntPtr _maxActiveChunksMed =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "maxactivechunks_med", notReq, 4);
    CVTUIntPtr _maxActiveChunksFast =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "maxactivechunks_fast", notReq, 4);
    CVTUIntPtr _scanMaxMinutesFast =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "scanmaxminutes_fast", notReq, 60);
    CVTUIntPtr _scanMaxMinutesMed =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "scanmaxminutes_med", notReq, 60 * 8);
    CVTUIntPtr _scanMaxMinutesSlow =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "scanmaxminutes_slow", notReq, 60 * 12);
    CVTUIntPtr _scanMaxMinutesSnail =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "scanmaxminutes_snail", notReq, 60 * 24);
    CVTUIntPtr _maxTasksBootedPerUserQuery =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "maxtasksbootedperuserquery", notReq, 5);
    CVTUIntPtr _maxConcurrentBootedTasks =
            util::ConfigValTUInt::create(_configValMap, "scheduler", "maxconcurrentbootedtasks", notReq, 25);
    CVTUIntPtr _maxSqlConnections =
            util::ConfigValTUInt::create(_configValMap, "sqlconnections", "maxsqlconn", notReq, 800);
    CVTUIntPtr _ReservedInteractiveSqlConnections = util::ConfigValTUInt::create(
            _configValMap, "sqlconnections", "reservedinteractivesqlconn", notReq, 50);
    CVTStrPtr _resultsDirname =
            util::ConfigValTStr::create(_configValMap, "results", "dirname", notReq, "/qserv/data/results");
    CVTUIntPtr _resultsXrootdPort =
            util::ConfigValTUInt::create(_configValMap, "results", "xrootd_port", notReq, 1094);
    CVTUIntPtr _resultsNumHttpThreads =
            util::ConfigValTUInt::create(_configValMap, "results", "num_http_threads", notReq, 1);
    ConfigValResultDeliveryProtocol::CvrdpPtr _resultDeliveryProtocol =
            ConfigValResultDeliveryProtocol::create(_configValMap, "results", "protocol", notReq, "HTTP");
    CVTBoolPtr _resultsCleanUpOnStart =
            util::ConfigValTBool::create(_configValMap, "results", "clean_up_on_start", notReq, true);

    CVTStrPtr _replicationInstanceId =
            util::ConfigValTStr::create(_configValMap, "replication", "instance_id", notReq, "");
    CVTStrPtr _replicationAuthKey =
            util::ConfigValTStr::create(_configValMap, "replication", "auth_key", notReq, "", hidden);
    CVTStrPtr _replicationAdminAuthKey =
            util::ConfigValTStr::create(_configValMap, "replication", "admin_auth_key", notReq, "", hidden);
    CVTStrPtr _replicationRegistryHost =
            util::ConfigValTStr::create(_configValMap, "replication", "registry_host", required, "");
    CVTUIntPtr _replicationRegistryPort =
            util::ConfigValTUInt::create(_configValMap, "replication", "registry_port", required, 0);
    CVTUIntPtr _replicationRegistryHearbeatIvalSec = util::ConfigValTUInt::create(
            _configValMap, "replication", "registry_heartbeat_ival_sec", notReq, 1);
    CVTUIntPtr _replicationHttpPort =
            util::ConfigValTUInt::create(_configValMap, "replication", "http_port", required, 0);
    CVTUIntPtr _replicationNumHttpThreads =
            util::ConfigValTUInt::create(_configValMap, "replication", "num_http_threads", notReq, 20);

    CVTUIntPtr _mysqlPort = util::ConfigValTUInt::create(_configValMap, "mysql", "port", notReq, 4048);
    CVTStrPtr _mysqlSocket = util::ConfigValTStr::create(_configValMap, "mysql", "socket", notReq, "");
    CVTStrPtr _mysqlUsername =
            util::ConfigValTStr::create(_configValMap, "mysql", "username", required, "qsmaster");
    CVTStrPtr _mysqlPassword = util::ConfigValTStr::create(_configValMap, "mysql", "password", required,
                                                           "not_the_password", hidden);
    CVTStrPtr _mysqlHostname =
            util::ConfigValTStr::create(_configValMap, "mysql", "hostname", required, "none");
    CVTStrPtr _mysqlDb = util::ConfigValTStr::create(_configValMap, "mysql", "db", notReq, "");

    CVTIntPtr _qPoolSize = util::ConfigValTInt::create(_configValMap, "qpool", "Size", notReq, 50);
    CVTIntPtr _qPoolMaxPriority =
            util::ConfigValTInt::create(_configValMap, "qpool", "MaxPriority", notReq, 2);
    CVTStrPtr _qPoolRunSizes =
            util::ConfigValTStr::create(_configValMap, "qpool", "RunSizes", notReq, "50:20:10");
    CVTStrPtr _qPoolMinRunningSizes =
            util::ConfigValTStr::create(_configValMap, "qpool", "MinRunningSizes", notReq, "3:3:3");
    CVTUIntPtr _czarDeadTimeSec =
            util::ConfigValTUInt::create(_configValMap, "czar", "DeadTimeSec", notReq, 180);
    CVTUIntPtr _czarComNumHttpThreads =
            util::ConfigValTUInt::create(_configValMap, "czar", "ComNumHttpThreads", notReq, 40);
};

}  // namespace lsst::qserv::wconfig

#endif  // LSST_QSERV_WCONFIG_WORKERCONFIG_H
