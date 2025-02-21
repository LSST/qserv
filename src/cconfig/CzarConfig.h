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

#ifndef LSST_QSERV_CCONFIG_CZARCONFIG_H
#define LSST_QSERV_CCONFIG_CZARCONFIG_H

// System headers
#include <map>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>

// Third party headers
#include <nlohmann/json.hpp>

// Qserv headers
#include "mysql/MySqlConfig.h"
#include "qmeta/types.h"
#include "util/ConfigStore.h"
#include "util/ConfigValMap.h"

namespace lsst::qserv::cconfig {

/**
 *  Provide all configuration parameters for a Qserv Czar instance
 *
 *  Parse an INI configuration file, identify required parameters and ignore
 *  others, analyze and store them inside private member variables, use default
 *  values for missing parameters, provide accessor for each of these variable.
 *  This class hide configuration complexity from other part of the code.
 *  All private member variables are related to Czar parameters and are immutables.
 *
 */
class CzarConfig {
public:
    using Ptr = std::shared_ptr<CzarConfig>;
    /**
     * Create an instance of CzarConfig and load parameters from the specifid file.
     * @note One has to call this method at least once before trying to obtain
     *   a pointer of the instance by calling 'instance()'. The method 'create()'
     *   can be called many times. A new instance would be created each time and
     *   stored within the class.
     * @param configFileName - path to worker INI configuration file
     * @param czarName - the unique name of Czar.
     * @return the shared pointer to the configuration object
     */
    static Ptr create(std::string const& configFileName, std::string const& czarName);

    /**
     * Get a pointer to an instance that was created by the last call to
     * the method 'create'.
     * @return the shared pointer to the configuration object
     * @throws std::logic_error when attempting to call the bethod before creating an instance.
     */
    static Ptr instance();

    CzarConfig() = delete;
    CzarConfig(CzarConfig const&) = delete;
    CzarConfig& operator=(CzarConfig const&) = delete;

    /** Overload output operator for current class
     *
     * @param out
     * @param czarConfig
     * @return an output stream
     */
    friend std::ostream& operator<<(std::ostream& out, CzarConfig const& czarConfig);

    /// Get MySQL configuration for czar MySQL result database.
    /// @return a structure containing MySQL parameters.
    mysql::MySqlConfig getMySqlResultConfig() const;

    /// Get MySQL configuration for czar MySQL qmeta database
    /// @return a structure containing MySQL parameters
    mysql::MySqlConfig getMySqlQmetaConfig() const;

    /// Get MySQL configuration for czar MySQL QStatusData
    /// @return a structure containing MySQL parameters
    mysql::MySqlConfig getMySqlQStatusDataConfig() const;

    /// Get CSS parameters as a collection of key-value
    /// Do not check CSS parameters consistency
    /// @return a structure containing CSS parameters
    std::map<std::string, std::string> getCssConfigMap() const;

    /* Get the maximum number of chunks that can be in an interactive query.
     * Queries that are not limited in area to a small number of chunks must
     * be part of a full table scan.
     */
    int getInteractiveChunkLimit() const { return _interactiveChunkLimit->getVal(); }

    bool getQueryDistributionTestVer() const { return _queryDistributionTestVer->getVal(); }

    /* Get minimum number of seconds between QMeta chunk completion updates.
     *
     * @return seconds between QMeta chunk completion updates.
     */
    int getQMetaSecondsBetweenChunkUpdates() const {
        return _qMetaSecsBetweenChunkCompletionUpdates->getVal();
    }

    int getMaxMsgSourceStore() const { return _maxMsgSourceStore->getVal(); }

    /// Getters for result aggregation options.
    int getMaxTableSizeMB() const { return _maxTableSizeMB->getVal(); }
    int getMaxSqlConnectionAttempts() const { return _maxSqlConnectionAttempts->getVal(); }
    std::string getResultEngine() const { return _resultEngine->getVal(); }
    int getResultMaxConnections() const { return _resultMaxConnections->getVal(); }

    /// The size of the TCP connection pool witin the client API that is used
    /// by the merger to pool result files from workers via the HTTP protocol.
    int getResultMaxHttpConnections() const { return _resultMaxHttpConnections->getVal(); }

    /// Getters for QdispPool configuration
    /// @return the number of threads to create for the pool.
    int getQdispPoolSize() const { return _qdispPoolSize->getVal(); }
    /// @return the maximum priority for a queue. The number of queues
    ///    equals this value +1.
    int getQdispMaxPriority() const { return _qdispMaxPriority->getVal(); }
    /// @return a string with substrings separated by ':' like "2:45:32:9"
    ///      The values indicate the maximum number of commands for each
    ///      priority that can be running concurrently.
    std::string getQdispVectRunSizes() const { return _qdispVectRunSizes->getVal(); }
    /// @return a string with substrings separated by ':' like "2:45:32:9"
    ///      The values indicate the minimum number of commands for each
    ///      priority that should be running concurrently
    std::string getQdispVectMinRunningSizes() const { return _qdispVectMinRunningSizes->getVal(); }

    int getOldestResultKeptDays() const { return _oldestResultKeptDays->getVal(); }

    /// @return 'true' to allow broadcasting query completion/cancellation events
    /// to all workers so that they would do proper garbage collection and resource recycling.
    bool notifyWorkersOnQueryFinish() const { return _notifyWorkersOnQueryFinish->getVal(); }

    /// @return 'true' to allow broadcasting this event to all workers to let them cancel
    /// any older that were submitted before the restart. The first query identifier in the new
    /// series will be reported to the workers. The identifier will be used as
    /// a high watermark for diffirentiating between the older (to be cancelled)
    /// and the newer queries.
    bool notifyWorkersOnCzarRestart() const { return _notifyWorkersOnCzarRestart->getVal(); }

    /// @return The desired sampling frequency of the Czar monitoring which is
    /// based on tracking state changes in various entities. If 0 is returned by
    /// the method then the monitoring will be disabled.
    unsigned int czarStatsUpdateIvalSec() const { return _czarStatsUpdateIvalSec->getVal(); }

    /// @return The maximum retain period for keeping in memory the relevant metrics
    /// captured by the Czar monitoring system. If 0 is returned by the method then
    /// query history archiving will be disabled.
    /// @note Setting the limit too high may be potentially result in runing onto
    /// the OOM situation.
    unsigned int czarStatsRetainPeriodSec() const { return _czarStatsRetainPeriodSec->getVal(); }

    /// A worker is considered fully ALIVE if the last update from the worker has been
    /// heard in less than _activeWorkerTimeoutAliveSecs seconds.
    int getActiveWorkerTimeoutAliveSecs() const { return _activeWorkerTimeoutAliveSecs->getVal(); }

    /// A worker is considered DEAD if it hasn't been heard from in more than
    /// _activeWorkerTimeoutDeadSecs.
    int getActiveWorkerTimeoutDeadSecs() const { return _activeWorkerTimeoutDeadSecs->getVal(); }

    /// Max lifetime of a message to be sent to an active worker. If the czar has been
    /// trying to send a message to a worker and has failed for this many seconds,
    /// it gives up at this point, removing elements of the message to save memory.
    int getActiveWorkerMaxLifetimeSecs() const { return _activeWorkerMaxLifetimeSecs->getVal(); }

    /// The maximum number of chunks (basically Jobs) allowed in a single UberJob.
    int getUberJobMaxChunks() const { return _uberJobMaxChunks->getVal(); }

    /// Return the maximum number of http connections to use for czar commands.
    int getCommandMaxHttpConnections() const { return _commandMaxHttpConnections->getVal(); }

    /// Return the sleep time (in milliseconds) between messages sent to active workers.
    int getMonitorSleepTimeMilliSec() const { return _monitorSleepTimeMilliSec->getVal(); }

    /// Return true if family map chunk distribution should depend on chunk size.
    bool getFamilyMapUsingChunkSize() const { return _familyMapUsingChunkSize->getVal(); }

    // Parameters of the Czar management service

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

    /// @return The unique name of Czar.
    std::string const& name() const { return _czarName; }

    /// @return The unique identifier of Czar.
    qmeta::CzarId id() const { return _czarId; }

    /// Set a unique identifier of Czar.
    /// @note In the current implementation of Qserv a value of the identifier is not
    /// available at a time when the configuration is initialized. The identifier is generated
    /// when registering Czar by name in a special table of teh Qserv database.
    /// This logic should be fixed in some future version of Qserv.
    void setId(qmeta::CzarId id);

    /// @return the JSON representation of the configuration parameters.
    /// @note The object has two collections of the parameters: 'input' - for
    /// parameters that were proided to the construction of the class, and
    /// 'actual' - for parameters that were expected (and set in the transient
    /// state). These collection may not be the same. For example, some older
    /// parameters may be phased out while still being present in the configuration
    /// files. Or, new actual parameters (with some reasonable defaults) might be
    /// introduced while not being set in the configuration file.
    nlohmann::json toJson() const { return _jsonConfig; }

private:
    CzarConfig(util::ConfigStore const& ConfigStore, std::string const& czarName);

    /// This mutex is needed for managing a state of the static member _instance.
    static std::mutex _mtxOnInstance;

    /// The configuratoon object created by the last call to the method 'create()'.
    static std::shared_ptr<CzarConfig> _instance;

    std::string const _czarName;  ///< The unique name of the Czar instance

    /// The unique identifier of the Czar instance, the real vale cannot be
    /// acquired until later. Using a crazy initial value in hopes of highlighting
    /// issues.
    /// TODO: Is this really the right place for this? (previously undefined)
    qmeta::CzarId _czarId = std::numeric_limits<qmeta::CzarId>::max();

    nlohmann::json _jsonConfig;  ///< JSON-ified configuration

    // Parameters below used in czar::Czar

    util::ConfigValMap _configValMap;  ///< Map of all configuration entries

    using CVTIntPtr = util::ConfigValTInt::IntPtr const;
    using CVTUIntPtr = util::ConfigValTUInt::UIntPtr const;
    using CVTBoolPtr = util::ConfigValTBool::BoolPtr const;
    using CVTStrPtr = util::ConfigValTStr::StrPtr const;

    bool const required = true;
    bool const notReq = false;
    bool const hidden = true;

    /// mySqlResultConfig values
    CVTStrPtr _resultDbUser =
            util::ConfigValTStr::create(_configValMap, "resultdb", "user", notReq, "qsmaster");
    CVTStrPtr _resultDbPasswd =
            util::ConfigValTStr::create(_configValMap, "resultdb", "passwd", required, "", hidden);
    CVTStrPtr _resultDbHost = util::ConfigValTStr::create(_configValMap, "resultdb", "host", required, "");
    CVTUIntPtr _resultDbPort = util::ConfigValTUInt::create(_configValMap, "resultdb", "port", notReq, 0);
    CVTStrPtr _resultDbUnixSocket =
            util::ConfigValTStr::create(_configValMap, "resultdb", "unix_socket", required, "");
    CVTStrPtr _resultDbDb =
            util::ConfigValTStr::create(_configValMap, "resultdb", "db", notReq, "qservResult");

    CVTIntPtr _maxTableSizeMB =
            util::ConfigValTInt::create(_configValMap, "resultdb", "maxtablesize_mb", notReq, 5001);
    CVTIntPtr _maxSqlConnectionAttempts =
            util::ConfigValTInt::create(_configValMap, "resultdb", "maxsqlconnectionattempts", notReq, 10);
    CVTStrPtr _resultEngine =
            util::ConfigValTStr::create(_configValMap, "resultdb", "engine", notReq, "myisam");
    CVTIntPtr _resultMaxConnections =
            util::ConfigValTInt::create(_configValMap, "resultdb", "maxconnections", notReq, 40);
    CVTIntPtr _resultMaxHttpConnections =
            util::ConfigValTInt::create(_configValMap, "resultdb", "maxhttpconnections", notReq, 2000);
    CVTIntPtr _oldestResultKeptDays =
            util::ConfigValTInt::create(_configValMap, "resultdb", "oldestResultKeptDays", notReq, 30);

    /// Get all the elements in the css section.
    CVTStrPtr _cssTechnology =
            util::ConfigValTStr::create(_configValMap, "css", "technology", notReq, "mysql");
    CVTStrPtr _cssHostname =
            util::ConfigValTStr::create(_configValMap, "css", "hostname", required, "127.0.0.1");
    CVTUIntPtr _cssPort = util::ConfigValTUInt::create(_configValMap, "css", "port", notReq, 0);
    CVTStrPtr _cssUsername =
            util::ConfigValTStr::create(_configValMap, "css", "username", notReq, "qsmaster");
    CVTStrPtr _cssPassword =
            util::ConfigValTStr::create(_configValMap, "css", "password", notReq, "", hidden);
    CVTStrPtr _cssDatabase =
            util::ConfigValTStr::create(_configValMap, "css", "database", notReq, "qservCssData");
    CVTStrPtr _cssSocket = util::ConfigValTStr::create(_configValMap, "css", "socket", notReq, "");

    // mySqlQmetaConfig values
    CVTStrPtr _qmetaUser = util::ConfigValTStr::create(_configValMap, "qmeta", "user", notReq, "qsmaster");
    CVTStrPtr _qmetaPasswd =
            util::ConfigValTStr::create(_configValMap, "qmeta", "passwd", notReq, "", hidden);
    CVTStrPtr _qmetaHost = util::ConfigValTStr::create(_configValMap, "qmeta", "host", notReq, "");
    CVTUIntPtr _qmetaPort = util::ConfigValTUInt::create(_configValMap, "qmeta", "port", notReq, 3306);
    CVTStrPtr _qmetaUnixSocket =
            util::ConfigValTStr::create(_configValMap, "qmeta", "unix_socket", notReq, "");
    CVTStrPtr _qmetaDb = util::ConfigValTStr::create(_configValMap, "qmeta", "db", notReq, "qservMeta");

    // mySqlQstatusDataConfig values
    CVTStrPtr _qstatusUser =
            util::ConfigValTStr::create(_configValMap, "qstatus", "user", notReq, "qsmaster");
    CVTStrPtr _qstatusPasswd = util::ConfigValTStr::create(_configValMap, "qstatus", "passwd", notReq, "");
    CVTStrPtr _qstatusHost = util::ConfigValTStr::create(_configValMap, "qstatus", "host", notReq, "");
    CVTUIntPtr _qstatusPort = util::ConfigValTUInt::create(_configValMap, "qstatus", "port", notReq, 3306);
    CVTStrPtr _qstatusUnixSocket =
            util::ConfigValTStr::create(_configValMap, "qstatus", "unix_socket", notReq, "");
    CVTStrPtr _qstatusDb =
            util::ConfigValTStr::create(_configValMap, "qstatus", "db", notReq, "qservStatusData");

    CVTStrPtr _emptyChunkPath =
            util::ConfigValTStr::create(_configValMap, "partitioner", "emptyChunkPath", notReq, ".");
    CVTIntPtr _maxMsgSourceStore =
            util::ConfigValTInt::create(_configValMap, "qmeta", "maxMsgSourceStore", notReq, 3);

    CVTIntPtr _qdispPoolSize =
            util::ConfigValTInt::create(_configValMap, "qdisppool", "poolSize", notReq, 1000);
    CVTIntPtr _qdispMaxPriority =
            util::ConfigValTInt::create(_configValMap, "qdisppool", "largestPriority", notReq, 2);
    CVTStrPtr _qdispVectRunSizes =
            util::ConfigValTStr::create(_configValMap, "qdisppool", "vectRunSizes", notReq, "800:800:500:50");
    CVTStrPtr _qdispVectMinRunningSizes =
            util::ConfigValTStr::create(_configValMap, "qdisppool", "vectMinRunningSizes", notReq, "0:3:3:3");

    // UberJobs
    CVTIntPtr _uberJobMaxChunks =
            util::ConfigValTInt::create(_configValMap, "uberjob", "maxChunks", notReq, 10000);

    CVTIntPtr _qMetaSecsBetweenChunkCompletionUpdates = util::ConfigValTInt::create(
            _configValMap, "tuning", "qMetaSecsBetweenChunkCompletionUpdates", notReq, 60);
    CVTIntPtr _interactiveChunkLimit =
            util::ConfigValTInt::create(_configValMap, "tuning", "interactiveChunkLimit", notReq, 10);
    CVTIntPtr _queryDistributionTestVer =
            util::ConfigValTInt::create(_configValMap, "tuning", "queryDistributionTestVer", notReq, 0);
    CVTBoolPtr _notifyWorkersOnQueryFinish =
            util::ConfigValTBool::create(_configValMap, "tuning", "notifyWorkersOnQueryFinish", notReq, 1);
    CVTBoolPtr _notifyWorkersOnCzarRestart =
            util::ConfigValTBool::create(_configValMap, "tuning", "notifyWorkersOnCzarRestart", notReq, 1);
    CVTIntPtr _czarStatsUpdateIvalSec =
            util::ConfigValTInt::create(_configValMap, "tuning", "czarStatsUpdateIvalSec", notReq, 1);
    CVTIntPtr _czarStatsRetainPeriodSec = util::ConfigValTInt::create(
            _configValMap, "tuning", "czarStatsRetainPeriodSec", notReq, 24 * 3600);

    // Replicator
    CVTStrPtr _replicationInstanceId =
            util::ConfigValTStr::create(_configValMap, "replication", "instance_id", notReq, "");
    CVTStrPtr _replicationAuthKey =
            util::ConfigValTStr::create(_configValMap, "replication", "auth_key", notReq, "", hidden);
    CVTStrPtr _replicationAdminAuthKey =
            util::ConfigValTStr::create(_configValMap, "replication", "admin_auth_key", notReq, "", hidden);
    CVTStrPtr _replicationRegistryHost =
            util::ConfigValTStr::create(_configValMap, "replication", "registry_host", notReq, "");
    CVTUIntPtr _replicationRegistryPort =
            util::ConfigValTUInt::create(_configValMap, "replication", "registry_port", notReq, 0);
    CVTIntPtr _replicationRegistryHearbeatIvalSec = util::ConfigValTInt::create(
            _configValMap, "replication", "registry_heartbeat_ival_sec", notReq, 1);
    CVTIntPtr _replicationHttpPort =
            util::ConfigValTInt::create(_configValMap, "replication", "http_port", notReq, 0);
    CVTUIntPtr _replicationNumHttpThreads =
            util::ConfigValTUInt::create(_configValMap, "replication", "num_http_threads", notReq, 2);

    // Active Worker
    CVTIntPtr _activeWorkerTimeoutAliveSecs =  // 5min
            util::ConfigValTInt::create(_configValMap, "activeworker", "timeoutAliveSecs", notReq, 60 * 5);
    CVTIntPtr _activeWorkerTimeoutDeadSecs =  // 10min
            util::ConfigValTInt::create(_configValMap, "activeworker", "timeoutDeadSecs", notReq, 60 * 10);
    CVTIntPtr _activeWorkerMaxLifetimeSecs =  // 1hr
            util::ConfigValTInt::create(_configValMap, "activeworker", "maxLifetimeSecs", notReq, 60 * 60);
    CVTIntPtr _monitorSleepTimeMilliSec = util::ConfigValTInt::create(
            _configValMap, "activeworker", "monitorSleepTimeMilliSec", notReq, 15'000);

    // FamilyMap
    CVTBoolPtr _familyMapUsingChunkSize =
            util::ConfigValTBool::create(_configValMap, "familymap", "usingChunkSize", notReq, 0);

    /// This may impact `_resultMaxHttpConnections` as too many connections may cause kernel memory issues.
    CVTIntPtr _commandMaxHttpConnections =
            util::ConfigValTInt::create(_configValMap, "uberjob", "commandMaxHttpConnections", notReq, 2000);
};

}  // namespace lsst::qserv::cconfig

#endif  // LSST_QSERV_CCONFIG_CZARCONFIG_H
