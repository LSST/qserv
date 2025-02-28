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
#ifndef LSST_QSERV_MASTERCONTROLLERHTTPAPP_H
#define LSST_QSERV_MASTERCONTROLLERHTTPAPP_H

// System headers
#include <string>

// Qserv headers
#include "replica/apps/Application.h"
#include "replica/contr/Controller.h"
#include "replica/contr/DeleteWorkerTask.h"
#include "replica/contr/HealthMonitorTask.h"
#include "replica/contr/ReplicationTask.h"
#include "replica/util/OneWayFailer.h"

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class MasterControllerHttpApp implements the Replication Controller which
 * allows interactions via the REST API. When it starts the controller launches
 * two tasks running in parallel (in dedicated threads): Linear Replication one and
 * the Health Monitoring one. These tasks can be suspended/resumed via the REST API.
 */
class MasterControllerHttpApp : public Application {
public:
    typedef std::shared_ptr<MasterControllerHttpApp> Ptr;

    /**
     * The factory method is the only way of creating objects of this class
     * because of the very base class's inheritance from 'enable_shared_from_this'.
     *
     * @param argc The number of command-line arguments.
     * @param argv The vector of command-line arguments.
     */
    static Ptr create(int argc, char* argv[]);

    MasterControllerHttpApp() = delete;
    MasterControllerHttpApp(MasterControllerHttpApp const&) = delete;
    MasterControllerHttpApp& operator=(MasterControllerHttpApp const&) = delete;

    ~MasterControllerHttpApp() final = default;

protected:
    /// @see Application::runImpl()
    int runImpl() final;

private:
    /// @see MasterControllerHttpApp::create()
    MasterControllerHttpApp(int argc, char* argv[]);

    /// @return the name of the application for the purpose of logging
    std::string _controllerName4log() const { return "CONTROLLER[" + _name + "]"; }

    /**
     * Evict the specified worker from the cluster.
     * @note This method is called by the health-monitoring thread when
     *   a condition for evicting the worker is detected. The calling thread
     *   will be blocked for a duration of this call.
     * @param worker The name of a worker to be evicted.
     */
    void _evict(std::string const& worker);

    /**
     * Log the very first event to report the initialization of the controller
     * along with values of its command-line parameters.
     * @throw std::logic_error If the instance of the Controller is not set up.
     */
    void _logControllerStartedEvent() const;

    /**
     * Log the very last event to report the finalization of the controller.
     * @throw std::logic_error If the instance of the Controller is not set up
     */
    void _logControllerStoppedEvent() const;

    /**
     * Log the beginning of the worker eviction.
     * @param worker The name of a worker to be evicted.
     * @throw std::logic_error If the instance of the Controller is not set up.
     */
    void _logWorkerEvictionStartedEvent(std::string const& worker) const;

    /**
     * Log the ending of the worker eviction.
     * @param worker The name of a worker to be evicted.
     * @throw std::logic_error If the instance of the Controller is not set up.
     */
    void _logWorkerEvictionFinishedEvent(std::string const& worker) const;

    /**
     * Log an event in the persistent log.
     * @param event Event to be recorded.
     */
    void _logEvent(ControllerEvent& event) const;

    /**
     * Ensure the Controller is running. Otherwise, throw an exception.
     * @param func The name of a method which called this operation.
     */
    void _assertIsStarted(std::string const& func) const;

    /**
     * This function will keep periodically updating Controller's info in the Replication
     * System's Registry.
     * @note The thread will terminate the process if the registraton request to the Registry
     * was explicitly denied by the service. This means the application may be misconfigured.
     * Transient communication errors when attempting to connect or send requests to
     * the Registry will be posted into the log stream and ignored.
     */
    void _registryUpdateLoop();

    // Command line parameters

    /// The unique name of the controller as it's seen in the service discovery Registry.
    std::string _name = "master";

    unsigned int _healthProbeIntervalSec;
    unsigned int _replicationIntervalSec;
    unsigned int _czarResponseTimeoutSec;
    unsigned int _workerResponseTimeoutSec;
    unsigned int _workerEvictTimeoutSec;
    unsigned int _qservSyncTimeoutSec;
    unsigned int _numReplicas;
    unsigned int _workerReconfigTimeoutSec;

    bool _purge;
    bool _disableQservSync;
    bool _forceQservSync;
    bool _qservChunkMapUpdate;
    bool _permanentDelete;

    /// A connection URL for the MySQL service of the Qserv master database.
    std::string _qservCzarDbUrl;

    /// The root folder for the static content to be served by the built-in
    /// HTTP service.
    std::string _httpRoot;

    /// The Controller will create missing folders unless told not to do so by
    /// passing the corresponding command-line flag.
    bool _doNotCreateMissingFolders = false;

    /// This flag will be raised by any thread if a non-recoverable
    /// catastrophic failure will be detected.
    OneWayFailer _isFailed;

    /// The controller for launching operations with the Replication system services.
    Controller::Ptr _controller;

    // Control threads

    HealthMonitorTask::Ptr _healthMonitorTask;
    ReplicationTask::Ptr _replicationTask;
    DeleteWorkerTask::Ptr _deleteWorkerTask;
};

}  // namespace lsst::qserv::replica

#endif /* LSST_QSERV_MASTERCONTROLLERHTTPAPP_H */
