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
#include "replica/apps/WorkerApp.h"

// System headers
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/export/ExportServer.h"
#include "replica/ingest/IngestHttpSvc.h"
#include "replica/ingest/IngestSvc.h"
#include "replica/mysql/DatabaseMySQL.h"
#include "replica/mysql/DatabaseMySQLUtils.h"
#include "replica/registry/Registry.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/FileUtils.h"
#include "replica/worker/FileServer.h"
#include "replica/worker/WorkerHttpSvc.h"
#include "replica/worker/WorkerServer.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {

string const description = "This application represents the worker service of the Replication system.";

bool const injectDatabaseOptions = true;
bool const boostProtobufVersionCheck = true;
bool const enableServiceProvider = true;

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerApp");

}  // namespace

namespace lsst::qserv::replica {

using namespace database::mysql;

WorkerApp::Ptr WorkerApp::create(int argc, char* argv[]) { return Ptr(new WorkerApp(argc, argv)); }

WorkerApp::WorkerApp(int argc, char* argv[])
        : Application(argc, argv, description, injectDatabaseOptions, boostProtobufVersionCheck,
                      enableServiceProvider),
          _qservWorkerDbUrl(Configuration::qservWorkerDbUrl()) {
    parser().option("qserv-worker-db",
                    "A connection url for the MySQL service of the Qserv"
                    " worker database.",
                    _qservWorkerDbUrl);
    parser().flag("do-not-create-folders",
                  "Do not attempt creating missing folders used by the worker services."
                  " Specify this flag in the production deployments of the Replication/Ingest"
                  " system.",
                  _doNotCreateMissingFolders);
}

int WorkerApp::runImpl() {
    string const context = "WorkerApp::" + string(__func__) + "  ";

    if (!_qservWorkerDbUrl.empty()) {
        // IMPORTANT: set the connector, then clear it up to avoid
        // contaminating the log files when logging command line arguments
        // parsed by the application.
        Configuration::setQservWorkerDbUrl(_qservWorkerDbUrl);
        _qservWorkerDbUrl = "******";
    }

    // Read a unique identifier of the worker from Qserv's worker database.
    string worker;
    {
        // The RAII-style connection handler will rollback a transaction
        // and close the MySQL connection in case of exceptions.
        ConnectionHandler const handler(
                Connection::open(Configuration::qservWorkerDbParams("qservw_worker")));
        QueryGenerator const g(handler.conn);
        string const query = g.select("id") + g.from("Id");
        handler.conn->executeInOwnTransaction([&worker, &context, &query](auto conn) {
            if (!selectSingleValue(conn, query, worker)) {
                throw invalid_argument(context + "worker identity is not set in the Qserv worker database.");
            }
        });
        LOGS(_log, LOG_LVL_INFO, context << "worker: " << worker);
    }

    _verifyCreateFolders();

    auto const reqProcSvr = WorkerServer::create(serviceProvider(), worker);
    thread reqProcSvrThread([reqProcSvr]() { reqProcSvr->run(); });

    auto const reqProcHttpSvr = WorkerHttpSvc::create(serviceProvider(), worker);
    thread reqProcHttpSvrThread([reqProcHttpSvr]() { reqProcHttpSvr->run(); });

    auto const fileSvr = FileServer::create(serviceProvider(), worker);
    thread fileSvrThread([fileSvr]() { fileSvr->run(); });

    auto const ingestSvr = IngestSvc::create(serviceProvider(), worker);
    thread ingestSvrThread([ingestSvr]() { ingestSvr->run(); });

    auto const ingestHttpSvr = IngestHttpSvc::create(serviceProvider(), worker);
    thread ingestHttpSvrThread([ingestHttpSvr]() { ingestHttpSvr->run(); });

    auto const exportSvr = ExportServer::create(serviceProvider(), worker);
    thread exportSvrThread([exportSvr]() { exportSvr->run(); });

    // Keep sending periodic 'heartbeats' to the Registry service to report
    // a configuration and a status of the current worker.
    auto const config = serviceProvider()->config();
    while (true) {
        try {
            serviceProvider()->registry()->addWorker(worker);
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_WARN, context << "adding worker to the registry failed, ex: " << ex.what());
        }
        LOGS(_log, LOG_LVL_DEBUG,
             "HEARTBEAT" << "  worker: " << reqProcSvr->worker()
                         << "  processor.state: " << reqProcSvr->processor()->state2string()
                         << "  new, in-progress, finished: " << reqProcSvr->processor()->numNewRequests()
                         << ", " << reqProcSvr->processor()->numInProgressRequests() << ", "
                         << reqProcSvr->processor()->numFinishedRequests());
        this_thread::sleep_for(
                chrono::seconds(max(1U, config->get<unsigned int>("registry", "heartbeat-ival-sec"))));
    }
    reqProcSvrThread.join();
    fileSvrThread.join();
    ingestSvrThread.join();
    ingestHttpSvrThread.join();
    exportSvrThread.join();

    return 0;
}

void WorkerApp::_verifyCreateFolders() const {
    auto const config = serviceProvider()->config();
    vector<string> const folders = {config->get<string>("worker", "data-dir"),
                                    config->get<string>("worker", "loader-tmp-dir"),
                                    config->get<string>("worker", "exporter-tmp-dir"),
                                    config->get<string>("worker", "http-loader-tmp-dir")};
    FileUtils::verifyFolders("WORKER", folders, !_doNotCreateMissingFolders);
}

}  // namespace lsst::qserv::replica
