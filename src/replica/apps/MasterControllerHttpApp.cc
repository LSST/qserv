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

// Class header
#include "replica/apps/MasterControllerHttpApp.h"

// System headers
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>

// Third-party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "http/Client.h"
#include "http/MetaModule.h"
#include "http/Method.h"
#include "replica/config/Configuration.h"
#include "replica/contr/HttpProcessor.h"
#include "replica/contr/HttpProcessor.h"
#include "replica/contr/HttpProcessorConfig.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "util/common.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.MasterControllerHttpApp");

/**
 * This structure encapsulates default values for the Master Controller.
 * These values may be overridden by specifying the corresponding command
 * line options. See the constructor of the application class for further details.
 */
struct {
    unsigned int const healthProbeIntervalSec = 60;
    unsigned int const replicationIntervalSec = 60;
    unsigned int const czarResponseTimeoutSec = 60;
    unsigned int const workerResponseTimeoutSec = 60;
    unsigned int const workerEvictTimeoutSec = 3600;
    unsigned int const qservSyncTimeoutSec = 1800;
    unsigned int const workerReconfigTimeoutSec = 600;

    bool const purge = false;
    bool const disableQservSync = false;
    bool const forceQservSync = false;
    bool const permanentDelete = false;

} const defaultOptions;

string const description =
        "This application is the Master Replication Controller which has"
        " a built-in Cluster Health Monitor and a linear Replication loop."
        " The Monitor would track a status of both Qserv and Replication workers"
        " and trigger the worker exclusion sequence if both services were found"
        " non-responsive within a configured interval."
        " The interval is specified via the corresponding command-line option."
        " And it also has some built-in default value."
        " Also, note that only a single node failure can trigger the worker"
        " exclusion sequence."
        " The controller has the built-in REST API which accepts external commands"
        " or request for information.";

bool const injectDatabaseOptions = true;
bool const boostProtobufVersionCheck = true;
bool const enableServiceProvider = true;

}  // namespace

namespace lsst::qserv::replica {

MasterControllerHttpApp::Ptr MasterControllerHttpApp::create(int argc, char* argv[]) {
    return Ptr(new MasterControllerHttpApp(argc, argv));
}

MasterControllerHttpApp::MasterControllerHttpApp(int argc, char* argv[])
        : Application(argc, argv, ::description, ::injectDatabaseOptions, ::boostProtobufVersionCheck,
                      ::enableServiceProvider),
          _healthProbeIntervalSec(::defaultOptions.healthProbeIntervalSec),
          _replicationIntervalSec(::defaultOptions.replicationIntervalSec),
          _czarResponseTimeoutSec(::defaultOptions.czarResponseTimeoutSec),
          _workerResponseTimeoutSec(::defaultOptions.workerResponseTimeoutSec),
          _workerEvictTimeoutSec(::defaultOptions.workerEvictTimeoutSec),
          _qservSyncTimeoutSec(::defaultOptions.qservSyncTimeoutSec),
          _workerReconfigTimeoutSec(::defaultOptions.workerReconfigTimeoutSec),
          _purge(::defaultOptions.purge),
          _disableQservSync(::defaultOptions.disableQservSync),
          _forceQservSync(::defaultOptions.forceQservSync),
          _permanentDelete(::defaultOptions.permanentDelete),
          _qservCzarDbUrl(Configuration::qservCzarDbUrl()) {
    // Configure the command line parser

    parser().option("name",
                    "The unique name of the controller as it's seen in the service discovery Registry.",
                    _name);
    parser().option("health-probe-interval",
                    "Interval (seconds) between iterations of the health monitoring probes.",
                    _healthProbeIntervalSec);
    parser().option("replication-interval",
                    "Interval (seconds) between running the linear sequence of"
                    " actions: check - fix-up - replicate - re-balance.",
                    _replicationIntervalSec);
    parser().option("czar-response-timeout",
                    "The maximum number of seconds to wait before giving up"
                    " on requests sent to Czar.",
                    _czarResponseTimeoutSec);
    parser().option("worker-response-timeout",
                    "The maximum number of seconds to wait before giving up"
                    " on requests sent to workers.",
                    _workerResponseTimeoutSec);
    parser().option("worker-evict-timeout",
                    "The maximum number of seconds to allow troubled workers to recover"
                    " from the last catastrophic event before evicting them from a cluster.",
                    _workerEvictTimeoutSec);
    parser().option("qserv-sync-timeout",
                    "The maximum number of seconds to wait before Qserv workers respond"
                    " to the synchronization requests before bailing out and proceeding"
                    " to the next step in the normal replication sequence. A value which"
                    " differs from " +
                            to_string(defaultOptions.qservSyncTimeoutSec) +
                            " would override the corresponding parameter specified"
                            " in the Configuration.",
                    _qservSyncTimeoutSec);
    parser().option("worker-config-timeout",
                    "The maximum number of seconds to wait for the completion of the worker"
                    " reconfiguration requests. A value which"
                    " differs from " +
                            to_string(defaultOptions.workerReconfigTimeoutSec) +
                            " would override the corresponding parameter specified"
                            " in the Configuration.",
                    _workerReconfigTimeoutSec);
    parser().flag("qserv-sync-disable", "The flag which disables replica synchroization at Qserv workers.",
                  _disableQservSync);
    parser().flag("qserv-sync-force",
                  "The flag which would force Qserv workers to update their list of replicas"
                  " even if some of the chunk replicas were still in use by on-going queries."
                  " This affect replicas to be deleted from the workers during the synchronization"
                  " stages.",
                  _forceQservSync);
    parser().flag("qserv-chunk-map-update",
                  "The flag which would result in updating the chunk disposition map"
                  " in Qserv's QMeta database.",
                  _qservChunkMapUpdate);
    parser().flag("purge",
                  "The binary flag which, if provided, enables the 'purge' algorithm in"
                  " the end of each replication cycle that eliminates excess replicas which"
                  " may have been created by algorithms ran earlier in the cycle.",
                  _purge);
    parser().flag("permanent-worker-delete",
                  "The flag would trigger the permanent removal of the evicted workers"
                  " from the configuration of the Replication system. Please, use"
                  " this option with caution as it will result in losing all records"
                  " associated with the deleted workers.",
                  _permanentDelete);
    parser().option("qserv-czar-db", "A connection URL to the MySQL server of the Qserv master database.",
                    _qservCzarDbUrl);
    parser().option("http-root",
                    "The root folder for the static content to be served by the built-in HTTP service.",
                    _httpRoot);
    parser().flag("do-not-create-folders",
                  "Do not attempt creating missing folders used by the Controller."
                  " Specify this flag in the production deployments of the Replication/Ingest system.",
                  _doNotCreateMissingFolders);
}

int MasterControllerHttpApp::runImpl() {
    // IMPORTANT: clear the corresponding member variables after using the URLs
    // to the Configuration to prevent contamination of the application's log
    // stream with values of the sensitive command line arguments.
    if (!_qservCzarDbUrl.empty()) {
        Configuration::setQservCzarDbUrl(_qservCzarDbUrl);
        _qservCzarDbUrl = "******";
    }
    _controller = Controller::create(serviceProvider());

    // ATTENTION: Controller depends on a number of folders that are used for
    // storing intermediate files of various sizes. Locations (absolute path names)
    // of the folders are set in the corresponding configuration parameters.
    // Desired characteristics (including size, I/O latency, I/O bandwidth, etc.) of
    // the folders may vary depending on a type of the Controller's operation and
    // a scale of a particular Qserv deployment. Note that the overall performance
    // and scalability greatly depends on the quality of of the underlying filesystems.
    // Usually, in the large-scale deployments, the folders should be pre-created and be placed
    // at the large-capacity high-performance filesystems at the Qserv deployment time.
    _controller->verifyFolders(!_doNotCreateMissingFolders);

    _logControllerStartedEvent();

    // These tasks should be running in parallel

    auto self = shared_from_base<MasterControllerHttpApp>();

    _replicationTask = ReplicationTask::create(
            _controller, [self](Task::Ptr const& ptr) { self->_isFailed.fail(); }, _qservSyncTimeoutSec,
            _disableQservSync, _forceQservSync, _qservChunkMapUpdate, _replicationIntervalSec, _purge);
    _replicationTask->start();

    _healthMonitorTask = HealthMonitorTask::create(
            _controller, [self](Task::Ptr const& ptr) { self->_isFailed.fail(); },
            [self](string const& worker2evict) { self->_evict(worker2evict); }, _workerEvictTimeoutSec,
            _workerResponseTimeoutSec, _healthProbeIntervalSec);
    _healthMonitorTask->start();

    // Running the REST server in its own thread
    auto const httpProcessor = HttpProcessor::create(
            _controller,
            HttpProcessorConfig(_czarResponseTimeoutSec, _workerResponseTimeoutSec, _qservSyncTimeoutSec,
                                _workerReconfigTimeoutSec, _httpRoot),
            _healthMonitorTask);
    thread ingestHttpSvrThread([httpProcessor]() { httpProcessor->run(); });

    // Keep sending periodic 'heartbeats' to the Registry service to report a configuration
    // and a status of the current Controller before a catastrophic failure is reported by
    // any activity.
    _registryUpdateLoop();

    // Stop all threads if any are still running
    _healthMonitorTask->stop();
    _replicationTask->stop();
    httpProcessor->stop();

    ingestHttpSvrThread.join();

    if ((_replicationTask != nullptr) && _replicationTask->isRunning()) _replicationTask->stop();
    _logControllerStoppedEvent();

    return 1;
}

void MasterControllerHttpApp::_evict(string const& worker) {
    _logWorkerEvictionStartedEvent(worker);

    // This thread needs to be stopped to avoid any interference with
    // the worker exclusion protocol.
    _replicationTask->stop();

    // This thread will be allowed to run for as long as it's permitted by
    // the corresponding timeouts set for Requests and Jobs in the Configuration,
    // or until a catastrophic failure occurs within any control thread (including
    // this one).
    auto self = shared_from_base<MasterControllerHttpApp>();

    _deleteWorkerTask = DeleteWorkerTask::create(
            _controller, [self](Task::Ptr const& ptr) { self->_isFailed.fail(); }, worker, _permanentDelete);
    _deleteWorkerTask->startAndWait([self](Task::Ptr const& ptr) -> bool { return self->_isFailed(); });
    _deleteWorkerTask->stop();  // it's safe to call this method even if the thread is
                                // no longer running.

    _deleteWorkerTask = nullptr;  // the object is no longer needed because it was
                                  // created for a specific worker.

    // Resume the normal replication sequence unless a catastrophic failure
    // in the system has been detected
    if (not _isFailed()) _replicationTask->start();

    _logWorkerEvictionFinishedEvent(worker);
}

void MasterControllerHttpApp::_logControllerStartedEvent() const {
    _assertIsStarted(__func__);
    ControllerEvent event;
    event.status = "STARTED";
    event.kvInfo.emplace_back("host", _controller->identity().host);
    event.kvInfo.emplace_back("pid", to_string(_controller->identity().pid));
    event.kvInfo.emplace_back("health-probe-interval", to_string(_healthProbeIntervalSec));
    event.kvInfo.emplace_back("replication-interval", to_string(_replicationIntervalSec));
    event.kvInfo.emplace_back("czar-response-timeout", to_string(_czarResponseTimeoutSec));
    event.kvInfo.emplace_back("worker-response-timeout", to_string(_workerResponseTimeoutSec));
    event.kvInfo.emplace_back("worker-evict-timeout", to_string(_workerEvictTimeoutSec));
    event.kvInfo.emplace_back("qserv-sync-timeout", to_string(_qservSyncTimeoutSec));
    event.kvInfo.emplace_back("qserv-sync-force", bool2str(_forceQservSync));
    event.kvInfo.emplace_back("worker-config-timeout", to_string(_workerReconfigTimeoutSec));
    event.kvInfo.emplace_back("purge", bool2str(_purge));
    event.kvInfo.emplace_back("permanent-worker-delete", bool2str(_permanentDelete));
    _logEvent(event);
}

void MasterControllerHttpApp::_logControllerStoppedEvent() const {
    _assertIsStarted(__func__);
    ControllerEvent event;
    event.status = "STOPPED";
    _logEvent(event);
}

void MasterControllerHttpApp::_logWorkerEvictionStartedEvent(string const& worker) const {
    _assertIsStarted(__func__);
    ControllerEvent event;
    event.operation = "worker eviction";
    event.status = "STARTED";
    event.kvInfo.emplace_back("worker", worker);
    _logEvent(event);
}

void MasterControllerHttpApp::_logWorkerEvictionFinishedEvent(string const& worker) const {
    _assertIsStarted(__func__);
    ControllerEvent event;
    event.operation = "worker eviction";
    event.status = "FINISHED";
    event.kvInfo.emplace_back("worker", worker);
    _logEvent(event);
}

void MasterControllerHttpApp::_logEvent(ControllerEvent& event) const {
    event.controllerId = _controller->identity().id;
    event.timeStamp = util::TimeUtils::now();
    event.task = _controllerName4log();

    // For now ignore exceptions when logging events. Just report errors.
    try {
        serviceProvider()->databaseServices()->logControllerEvent(event);
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR, _controllerName4log() << "  failed to log event in " << __func__);
    }
}

void MasterControllerHttpApp::_assertIsStarted(string const& func) const {
    if (nullptr == _controller) {
        throw logic_error("MasterControllerHttpApp::" + func + "  Controller is not running");
    }
}

void MasterControllerHttpApp::_registryUpdateLoop() {
    auto const serviceProvider = _controller->serviceProvider();
    auto const config = serviceProvider->config();
    auto const method = http::Method::POST;
    string const url = "http://" + config->get<string>("registry", "host") + ":" +
                       to_string(config->get<uint16_t>("registry", "port")) + "/controller";
    vector<string> const headers = {"Content-Type: application/json"};
    json const request = json::object({{"version", http::MetaModule::version},
                                       {"instance_id", serviceProvider->instanceId()},
                                       {"auth_key", serviceProvider->authKey()},
                                       {"controller",
                                        {{"name", _name},
                                         {"id", _controller->identity().id},
                                         {"port", config->get<uint16_t>("controller", "http-server-port")},
                                         {"host-name", util::get_current_host_fqdn()}}}});
    string const requestContext =
            _controllerName4log() + ": '" + http::method2string(method) + "' request to '" + url + "'";
    http::Client client(method, url, request.dump(), headers);
    while (!_isFailed()) {
        try {
            json const response = client.readAsJson();
            if (0 == response.at("success").get<int>()) {
                string const error = response.at("error").get<string>();
                LOGS(_log, LOG_LVL_ERROR, requestContext + " was denied, error: '" + error + "'.");
                abort();
            }
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_WARN, requestContext + " failed, ex: " + ex.what());
        }
        this_thread::sleep_for(
                chrono::seconds(max(1U, config->get<unsigned int>("registry", "heartbeat-ival-sec"))));
    }
}

}  // namespace lsst::qserv::replica
