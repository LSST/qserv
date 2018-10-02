/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
#include "replica/SetReplicasQservMgtRequest.h"

// System headers
#include <future>
#include <set>
#include <stdexcept>

// Third party headers
#include "XrdSsi/XrdSsiProvider.hh"
#include "XrdSsi/XrdSsiService.hh"

// Qserv headers
#include "global/ResourceUnit.h"
#include "lsst/log/Log.h"
#include "replica/Configuration.h"
#include "replica/ServiceProvider.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.SetReplicasQservMgtRequest");

} /// namespace

namespace lsst {
namespace qserv {
namespace replica {

SetReplicasQservMgtRequest::Ptr SetReplicasQservMgtRequest::create(
                                        ServiceProvider::Ptr const& serviceProvider,
                                        std::string const& worker,
                                        QservReplicaCollection const& newReplicas,
                                        bool force,
                                        SetReplicasQservMgtRequest::CallbackType onFinish) {
    return SetReplicasQservMgtRequest::Ptr(
        new SetReplicasQservMgtRequest(serviceProvider,
                                       worker,
                                       newReplicas,
                                       force,
                                       onFinish));
 }

SetReplicasQservMgtRequest::SetReplicasQservMgtRequest(
                                ServiceProvider::Ptr const& serviceProvider,
                                std::string const& worker,
                                QservReplicaCollection const& newReplicas,
                                bool force,
                                SetReplicasQservMgtRequest::CallbackType onFinish)
    :   QservMgtRequest(serviceProvider,
                        "QSERV_SET_REPLICAS",
                        worker),
        _newReplicas(newReplicas),
        _force(force),
        _onFinish(onFinish),
        _qservRequest(nullptr) {
}

QservReplicaCollection const& SetReplicasQservMgtRequest::replicas() const {
    if (not ((state() == State::FINISHED) and (extendedState() == ExtendedState::SUCCESS))) {
        throw std::logic_error(
                "SetReplicasQservMgtRequest::replicas  replicas aren't available in state: " +
                state2string(state(), extendedState()));
    }
    return _replicas;
}

std::map<std::string,std::string> SetReplicasQservMgtRequest::extendedPersistentState() const {
    std::map<std::string,std::string> result;
    for (auto&& replica: newReplicas()) {
        result["replica"] = replica.database + ":" + std::to_string(replica.chunk);
    }
    result["force"] = force() ? "1" : "0";
    return result;
}

void SetReplicasQservMgtRequest::setReplicas(
            util::Lock const& lock,
            wpublish::SetChunkListQservRequest::ChunkCollection const& collection) {

    _replicas.clear();
    for (auto&& replica: collection) {
        _replicas.push_back(
            QservReplica{
                replica.chunk,
                replica.database,
                replica.use_count
            }
        );
    }
}

void SetReplicasQservMgtRequest::startImpl(util::Lock const& lock) {

    wpublish::SetChunkListQservRequest::ChunkCollection chunks;
    for (auto&& chunkEntry: newReplicas()) {
        chunks.push_back(
            wpublish::SetChunkListQservRequest::Chunk{
                chunkEntry.chunk,
                chunkEntry.database,
                0  /* UNUSED: use_count */
            }
        );
    }
    auto const request = shared_from_base<SetReplicasQservMgtRequest>();

    _qservRequest = wpublish::SetChunkListQservRequest::create(
        chunks,
        force(),
        [request] (wpublish::SetChunkListQservRequest::Status status,
                   std::string const& error,
                   wpublish::SetChunkListQservRequest::ChunkCollection const& collection) {

            // IMPORTANT: the final state is required to be tested twice. The first time
            // it's done in order to avoid deadlock on the "in-flight" callbacks reporting
            // their completion while the request termination is in a progress. And the second
            // test is made after acquering the lock to recheck the state in case if it
            // has transitioned while acquering the lock.

            if (request->state() == State::FINISHED) return;
        
            util::Lock lock(request->_mtx, request->context() + "startImpl[callback]");
        
            if (request->state() == State::FINISHED) return;

            switch (status) {
                case wpublish::SetChunkListQservRequest::Status::SUCCESS:
                    request->setReplicas(lock, collection);
                    request->finish(lock, QservMgtRequest::ExtendedState::SUCCESS);
                    break;

                case wpublish::SetChunkListQservRequest::Status::ERROR:
                    request->finish(lock, QservMgtRequest::ExtendedState::SERVER_ERROR, error);
                    break;

                default:
                    throw std::logic_error(
                        "SetReplicasQservMgtRequest:  unhandled server status: " +
                        wpublish::SetChunkListQservRequest::status2str(status));
            }
        }
    );
    XrdSsiResource resource(ResourceUnit::makeWorkerPath(worker()));
    service()->ProcessRequest(*_qservRequest, resource);
}

void SetReplicasQservMgtRequest::finishImpl(util::Lock const& lock) {

    switch (extendedState()) {

        case ExtendedState::CANCELLED:
        case ExtendedState::TIMEOUT_EXPIRED:

            // And if the SSI request is still around then tell it to stop

            if (_qservRequest) {
                bool const cancel = true;
                _qservRequest->Finished(cancel);
            }
            break;

        default:
            break;
    }
    _qservRequest = nullptr;
}

void SetReplicasQservMgtRequest::notify(util::Lock const& lock) {

    LOGS(_log, LOG_LVL_DEBUG, context() << "notify");

    if (nullptr != _onFinish) {

        // Clearing the stored callback after finishing the up-stream notification
        // has two purposes:
        //
        // 1. it guaranties (exactly) one time notification
        // 2. it breaks the up-stream dependency on a caller object if a shared
        //    pointer to the object was mentioned as the lambda-function's closure

        serviceProvider()->io_service().post(
            std::bind(
                std::move(_onFinish),
                shared_from_base<SetReplicasQservMgtRequest>()
            )
        );
        _onFinish = nullptr;
    }
}
}}} // namespace lsst::qserv::replica
