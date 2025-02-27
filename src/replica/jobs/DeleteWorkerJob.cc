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
#include "replica/jobs/DeleteWorkerJob.h"

// System headers
#include <algorithm>
#include <stdexcept>
#include <tuple>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/requests/ServiceManagementRequest.h"
#include "replica/requests/StopRequest.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ErrorReporting.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DeleteWorkerJob");

}  // namespace

namespace lsst::qserv::replica {

string DeleteWorkerJob::typeName() { return "DeleteWorkerJob"; }

DeleteWorkerJob::Ptr DeleteWorkerJob::create(string const& workerName, bool permanentDelete,
                                             Controller::Ptr const& controller, string const& parentJobId,
                                             CallbackType const& onFinish, int priority) {
    return DeleteWorkerJob::Ptr(
            new DeleteWorkerJob(workerName, permanentDelete, controller, parentJobId, onFinish, priority));
}

DeleteWorkerJob::DeleteWorkerJob(string const& workerName, bool permanentDelete,
                                 Controller::Ptr const& controller, string const& parentJobId,
                                 CallbackType const& onFinish, int priority)
        : Job(controller, parentJobId, "DELETE_WORKER", priority),
          _workerName(workerName),
          _permanentDelete(permanentDelete),
          _onFinish(onFinish) {}

DeleteWorkerJobResult const& DeleteWorkerJob::getReplicaData() const {
    LOGS(_log, LOG_LVL_DEBUG, context());
    if (state() == State::FINISHED) return _replicaData;
    throw logic_error("DeleteWorkerJob::" + string(__func__) +
                      "  the method can't be called while the job hasn't finished");
}

list<pair<string, string>> DeleteWorkerJob::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("worker", workerName());
    result.emplace_back("permanent_delete", bool2str(permanentDelete()));
    return result;
}

list<pair<string, string>> DeleteWorkerJob::persistentLogData() const {
    list<pair<string, string>> result;

    // Encode new chunk replicas (if any) which had to be created to compensate
    // for lost ones.
    auto&& replicaData = getReplicaData();
    for (auto&& familyChunkDatabaseWorkerInfo : replicaData.chunks) {
        auto&& family = familyChunkDatabaseWorkerInfo.first;
        for (auto&& chunkDatabaseWorkerInfo : familyChunkDatabaseWorkerInfo.second) {
            auto&& chunk = chunkDatabaseWorkerInfo.first;
            for (auto&& databaseWorkerInfo : chunkDatabaseWorkerInfo.second) {
                auto&& database = databaseWorkerInfo.first;
                for (auto&& workerInfo : databaseWorkerInfo.second) {
                    auto&& workerName = workerInfo.first;
                    result.emplace_back("new-replica", "family=" + family + " chunk=" + to_string(chunk) +
                                                               " database=" + database +
                                                               " worker=" + workerName);
                }
            }
        }
    }

    // Encode orphan replicas (if any) which only existed on the evicted worker
    for (auto&& chunkDatabaseReplicaInfo : replicaData.orphanChunks) {
        auto&& chunk = chunkDatabaseReplicaInfo.first;
        for (auto&& databaseReplicaInfo : chunkDatabaseReplicaInfo.second) {
            auto&& database = databaseReplicaInfo.first;
            result.emplace_back("orphan-replica", "chunk=" + to_string(chunk) + " database=" + database);
        }
    }
    return result;
}

void DeleteWorkerJob::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    auto const noCallbackOnFinish = nullptr;
    unsigned int requestExpirationIvalSec = 60;

    // Check the status of the worker service, and if it's still running
    // try to get as much info from it as possible
    auto const statusRequest = ServiceStatusRequest::createAndStart(
            controller(), workerName(), noCallbackOnFinish, priority(), id(), requestExpirationIvalSec);
    statusRequest->wait();

    if (statusRequest->extendedState() == Request::ExtendedState::SUCCESS) {
        if (statusRequest->getServiceState().state == ServiceState::State::RUNNING) {
            // Make sure the service won't be executing any other "leftover"
            // requests which may be interfering with the current job's requests
            auto const drainRequest =
                    ServiceDrainRequest::createAndStart(controller(), workerName(), noCallbackOnFinish,
                                                        priority(), id(), requestExpirationIvalSec);
            drainRequest->wait();
            if (drainRequest->extendedState() == Request::ExtendedState::SUCCESS) {
                if (drainRequest->getServiceState().state == ServiceState::State::RUNNING) {
                    // Try to get the most recent state the worker's replicas
                    // for all known databases
                    bool const saveReplicaInfo = true;  // always save the replica info in a database because
                                                        // the algorithm depends on it.
                    for (auto&& database : controller()->serviceProvider()->config()->databases()) {
                        auto const request = FindAllRequest::createAndStart(
                                controller(), workerName(), database, saveReplicaInfo,
                                [self = shared_from_base<DeleteWorkerJob>()](
                                        FindAllRequest::Ptr const& request) {
                                    self->_onRequestFinish(request);
                                },
                                priority());
                        _findAllRequests.push_back(request);
                        _numLaunched++;
                    }
                    // The rest will be happening in a method processing the completion
                    // of the above launched requests.
                    return;
                }
            }
        }
    }

    // Since the worker is not available then go straight to a point
    // at which we'll be changing its state within the replication system
    _disableWorker(lock);
}

void DeleteWorkerJob::cancelImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // To ensure no lingering "side effects" will be left after cancelling this
    // job the request cancellation should be also followed (where it makes a sense)
    // by stopping the request at corresponding worker service.
    auto const noCallbackOnFinish = nullptr;
    auto const keepTracking = true;
    for (auto&& ptr : _findAllRequests) {
        ptr->cancel();
        if (ptr->state() != Request::State::FINISHED) {
            StopRequest::createAndStart(controller(), ptr->workerName(), ptr->id(), noCallbackOnFinish,
                                        priority(), keepTracking, id());
        }
    }

    // Stop chained jobs (if any) as well
    for (auto&& ptr : _replicateJobs) ptr->cancel();
}

void DeleteWorkerJob::_onRequestFinish(FindAllRequest::Ptr const& request) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "  worker=" << request->workerName()
                   << "  database=" << request->database());

    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    _numFinished++;
    if (request->extendedState() == Request::ExtendedState::SUCCESS) _numSuccess++;

    // Evaluate the status of on-going operations to see if the job
    // has finished. If so then proceed to the next stage of the job.
    //
    // ATTENTION: we don't care about the completion status of the requests
    // because they're related to a worker which is going to be removed, and
    // this worker may already be experiencing problems.
    if (_numFinished == _numLaunched) _disableWorker(lock);
}

void DeleteWorkerJob::_disableWorker(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Temporary disable this worker from the configuration. If it's requested
    // to be permanently deleted this will be done only after all other relevant
    // operations of this job will be done.
    controller()->serviceProvider()->config()->disableWorker(workerName());

    // Launch chained jobs to ensure the minimal replication level
    // which might be affected by the worker removal.
    _numLaunched = 0;
    _numFinished = 0;
    _numSuccess = 0;
    for (auto&& databaseFamily : controller()->serviceProvider()->config()->databaseFamilies()) {
        ReplicateJob::Ptr const job = ReplicateJob::create(
                databaseFamily, 0, /* numReplicas -- pull from Configuration */
                controller(), id(),
                [self = shared_from_base<DeleteWorkerJob>()](ReplicateJob::Ptr job) {
                    self->_onJobFinish(job);
                },
                priority());
        job->start();
        _replicateJobs.push_back(job);
        _numLaunched++;
    }
}

void DeleteWorkerJob::_onJobFinish(ReplicateJob::Ptr const& job) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "(ReplicateJob) "
                   << " databaseFamily: " << job->databaseFamily() << " numReplicas: " << job->numReplicas()
                   << " state: " << job->state2string());

    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + string(__func__) + "(ReplicateJob)");
    if (state() == State::FINISHED) return;

    _numFinished++;
    if (job->extendedState() != ExtendedState::SUCCESS) {
        finish(lock, ExtendedState::FAILED);
        return;
    }

    // Process the normal completion of the child job
    _numSuccess++;
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "(ReplicateJob)  "
                   << "job->getReplicaData().chunks.size(): " << job->getReplicaData().chunks.size());

    // Merge results into the current job's result object
    _replicaData.chunks[job->databaseFamily()] = job->getReplicaData().chunks;
    if (_numFinished == _numLaunched) {
        // Construct a collection of orphan replicas if possible
        ReplicaInfoCollection replicas;
        try {
            controller()->serviceProvider()->databaseServices()->findWorkerReplicas(replicas, workerName());
            for (ReplicaInfo const& replica : replicas) {
                unsigned int const chunk = replica.chunk();
                string const& database = replica.database();
                bool replicated = false;
                for (auto&& databaseFamilyEntry : _replicaData.chunks) {
                    auto const& chunks = databaseFamilyEntry.second;
                    replicated = replicated or (chunks.count(chunk) and chunks.at(chunk).count(database));
                }
                if (not replicated) {
                    _replicaData.orphanChunks[chunk][database] = replica;
                }
            }
        } catch (invalid_argument const& ex) {
            LOGS(_log, LOG_LVL_ERROR,
                 context() << __func__ << "(ReplicateJob)  "
                           << "** MISCONFIGURED ** "
                           << " worker: " << workerName() << " exception: " << ex.what());
            throw;
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_ERROR,
                 context() << __func__ << "(ReplicateJob)"
                           << "  ** failed to find replicas ** "
                           << " worker: " << workerName() << " exception: " << ex.what());
            finish(lock, ExtendedState::FAILED);
            return;
        }

        // TODO: if the list of orphan chunks is not empty then consider bringing
        // back the disabled worker (if the service still responds) in the read-only
        // mode and try using it for redistributing those chunks across the cluster.
        //
        // NOTE: this could be a complicated procedure which needs to be thought
        // through.
        ;

        // Do this only if requested, and only in case of the successful
        // completion of the job
        if (permanentDelete()) {
            controller()->serviceProvider()->config()->deleteWorker(workerName());
        }
        finish(lock, ExtendedState::SUCCESS);
    }
}

void DeleteWorkerJob::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<DeleteWorkerJob>(lock, _onFinish);
}

}  // namespace lsst::qserv::replica
