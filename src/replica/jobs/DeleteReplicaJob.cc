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
#include "replica/jobs/DeleteReplicaJob.h"

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

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DeleteReplicaJob");

}  // namespace

namespace lsst::qserv::replica {

string DeleteReplicaJob::typeName() { return "DeleteReplicaJob"; }

DeleteReplicaJob::Ptr DeleteReplicaJob::create(string const& databaseFamily, unsigned int chunk,
                                               string const& workerName, Controller::Ptr const& controller,
                                               string const& parentJobId, CallbackType const& onFinish,
                                               int priority) {
    return DeleteReplicaJob::Ptr(new DeleteReplicaJob(databaseFamily, chunk, workerName, controller,
                                                      parentJobId, onFinish, priority));
}

DeleteReplicaJob::DeleteReplicaJob(string const& databaseFamily, unsigned int chunk, string const& workerName,
                                   Controller::Ptr const& controller, string const& parentJobId,
                                   CallbackType const& onFinish, int priority)
        : Job(controller, parentJobId, "DELETE_REPLICA", priority),
          _databaseFamily(databaseFamily),
          _chunk(chunk),
          _workerName(workerName),
          _onFinish(onFinish) {}

DeleteReplicaJobResult const& DeleteReplicaJob::getReplicaData() const {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    if (state() == State::FINISHED) return _replicaData;
    throw logic_error("DeleteReplicaJob::" + string(__func__) +
                      "  the method can't be called while the job hasn't finished");
}

list<pair<string, string>> DeleteReplicaJob::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("database_family", databaseFamily());
    result.emplace_back("chunk", to_string(chunk()));
    result.emplace_back("worker", workerName());
    return result;
}

list<pair<string, string>> DeleteReplicaJob::persistentLogData() const {
    list<pair<string, string>> result;
    auto&& replicaData = getReplicaData();

    // Per-worker counters for the following categories:
    //
    //   deleted-chunks:
    //     the total number of chunks deleted from the workers as a result
    //     of the operation
    map<string, map<string, size_t>> workerCategoryCounter;
    for (auto&& info : replicaData.replicas) {
        workerCategoryCounter[info.worker()]["deleted-chunks"]++;
    }
    for (auto&& workerItr : workerCategoryCounter) {
        auto&& workerName = workerItr.first;
        string val = "worker=" + workerName;
        for (auto&& categoryItr : workerItr.second) {
            auto&& category = categoryItr.first;
            size_t const counter = categoryItr.second;
            val += " " + category + "=" + to_string(counter);
        }
        result.emplace_back("worker-stats", val);
    }
    return result;
}

void DeleteReplicaJob::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);

    // Check if configuration parameters are valid
    auto const& config = controller()->serviceProvider()->config();
    if (not(config->isKnownDatabaseFamily(databaseFamily()) and config->isKnownWorker(workerName()))) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << __func__ << "  ** MISCONFIGURED ** "
                       << " database family: '" << databaseFamily() << "'"
                       << " worker: '" << workerName() << "'");

        finish(lock, ExtendedState::CONFIG_ERROR);
        return;
    }

    // Get all databases for which this chunk is in the COMPLETE state on
    // at the worker.
    //
    // Alternative options would be:
    // 1. launching requests for all databases of the family and then
    //    filter requests based on a result status (something like FILE_ROPEN)
    //
    // 2. launching FindRequest for each member of the database family to
    //    see if the chunk is available on a source node.
    try {
        controller()->serviceProvider()->databaseServices()->findWorkerReplicas(
                _replicas, chunk(), workerName(), databaseFamily());
    } catch (invalid_argument const& ex) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << __func__ << "  ** MISCONFIGURED ** "
                       << " chunk: " << chunk() << " worker: " << workerName()
                       << " databaseFamily: " << databaseFamily() << " exception: " << ex.what());
        finish(lock, ExtendedState::CONFIG_ERROR);
        return;

    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << __func__ << "  ** failed to find replicas ** "
                       << " chunk: " << chunk() << " worker: " << workerName()
                       << " databaseFamily: " << databaseFamily() << " exception: " << ex.what());
        finish(lock, ExtendedState::FAILED);
        return;
    }
    if (not _replicas.size()) {
        LOGS(_log, LOG_LVL_ERROR,
             context() << __func__ << "  ** worker has no replicas to be deleted ** "
                       << " chunk: " << chunk() << " worker: " << workerName()
                       << " databaseFamily: " << databaseFamily());
        finish(lock, ExtendedState::FAILED);
        return;
    }

    // Notify Qserv about the change in a disposition of replicas
    // if the notification is required before actually deleting the replica.
    // ATTENTION: only for ACTUALLY participating databases
    ServiceProvider::Ptr const serviceProvider = controller()->serviceProvider();
    if (serviceProvider->config()->get<unsigned int>("xrootd", "auto-notify") != 0) {
        // Start right away
        _beginDeleteReplica(lock);
    } else {
        // Notify Qserv first. Then start once a confirmation is received
        vector<string> databases;
        for (auto&& replica : _replicas) {
            databases.push_back(replica.database());
        }

        // Force the removal regardless of the replica usage status.
        // See the implementation of the corresponding worker management service
        // for specific detail on what "remove" means in that service's context.
        bool const force = true;
        _qservRemoveReplica(lock, chunk(), databases, workerName(), force,
                            [self = shared_from_base<DeleteReplicaJob>()](
                                    RemoveReplicaQservMgtRequest::Ptr const& request) {
                                replica::Lock lock(self->_mtx, self->context() + string(__func__) +
                                                                       "::qservRemoveReplica");
                                switch (request->extendedState()) {
                                    case QservMgtRequest::ExtendedState::SUCCESS:
                                        // If there is a solid confirmation from Qserv on source node that the
                                        // replica is not being used and it won't be used then it's safe to
                                        // proceed with the second stage of requests to actually eliminate
                                        // replica's files from the source worker.
                                        self->_beginDeleteReplica(lock);
                                        return;
                                    case QservMgtRequest::ExtendedState::SERVER_CHUNK_IN_USE:
                                        // Otherwise set an appropriate status of the operation, finish them
                                        // job and notify the caller.
                                        self->finish(lock, ExtendedState::QSERV_CHUNK_IN_USE);
                                        break;
                                    default:
                                        self->finish(lock, ExtendedState::QSERV_FAILED);
                                        break;
                                }
                            });
    }
}

void DeleteReplicaJob::cancelImpl(replica::Lock const& lock) {
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
        if (ptr->state() != Request::State::FINISHED) {
            StopRequest::createAndStart(controller(), workerName(), ptr->id(), noCallbackOnFinish, priority(),
                                        keepTracking, id());
        }
    }
    _requests.clear();
}

void DeleteReplicaJob::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<DeleteReplicaJob>(lock, _onFinish);
}

void DeleteReplicaJob::_beginDeleteReplica(replica::Lock const& lock) {
    // VERY IMPORTANT: the requests are sent for participating databases
    // only because some catalogs may not have a full coverage
    bool const keepTracking = true;
    for (auto&& replica : _replicas) {
        _requests.push_back(DeleteRequest::createAndStart(
                controller(), workerName(), replica.database(), chunk(),
                [self = shared_from_base<DeleteReplicaJob>()](DeleteRequest::Ptr ptr) {
                    self->_onRequestFinish(ptr);
                },
                priority(), keepTracking, id()));
    }
}

void DeleteReplicaJob::_onRequestFinish(DeleteRequest::Ptr const& request) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "(DeleteRequest)"
                   << "  database=" << request->database() << "  worker=" << workerName()
                   << "  chunk=" << chunk());

    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;

    ++_numRequestsFinished;
    if (request->extendedState() == Request::ExtendedState::SUCCESS) {
        ++_numRequestsSuccess;
        _replicaData.replicas.push_back(request->responseData());
        _replicaData.chunks[chunk()][request->database()][workerName()] = request->responseData();
    }

    // Evaluate the status of on-going operations to see if the job
    // has finished.

    if (_numRequestsFinished == _requests.size()) {
        finish(lock,
               _numRequestsSuccess == _requests.size() ? ExtendedState::SUCCESS : ExtendedState::FAILED);
    }
}
void DeleteReplicaJob::_qservRemoveReplica(replica::Lock const& lock, unsigned int chunk,
                                           vector<string> const& databases, string const& workerName,
                                           bool force,
                                           RemoveReplicaQservMgtRequest::CallbackType const& onFinish) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << "  ** START ** Qserv notification on REMOVE replica:"
                   << "  chunk=" << chunk << ", databases=" << util::String::toString(databases)
                   << ", worker=" << workerName << ", force=" << (force ? "true" : "false"));

    controller()->serviceProvider()->qservMgtServices()->removeReplica(
            chunk, databases, workerName, force,
            [self = shared_from_this(), onFinish](RemoveReplicaQservMgtRequest::Ptr const& request) {
                LOGS(_log, LOG_LVL_DEBUG,
                     self->context() << __func__ << "  ** FINISH ** Qserv notification on REMOVE replica:"
                                     << "  chunk=" << request->chunk()
                                     << ", databases=" << util::String::toString(request->databases())
                                     << ", worker=" << request->workerName()
                                     << ", force=" << (request->force() ? "true" : "false")
                                     << ", state=" << request->state2string());
                if (onFinish) onFinish(request);
            },
            id());
}

}  // namespace lsst::qserv::replica
