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
#include "replica/worker/WorkerHttpRequest.h"

// System headers
#include <stdexcept>

// Third party headers
#include "boost/date_time/posix_time/posix_time.hpp"

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/services/ServiceProvider.h"

// LSST headers
#include "lsst/log/Log.h"

#define CONTEXT context("WorkerHttpRequest", __func__)

using namespace std;
using namespace std::placeholders;
using json = nlohmann::json;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerHttpRequest");
}  // namespace

namespace lsst::qserv::replica {

replica::Mutex WorkerHttpRequest::_mtxDataFolderOperations;

atomic<size_t> WorkerHttpRequest::_numInstances{0};

WorkerHttpRequest::WorkerHttpRequest(shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
                                     string const& type, protocol::QueuedRequestHdr const& hdr,
                                     json const& req, ExpirationCallbackType const& onExpired)
        : _serviceProvider(serviceProvider),
          _worker(worker),
          _type(type),
          _hdr(hdr),
          _req(req),
          _onExpired(onExpired),
          _expirationTimeoutSec(hdr.timeout == 0 ? serviceProvider->config()->get<unsigned int>(
                                                           "controller", "request-timeout-sec")
                                                 : hdr.timeout),
          _expirationTimer(serviceProvider->io_service()),
          _status(protocol::Status::CREATED),
          _extendedStatus(protocol::StatusExt::NONE),
          _performance() {
    _numInstances++;
    LOGS(_log, LOG_LVL_TRACE, CONTEXT << " numInstances: " << _numInstances);
}

WorkerHttpRequest::~WorkerHttpRequest() {
    _numInstances--;
    LOGS(_log, LOG_LVL_TRACE, CONTEXT << " numInstances: " << _numInstances);
    dispose();
}

void WorkerHttpRequest::checkIfCancelling(replica::Lock const& lock, string const& context_) {
    switch (status()) {
        case protocol::Status::IN_PROGRESS:
            break;
        case protocol::Status::IS_CANCELLING:
            setStatus(lock, protocol::Status::CANCELLED);
            throw WorkerHttpRequestCancelled();
        default:
            throw logic_error(CONTEXT + " not allowed while in status: " + protocol::toString(status()));
    }
}

WorkerHttpRequest::ErrorContext WorkerHttpRequest::reportErrorIf(bool errorCondition,
                                                                 protocol::StatusExt extendedStatus,
                                                                 string const& errorMsg) {
    WorkerHttpRequest::ErrorContext errorContext;
    if (errorCondition) {
        errorContext.failed = true;
        errorContext.extendedStatus = extendedStatus;
        LOGS(_log, LOG_LVL_ERROR, CONTEXT << " execute" << errorMsg);
    }
    return errorContext;
}

void WorkerHttpRequest::init() {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);
    replica::Lock lock(_mtx, CONTEXT);
    if (status() != protocol::Status::CREATED) return;

    // Start the expiration timer
    if (_expirationTimeoutSec != 0) {
        _expirationTimer.cancel();
        _expirationTimer.expires_from_now(boost::posix_time::seconds(_expirationTimeoutSec));
        _expirationTimer.async_wait(bind(&WorkerHttpRequest::_expired, shared_from_this(), _1));
        LOGS(_log, LOG_LVL_TRACE,
             CONTEXT << " started timer with _expirationTimeoutSec: " << _expirationTimeoutSec);
    }
}

void WorkerHttpRequest::start() {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);
    replica::Lock lock(_mtx, CONTEXT);
    switch (status()) {
        case protocol::Status::CREATED:
            setStatus(lock, protocol::Status::IN_PROGRESS);
            break;
        default:
            throw logic_error(CONTEXT + " not allowed while in status: " + protocol::toString(status()));
    }
}

void WorkerHttpRequest::cancel() {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);
    replica::Lock lock(_mtx, CONTEXT);
    switch (status()) {
        case protocol::Status::QUEUED:
        case protocol::Status::CREATED:
        case protocol::Status::CANCELLED:
            setStatus(lock, protocol::Status::CANCELLED);
            break;
        case protocol::Status::IN_PROGRESS:
        case protocol::Status::IS_CANCELLING:
            setStatus(lock, protocol::Status::IS_CANCELLING);
            break;

        // Nothing to be done to the completed requests
        case protocol::Status::SUCCESS:
        case protocol::Status::BAD:
        case protocol::Status::FAILED:
            break;
    }
}

void WorkerHttpRequest::rollback() {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);
    replica::Lock lock(_mtx, CONTEXT);
    switch (status()) {
        case protocol::Status::CREATED:
        case protocol::Status::IN_PROGRESS:
            setStatus(lock, protocol::Status::CREATED);
            break;
        case protocol::Status::IS_CANCELLING:
            setStatus(lock, protocol::Status::CANCELLED);
            throw WorkerHttpRequestCancelled();
            break;
        default:
            throw logic_error(CONTEXT + " not allowed while in status: " + protocol::toString(status()));
    }
}

void WorkerHttpRequest::stop() {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);
    replica::Lock lock(_mtx, CONTEXT);
    setStatus(lock, protocol::Status::CREATED);
}

void WorkerHttpRequest::dispose() noexcept {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);
    replica::Lock lock(_mtx, CONTEXT);
    if (_expirationTimeoutSec != 0) {
        try {
            _expirationTimer.cancel();
        } catch (exception const& ex) {
            LOGS(_log, LOG_LVL_WARN,
                 CONTEXT << " request expiration couldn't be cancelled, ex: " << ex.what());
        }
    }
}

json WorkerHttpRequest::toJson(bool includeResultIfFinished) const {
    LOGS(_log, LOG_LVL_TRACE, CONTEXT);

    // IMPORTANT: the lock is not needed here because the data read by the method
    // are safe to read w/o any synchronization. The only exception is the results
    // which is not a problem since results are only read after the request is finished.

    json response = _hdr.toJson();
    response["req"] = _req;
    response["type"] = _type;
    response["status"] = _status.load();
    response["status_str"] = protocol::toString(_status.load());
    response["status_ext"] = _extendedStatus.load();
    response["status_ext_str"] = protocol::toString(_extendedStatus.load());
    response["expiration_timeout_sec"] = _expirationTimeoutSec;
    response["performance"] = _performance.toJson();
    response["result"] = json::object();
    if (includeResultIfFinished && _status == protocol::Status::SUCCESS) {
        getResult(response["result"]);
    }
    return response;
}

string WorkerHttpRequest::context(string const& className, string const& func) const {
    return id() + " " + type() + " " + protocol::toString(status()) + " " + className + "::" + func;
}

void WorkerHttpRequest::setStatus(replica::Lock const& lock, protocol::Status status,
                                  protocol::StatusExt extendedStatus) {
    LOGS(_log, LOG_LVL_TRACE,
         CONTEXT << " " << protocol::toString(_status, _extendedStatus) << " -> "
                 << protocol::toString(status, extendedStatus));
    switch (status) {
        case protocol::Status::CREATED:
            _performance.start_time = 0;
            _performance.finish_time = 0;
            break;
        case protocol::Status::IN_PROGRESS:
            _performance.setUpdateStart();
            _performance.finish_time = 0;
            break;
        case protocol::Status::IS_CANCELLING:
            break;
        case protocol::Status::CANCELLED:

            // Set the start time to some meaningful value in case if the request was
            // cancelled while sitting in the input queue
            if (0 == _performance.start_time) _performance.setUpdateStart();
            _performance.setUpdateFinish();
            break;

        case protocol::Status::SUCCESS:
        case protocol::Status::FAILED:
            _performance.setUpdateFinish();
            break;
        default:
            throw logic_error(CONTEXT + " unhandled status: " + protocol::toString(status));
    }

    // ATTENTION: the top-level status is the last to be modified in
    // the state transition to ensure clients will see a consistent state
    // of the object.
    _extendedStatus = extendedStatus;
    _status = status;
}

void WorkerHttpRequest::_expired(boost::system::error_code const& ec) {
    LOGS(_log, LOG_LVL_TRACE,
         CONTEXT << (ec == boost::asio::error::operation_aborted ? " ** ABORTED **" : ""));

    replica::Lock lock(_mtx, CONTEXT);

    // Clearing the stored callback after finishing the up-stream notification
    // has two purposes:
    //
    // 1. it guaranties no more than one time notification
    // 2. it breaks the up-stream dependency on a caller object if a shared
    //    pointer to the object was mentioned as the lambda-function's closure

    // Ignore this event if the timer was aborted
    if (ec != boost::asio::error::operation_aborted) {
        if (_onExpired != nullptr) {
            serviceProvider()->io_service().post(bind(move(_onExpired), _hdr.id));
        }
    }
    _onExpired = nullptr;
}

}  // namespace lsst::qserv::replica
