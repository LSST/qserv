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
#include "replica/jobs/CreateReplicaJob.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/qserv/QservMgtServices.h"
#include "replica/requests/StopRequest.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ErrorReporting.h"
#include "util/String.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace lsst::qserv::replica;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.CreateReplicaJob");

}  // namespace

namespace lsst::qserv::replica {

string CreateReplicaJob::typeName() { return "CreateReplicaJob"; }

CreateReplicaJob::Ptr CreateReplicaJob::create(string const& databaseFamily, unsigned int chunk,
                                               string const& sourceWorker, string const& destinationWorker,
                                               Controller::Ptr const& controller, string const& parentJobId,
                                               CallbackType const& onFinish, int priority) {
    return CreateReplicaJob::Ptr(new CreateReplicaJob(databaseFamily, chunk, sourceWorker, destinationWorker,
                                                      controller, parentJobId, onFinish, priority));
}

CreateReplicaJob::CreateReplicaJob(string const& databaseFamily, unsigned int chunk,
                                   string const& sourceWorker, string const& destinationWorker,
                                   Controller::Ptr const& controller, string const& parentJobId,
                                   CallbackType const& onFinish, int priority)
        : Job(controller, parentJobId, "CREATE_REPLICA", priority),
          _databaseFamily(databaseFamily),
          _chunk(chunk),
          _sourceWorker(sourceWorker),
          _destinationWorker(destinationWorker),
          _onFinish(onFinish) {}

CreateReplicaJobResult const& CreateReplicaJob::getReplicaData() const {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    if (state() == State::FINISHED) return _replicaData;
    throw logic_error("CreateReplicaJob::" + string(__func__) +
                      "  the method can't be called while the job hasn't finished");
}

list<pair<string, string>> CreateReplicaJob::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("database_family", databaseFamily());
    result.emplace_back("timeout_sec", to_string(chunk()));
    result.emplace_back("source_worker", sourceWorker());
    result.emplace_back("destination_worker", destinationWorker());
    return result;
}

list<pair<string, string>> CreateReplicaJob::persistentLogData() const {
    list<pair<string, string>> result;
    auto&& replicaData = getReplicaData();

    // Per-worker counters for the following categories:
    //
    //   created-chunks:
    //     the total number of chunks created on the workers as a result
    //     of the operation
    map<string, map<string, size_t>> workerCategoryCounter;
    for (auto&& info : replicaData.replicas) {
        workerCategoryCounter[info.worker()]["created-chunks"]++;
    }
    for (auto&& workerItr : workerCategoryCounter) {
        auto&& worker = workerItr.first;
        string val = "worker=" + worker;
        for (auto&& categoryItr : workerItr.second) {
            auto&& category = categoryItr.first;
            size_t const counter = categoryItr.second;
            val += " " + category + "=" + to_string(counter);
        }
        result.emplace_back("worker-stats", val);
    }
    return result;
}

void CreateReplicaJob::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << "startImpl");

    // Check if configuration parameters are valid
    auto const& config = controller()->serviceProvider()->config();
    if (not(config->isKnownDatabaseFamily(databaseFamily()) and config->isKnownWorker(sourceWorker()) and
            config->isKnownWorker(destinationWorker()) and (sourceWorker() != destinationWorker()))) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** MISCONFIGURED ** "
                       << " database family: '" << databaseFamily() << "'"
                       << " source worker: '" << sourceWorker() << "'"
                       << " destination worker: '" << destinationWorker() << "'");
        finish(lock, ExtendedState::CONFIG_ERROR);
        return;
    }

    // Make sure no such replicas exist yet at the destination
    vector<ReplicaInfo> destinationReplicas;
    try {
        controller()->serviceProvider()->databaseServices()->findWorkerReplicas(
                destinationReplicas, chunk(), destinationWorker(), databaseFamily());
    } catch (invalid_argument const& ex) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** MISCONFIGURED ** "
                       << " chunk: " << chunk() << " destinationWorker: " << destinationWorker()
                       << " databaseFamily: " << databaseFamily() << " exception: " << ex.what());
        throw;
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** failed to find replicas ** "
                       << " chunk: " << chunk() << " destinationWorker: " << destinationWorker()
                       << " databaseFamily: " << databaseFamily() << " exception: " << ex.what());
        finish(lock, ExtendedState::FAILED);
        return;
    }
    if (destinationReplicas.size()) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** destination worker already has "
                       << destinationReplicas.size() << " replicas ** "
                       << " chunk: " << chunk() << " destinationWorker: " << destinationWorker()
                       << " databaseFamily: " << databaseFamily());
        finish(lock, ExtendedState::FAILED);
        return;
    }

    // Get all databases for which this chunk is in the COMPLETE state on
    // at the source worker.
    //
    // Alternative options would be:
    // 1. launching requests for all databases of the family and then see
    //    filter them on a result status (something like FILE_ROPEN)
    //
    // 2. launching FindRequest for each member of the database family to
    //    see if the chunk is available on a source node.
    vector<ReplicaInfo> sourceReplicas;
    try {
        controller()->serviceProvider()->databaseServices()->findWorkerReplicas(
                sourceReplicas, chunk(), sourceWorker(), databaseFamily());

    } catch (invalid_argument const& ex) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** MISCONFIGURED ** "
                       << " chunk: " << chunk() << " sourceWorker: " << sourceWorker()
                       << " databaseFamily: " << databaseFamily() << " exception: " << ex.what());

        throw;
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** failed to find replicas ** "
                       << " chunk: " << chunk() << " sourceWorker: " << sourceWorker()
                       << " databaseFamily: " << databaseFamily() << " exception: " << ex.what());

        finish(lock, ExtendedState::FAILED);
        return;
    }
    if (not sourceReplicas.size()) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << string(__func__) << "  ** source worker has no replicas to be moved ** "
                       << " chunk: " << chunk() << " worker: " << sourceWorker());

        finish(lock, ExtendedState::FAILED);
        return;
    }

    // Launch the replication requests first. After (if) they all will
    // succeed the next optional stage will be launched to remove replicas
    // from the source worker.
    //
    // VERY IMPORTANT: the requests are sent for participating databases
    // only because some catalogs may not have a full coverage
    bool const keepTracking = true;
    for (auto&& replica : sourceReplicas) {
        _requests.push_back(ReplicationRequest::createAndStart(
                controller(), destinationWorker(), sourceWorker(), replica.database(), chunk(),
                [self = shared_from_base<CreateReplicaJob>()](ReplicationRequest::Ptr ptr) {
                    self->_onRequestFinish(ptr);
                },
                priority(), keepTracking, id()));
    }
}

void CreateReplicaJob::cancelImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // The algorithm will also clear resources taken by various
    // locally created objects.
    //
    // To ensure no lingering "side effects" will be left after cancelling this
    // job the request cancellation should be also followed (where it makes a sense)
    // by stopping the request at corresponding worker service.
    auto const noCallbackOnFinish = nullptr;
    bool const keepTracking = true;
    for (auto&& ptr : _requests) {
        ptr->cancel();
        if (ptr->state() != Request::State::FINISHED)
            StopRequest::createAndStart(controller(), destinationWorker(), ptr->id(), noCallbackOnFinish,
                                        priority(), keepTracking, id());
    }
    _requests.clear();
}

void CreateReplicaJob::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<CreateReplicaJob>(lock, _onFinish);
}

void CreateReplicaJob::_onRequestFinish(ReplicationRequest::Ptr const& request) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << string(__func__) << "(ReplicationeRequest)"
                   << "  database=" << request->database() << "  destinationWorker=" << destinationWorker()
                   << "  sourceWorker=" << sourceWorker() << "  chunk=" << chunk());

    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + string(__func__) + "(ReplicationeRequest)");
    if (state() == State::FINISHED) return;

    ++_numRequestsFinished;
    if (request->extendedState() == Request::ExtendedState::SUCCESS) {
        ++_numRequestsSuccess;
        _replicaData.replicas.push_back(request->responseData());
        _replicaData.chunks[chunk()][request->database()][destinationWorker()] = request->responseData();
    }

    // Evaluate the status of on-going operations to see if the replica creation
    // stage has finished.
    if (_numRequestsFinished == _requests.size()) {
        if (_numRequestsSuccess == _requests.size()) {
            // Notify Qserv about the change in a disposition of replicas.
            //
            // ATTENTION: only for ACTUALLY participating databases
            //
            // NOTE: The current implementation will not be affected by a result
            //       of the operation. Neither any upstream notifications will be
            //       sent to a requester of this job.
            vector<string> databases;
            for (auto&& databaseEntry : _replicaData.chunks[chunk()]) {
                databases.push_back(databaseEntry.first);
            }
            ServiceProvider::Ptr const serviceProvider = controller()->serviceProvider();
            if (serviceProvider->config()->get<unsigned int>("xrootd", "auto-notify") != 0) {
                _qservAddReplica(lock, chunk(), databases, destinationWorker());
            }
            finish(lock, ExtendedState::SUCCESS);
        } else {
            finish(lock, ExtendedState::FAILED);
        }
    }
}

void CreateReplicaJob::_qservAddReplica(replica::Lock const& lock, unsigned int chunk,
                                        vector<string> const& databases, string const& worker,
                                        AddReplicaQservMgtRequest::CallbackType const& onFinish) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "  ** START ** Qserv notification on ADD replica:"
                   << ", chunk=" << chunk << ", databases=" << util::String::toString(databases)
                   << "  worker=" << worker);
    controller()->serviceProvider()->qservMgtServices()->addReplica(
            chunk, databases, worker,
            [self = shared_from_this(), onFinish](AddReplicaQservMgtRequest::Ptr const& request) {
                LOGS(_log, LOG_LVL_DEBUG,
                     self->context() << __func__ << "  ** FINISH ** Qserv notification on ADD replica:"
                                     << "  chunk=" << request->chunk()
                                     << ", databases=" << util::String::toString(request->databases())
                                     << ", worker=" << request->workerName()
                                     << ", state=" << request->state2string());
                if (onFinish) onFinish(request);
            },
            id());
}
}  // namespace lsst::qserv::replica
