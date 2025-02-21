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
#include "replica/contr/ReplicationTask.h"

// System headers
#include <vector>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/jobs/FindAllJob.h"
#include "replica/jobs/FixUpJob.h"
#include "replica/jobs/ReplicateJob.h"
#include "replica/jobs/RebalanceJob.h"
#include "replica/jobs/PurgeJob.h"
#include "replica/mysql/DatabaseMySQL.h"
#include "replica/mysql/DatabaseMySQLGenerator.h"
#include "replica/mysql/DatabaseMySQLUtils.h"
#include "replica/services/DatabaseServices.h"
#include "replica/util/ReplicaInfo.h"

using namespace std;

namespace lsst::qserv::replica {

using namespace database::mysql;

ReplicationTask::Ptr ReplicationTask::create(Controller::Ptr const& controller,
                                             Task::AbnormalTerminationCallbackType const& onTerminated,
                                             unsigned int qservSyncTimeoutSec, bool disableQservSync,
                                             bool forceQservSync, bool qservChunkMapUpdate,
                                             unsigned int replicationIntervalSec, bool purge) {
    return Ptr(new ReplicationTask(controller, onTerminated, qservSyncTimeoutSec, disableQservSync,
                                   forceQservSync, qservChunkMapUpdate, replicationIntervalSec, purge));
}

bool ReplicationTask::onRun() {
    bool const saveReplicaInfo = true;
    bool const allWorkers = false;
    unsigned int const numReplicas = 0;  // Always assume the curently configured level for each family
    int const priority =
            serviceProvider()->config()->get<int>("controller", "catalog-management-priority-level");

    launch<FindAllJob>(priority, saveReplicaInfo, allWorkers);
    if (!_disableQservSync) sync(_qservSyncTimeoutSec, _forceQservSync);

    if (_qservChunkMapUpdate) _updateChunkMap();

    launch<FixUpJob>(priority);
    if (!_disableQservSync) sync(_qservSyncTimeoutSec, _forceQservSync);

    launch<ReplicateJob>(priority, numReplicas);
    if (!_disableQservSync) sync(_qservSyncTimeoutSec, _forceQservSync);

    bool const estimateOnly = false;
    launch<RebalanceJob>(priority, estimateOnly);
    if (!_disableQservSync) sync(_qservSyncTimeoutSec, _forceQservSync);

    if (_purge) {
        launch<PurgeJob>(priority, numReplicas);
        if (!_disableQservSync) sync(_qservSyncTimeoutSec, _forceQservSync);
    }

    // Keep on getting calls on this method after a wait time
    return true;
}

ReplicationTask::ReplicationTask(Controller::Ptr const& controller,
                                 Task::AbnormalTerminationCallbackType const& onTerminated,
                                 unsigned int qservSyncTimeoutSec, bool disableQservSync, bool forceQservSync,
                                 bool qservChunkMapUpdate, unsigned int replicationIntervalSec, bool purge)
        : Task(controller, "REPLICATION-THREAD  ", onTerminated, replicationIntervalSec),
          _qservSyncTimeoutSec(qservSyncTimeoutSec),
          _disableQservSync(disableQservSync),
          _forceQservSync(forceQservSync),
          _qservChunkMapUpdate(qservChunkMapUpdate),
          _purge(purge) {}

void ReplicationTask::_updateChunkMap() {
    // Open MySQL connection using the RAII-style handler that would automatically
    // abort the transaction should any problem occured when loading data into the table.
    ConnectionHandler h;
    try {
        h.conn = Connection::open(Configuration::qservCzarDbParams("qservMeta"));
    } catch (exception const& ex) {
        error("failed to connect to the czar's database server, ex: " + string(ex.what()));
        return;
    }
    QueryGenerator const g(h.conn);

    // Get info on known chunk replicas from the persistent store of the Replication system
    // and package those into ready-to-ingest data.
    bool const allDatabases = true;
    string const emptyDatabaseFilter;
    bool const isPublished = true;
    bool const includeFileInfo = true;  // need this to access tables sizes
    vector<string> rows;
    for (auto const& workerName : serviceProvider()->config()->workers()) {
        vector<ReplicaInfo> replicas;
        serviceProvider()->databaseServices()->findWorkerReplicas(replicas, workerName, emptyDatabaseFilter,
                                                                  allDatabases, isPublished, includeFileInfo);
        for (auto const& replica : replicas) {
            for (auto const& fileInfo : replica.fileInfo()) {
                if (fileInfo.isData() && !fileInfo.isOverlap()) {
                    rows.push_back(g.packVals(workerName, replica.database(), fileInfo.baseTable(),
                                              replica.chunk(), fileInfo.size));
                }
            }
        }
    }
    if (rows.empty()) {
        warn("no replicas found in the persistent state of the Replication system");
        return;
    }

    // Get the limit for the length of the bulk insert queries. The limit is needed
    // to run the query generation.
    size_t maxQueryLength = 0;
    string const globalVariableName = "max_allowed_packet";
    try {
        string const query = g.showVars(SqlVarScope::GLOBAL, globalVariableName);
        h.conn->executeInOwnTransaction([&query, &maxQueryLength](auto conn) {
            bool const noMoreThanOne = true;
            if (!selectSingleValue(conn, query, maxQueryLength, "Value", noMoreThanOne)) {
                throw runtime_error("no such variable found");
            }
        });
    } catch (exception const& ex) {
        error("failed to get a value of GLOBAL '" + globalVariableName + "', ex: " + string(ex.what()));
        return;
    }

    // Execute a sequence of queries atomically
    vector<string> const deleteQueries = {g.delete_("chunkMap"), g.delete_("chunkMapStatus")};
    vector<string> insertQueries = g.insertPacked(
            "chunkMap", g.packIds("worker", "database", "table", "chunk", "size"), rows, maxQueryLength);
    insertQueries.push_back(g.insert("chunkMapStatus", Sql::NOW));
    try {
        h.conn->executeInOwnTransaction([&deleteQueries, &insertQueries](auto conn) {
            for (auto const& query : deleteQueries) conn->execute(query);
            for (auto const& query : insertQueries) conn->execute(query);
        });
    } catch (exception const& ex) {
        error("failed to update chunk map in the Czar database, ex: " + string(ex.what()));
        return;
    }
}

}  // namespace lsst::qserv::replica
