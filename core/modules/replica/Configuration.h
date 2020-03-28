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
#ifndef LSST_QSERV_REPLICA_CONFIGURATION_H
#define LSST_QSERV_REPLICA_CONFIGURATION_H

/**
 * This header defines an abstract class Configuration and a number of
 * other relevant classes, which represent a public interface to
 * the Configuration service of the Replication System. Specific implementations
 * of the service's interface are found in separate headers and source files.
 */

// System headers
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "global/constants.h"
#include "replica/Common.h"
#include "replica/ConfigurationIFace.h"
#include "util/Mutex.h"

// This header declarations
namespace lsst {
namespace qserv {
namespace replica {

/**
  * Class Configuration is a proxy class to a family of concrete classes
  * providing configuration services for the components of the Replication
  * system.
  */
class Configuration : public ConfigurationIFace {
public:
    /// The pointer type for instances of the class.
    typedef std::shared_ptr<Configuration> Ptr;

    /// @return The JSON representation of the object.
    static nlohmann::json toJson(Configuration::Ptr const& config);

    /**
     * The static factory method will instantiate an instance of a subclass
     * corresponding to a prefix of the configuration URL. The following
     * prefixes are supported:
     * @code
     *   file:<path>
     *   mysql://[user][:password]@[host][:port][/database]
     * @code
     *
     * @param configUrl The configuration source.
     * @throw std::invalid_argument If the URL has unsupported prefix or it
     *   couldn't be parsed.                          
     * @throw std::runtime_error If the input configuration is not consistent
     *   with expectations of the application.
     */
    static Ptr load(std::string const& configUrl);

    /**
     * The static factory method will instantiate an instance of a subclass
     * loaded from a key-value map. This instance will correspond to the following
     * special prefix:
     *
     *   map:
     *
     * @param kvMap The configuration source.
     * @throw std::runtime_error If the input configuration is not consistent
     *   with expectations of the application.
     */
    static Ptr load(std::map<std::string, std::string> const& kvMap);

    Configuration() = delete;
    Configuration(Configuration const&) = delete;
    Configuration& operator=(Configuration const&) = delete;

    virtual ~Configuration() = default;

    /**
     * Reload non-static parameters of the Configuration from the same source
     * they were originally read before.
     * 
     * @note If the object was initialed from an in-memory map then
     *   the method will do noting.
     */
    void reload();

    /**
     * Reload non-static parameters of the Configuration from an external source.
     *
     * @param configUrl The configuration source,
     * @throw std::invalid_argument If the URL has unsupported prefix or it couldn't
     *   be parsed.
     * @throw std::runtime_error If the input configuration is not consistent with
     *   expectations of the application.
     */
    void reload(std::string const& configUrl);

    /**
     * Reload non-static parameters of the Configuration from the in-memory map.
     *
     * @param kvMap The configuration source.
     * @throw std::runtime_error If the input configuration is not consistent
     *   with expectations of the application.
     */
    void reload(std::map<std::string, std::string> const& kvMap);
    
    std::string prefix() const final;

    std::string configUrl(bool showPassword=false) const final;

    std::vector<std::string> workers(bool isEnabled=true,
                                     bool isReadOnly=false) const final;

    std::vector<std::string> allWorkers() const final;

    size_t requestBufferSizeBytes() const final;
    void setRequestBufferSizeBytes(size_t val,
                                   bool updatePersistentState=true)final;

    unsigned int retryTimeoutSec() const final;
    void setRetryTimeoutSec(unsigned int val,
                            bool updatePersistentState=true) final;

    size_t controllerThreads() const final;
    void setControllerThreads(size_t val,
                              bool updatePersistentState=true) final;

    uint16_t controllerHttpPort() const final;
    void setControllerHttpPort(uint16_t val,
                               bool updatePersistentState=true) final;

    size_t controllerHttpThreads() const final;
    void setControllerHttpThreads(size_t val,
                                  bool updatePersistentState=true) final;

    unsigned int controllerRequestTimeoutSec() const final;
    void setControllerRequestTimeoutSec(unsigned int val,
                                        bool updatePersistentState=true) final;

    std::string controllerEmptyChunksDir() const final;

    unsigned int jobTimeoutSec() const final;
    void setJobTimeoutSec(unsigned int val,
                          bool updatePersistentState=true) final;

    unsigned int jobHeartbeatTimeoutSec() const final;
    void setJobHeartbeatTimeoutSec(unsigned int val,
                                   bool updatePersistentState=true) final;

    bool xrootdAutoNotify() const final;
    void setXrootdAutoNotify(bool val,
                             bool updatePersistentState=true) final;

    std::string xrootdHost() const final;
    void setXrootdHost(std::string const& val,
                       bool updatePersistentState=true) final;

    uint16_t xrootdPort() const final;
    void     setXrootdPort(uint16_t val,
                           bool updatePersistentState=true) final;

    unsigned int xrootdTimeoutSec() const final;
    void setXrootdTimeoutSec(unsigned int val,
                             bool updatePersistentState=true) final;

    std::string databaseTechnology() const final;
    std::string databaseHost() const final;
    uint16_t    databasePort() const final;
    std::string databaseUser() const final;
    std::string databasePassword() const final;
    std::string databaseName() const final;

    size_t databaseServicesPoolSize() const final;
    void setDatabaseServicesPoolSize(size_t val,
                                     bool updatePersistentState=true) final;

    std::string qservMasterDatabaseHost() const final;
    uint16_t    qservMasterDatabasePort() const final;
    std::string qservMasterDatabaseUser() const final;
    std::string qservMasterDatabaseName() const final;
    size_t      qservMasterDatabaseServicesPoolSize() const final;
    std::string qservMasterDatabaseTmpDir() const final;

    std::vector<std::string> databaseFamilies() const final;

    bool isKnownDatabaseFamily(std::string const& name) const final;

    DatabaseFamilyInfo databaseFamilyInfo(std::string const& name) const final;
    DatabaseFamilyInfo addDatabaseFamily(DatabaseFamilyInfo const& info) final;

    void deleteDatabaseFamily(std::string const& name) final;

    size_t replicationLevel(std::string const& family) const;

    std::vector<std::string> databases(std::string const& family=std::string(),
                                       bool allDatabases=false,
                                       bool isPublished=true) const final;

    bool isKnownDatabase(std::string const& name) const final;

    DatabaseInfo databaseInfo(std::string const& name) const final;
    DatabaseInfo addDatabase(DatabaseInfo const& info) final;
    DatabaseInfo publishDatabase(std::string const& name) final;

    void deleteDatabase(std::string const& name) final;

    DatabaseInfo addTable(std::string const& database,
                            std::string const& table,
                            bool isPartitioned,
                            std::list<SqlColDef> const& columns=std::list<SqlColDef>(),
                            bool isDirectorTable=false,
                            std::string const& directorTableKey="objectId",
                            std::string const& chunkIdColName=lsst::qserv::CHUNK_COLUMN,
                            std::string const& subChunkIdColName=lsst::qserv::SUB_CHUNK_COLUMN,
                            std::string const& latitudeColName=std::string(),
                            std::string const& longitudeColName=std::string()) final;

    DatabaseInfo deleteTable(std::string const& database,
                             std::string const& table) final;

    bool isKnownWorker(std::string const& name) const final;

    WorkerInfo workerInfo(std::string const& name) const final;


    void addWorker(WorkerInfo const& info) final;
    void deleteWorker(std::string const& name) final;

    WorkerInfo disableWorker(std::string const& name,
                             bool disable=true,
                             bool updatePersistentState=true) final;

    WorkerInfo setWorkerReadOnly(std::string const& name,
                                 bool readOnly=true,
                                 bool updatePersistentState=true) final;

    WorkerInfo setWorkerSvcHost(std::string const& name,
                                 std::string const& host,
                                 bool updatePersistentState=true) final;

    WorkerInfo setWorkerSvcPort(std::string const& name,
                                uint16_t port,
                                bool updatePersistentState=true) final;

    WorkerInfo setWorkerFsHost(std::string const& name,
                               std::string const& host,
                               bool updatePersistentState=true) final;

    WorkerInfo setWorkerFsPort(std::string const& name,
                               uint16_t port,
                               bool updatePersistentState=true) final;

    WorkerInfo setWorkerDataDir(std::string const& name,
                                std::string const& dataDir,
                                bool updatePersistentState=true) final;

    WorkerInfo setWorkerDbHost(std::string const& name,
                               std::string const& host,
                               bool updatePersistentState=true) final;

    WorkerInfo setWorkerDbPort(std::string const& name,
                               uint16_t port,
                               bool updatePersistentState=true) final;

    WorkerInfo setWorkerDbUser(std::string const& name,
                               std::string const& user,
                               bool updatePersistentState=true) final;

    WorkerInfo setWorkerLoaderHost(std::string const& name,
                                   std::string const& host,
                                   bool updatePersistentState=true) final;

    WorkerInfo setWorkerLoaderPort(std::string const& name,
                                   uint16_t port,
                                   bool updatePersistentState=true) final;

    WorkerInfo setWorkerLoaderTmpDir(std::string const& name,
                                     std::string const& tmpDir,
                                     bool updatePersistentState=true) final;

    WorkerInfo setWorkerExporterHost(std::string const& name,
                                     std::string const& host,
                                     bool updatePersistentState=true) final;

    WorkerInfo setWorkerExporterPort(std::string const& name,
                                     uint16_t port,
                                     bool updatePersistentState=true) final;

    WorkerInfo setWorkerExporterTmpDir(std::string const& name,
                                       std::string const& tmpDir,
                                       bool updatePersistentState=true) final;

    std::string workerTechnology() const final;
    void setWorkerTechnology(std::string const& val,
                             bool updatePersistentState=true) final;

    size_t workerNumProcessingThreads() const final;
    void setWorkerNumProcessingThreads(size_t val,
                                       bool updatePersistentState=true) final;

    size_t fsNumProcessingThreads() const final;
    void setFsNumProcessingThreads(size_t val,
                                   bool updatePersistentState=true) final;

    size_t workerFsBufferSizeBytes() const final;
    void setWorkerFsBufferSizeBytes(size_t val,
                                    bool updatePersistentState=true) final;

    size_t loaderNumProcessingThreads() const final;
    void setLoaderNumProcessingThreads(size_t val,
                                       bool updatePersistentState=true) final;

    size_t exporterNumProcessingThreads() const final;
    void setExporterNumProcessingThreads(size_t val,
                                         bool updatePersistentState=true) final;

    std::string asString() const final;

    void dumpIntoLogger() const final;

    // ---------------------------------------------------------------
    // -- (global) parameters of the Qserv worker database services --
    // ---------------------------------------------------------------

    /// @return the database password
    static std::string const& qservMasterDatabasePassword() { return _qservMasterDatabasePassword; }

    /**
     * @param newPassword The new password to be set.
     * @return the previous value of the password
     */
    static std::string setQservMasterDatabasePassword(std::string const& newPassword);

    // -------------------------------------------------------------
    // -- Global parameters of the Qserv worker database services --
    // -------------------------------------------------------------

    /**
     * This method is used by the workers when they need to connect directly
     * to the corresponding MySQL/MariaDB service of the Qserv worker.
     *
     * @return The current password for the worker databases
     */
    static std::string qservWorkerDatabasePassword() { return _qservWorkerDatabasePassword; }

    /**
     * @param newPassword The new password to be set.
     * @return the previous value of the password
     */
    static std::string setQservWorkerDatabasePassword(std::string const& newPassword);

    // --------------------------------------------------
    // -- Global parameters of the database connectors --
    // --------------------------------------------------

    /// @return the default mode for database reconnects.
    static bool databaseAllowReconnect() { return _databaseAllowReconnect; }

    /**
     * Change the default value of a parameter defining a policy for handling
     * automatic reconnects to a database server. Setting 'true' will enable
     * reconnects.
     *
     * @param value The new value of the parameter.
     * @return The previous value.
     */
    static bool setDatabaseAllowReconnect(bool value);

    /// @return The default timeout for connecting to database servers.
    static unsigned int databaseConnectTimeoutSec() { return _databaseConnectTimeoutSec; }

    /**
     * Change the default value of a parameter specifying delays between automatic
     * reconnects (should those be enabled by the corresponding policy).
     *
     * @param value The new value of the parameter (must be strictly greater than 0).
     * @return The previous value.
     * @throws std::invalid_argument If the new value of the parameter is 0.
     */
    static unsigned int setDatabaseConnectTimeoutSec(unsigned int value);

    /**
     * @return The default number of a maximum number of attempts to execute
     *   a query due to database connection failures and subsequent reconnects.
     */
    static unsigned int databaseMaxReconnects() { return _databaseMaxReconnects; }

    /**
     * Change the default value of a parameter specifying the maximum number
     * of attempts to execute a query due to database connection failures and
     * subsequent reconnects (should they be enabled by the corresponding policy).
     *
     * @param value The new value of the parameter (must be strictly greater than 0).
     *
     * @return The previous value.
     * @throws std::invalid_argument If the new value of the parameter is 0.
     */
    static unsigned int setDatabaseMaxReconnects(unsigned int value);

    /**
     * @return The default timeout for executing transactions at a presence
     *   of server reconnects.
     */
    static unsigned int databaseTransactionTimeoutSec() { return _databaseTransactionTimeoutSec; }

    /**
     * Change the default value of a parameter specifying a timeout for executing
     * transactions at a presence of server reconnects.
     *
     * @param value The new value of the parameter (must be strictly greater than 0).
     * @return The previous value.
     * @throws std::invalid_argument If the new value of the parameter is 0.
     */
    static unsigned int setDatabaseTransactionTimeoutSec(unsigned int value);

protected:
    /**
     * @param impl A pointer to the actual implementation of the Configuration.
     */
    Configuration(ConfigurationIFace::Ptr const& impl) : _impl(impl) {}

private:
    /// For implementing synchronized methods.
    mutable util::Mutex _mtx;

    /// The actual implementation of the forwarded methods
    ConfigurationIFace::Ptr _impl;

    // Global parameters of the database connectors (read-write).

    static bool         _databaseAllowReconnect;
    static unsigned int _databaseConnectTimeoutSec;
    static unsigned int _databaseMaxReconnects;
    static unsigned int _databaseTransactionTimeoutSec;
    static std::string  _qservMasterDatabasePassword;
    static std::string  _qservWorkerDatabasePassword;
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_REPLICA_CONFIGURATION_H
