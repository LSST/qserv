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
#include "replica/worker/WorkerEchoHttpRequest.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "util/BlockPost.h"

// LSST headers
#include "lsst/log/Log.h"

#define CONTEXT context("WorkerEchoHttpRequest", __func__)

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.WorkerEchoHttpRequest");

}  // namespace

namespace lsst::qserv::replica {

shared_ptr<WorkerEchoHttpRequest> WorkerEchoHttpRequest::create(
        shared_ptr<ServiceProvider> const& serviceProvider, string const& worker,
        protocol::QueuedRequestHdr const& hdr, json const& req, ExpirationCallbackType const& onExpired) {
    auto ptr = shared_ptr<WorkerEchoHttpRequest>(
            new WorkerEchoHttpRequest(serviceProvider, worker, hdr, req, onExpired));
    ptr->init();
    return ptr;
}

WorkerEchoHttpRequest::WorkerEchoHttpRequest(shared_ptr<ServiceProvider> const& serviceProvider,
                                             string const& worker, protocol::QueuedRequestHdr const& hdr,
                                             json const& req, ExpirationCallbackType const& onExpired)
        : WorkerHttpRequest(serviceProvider, worker, "TEST_ECHO", hdr, req, onExpired),
          _delay(req.at("delay")),
          _data(req.at("data")),
          _delayLeft(_delay) {
    if (_delay < 0) {
        throw invalid_argument(CONTEXT + " invalid delay[ms]: " + to_string(_delay));
    }
}

void WorkerEchoHttpRequest::getResult(json& result) const {
    // No locking is needed here since the method is called only after
    // the request is completed.
    result["data"] = _data;
}

bool WorkerEchoHttpRequest::execute() {
    LOGS(_log, LOG_LVL_DEBUG, CONTEXT << " delay[ms]: " << _delayLeft << " / " << _delay);

    replica::Lock lock(_mtx, CONTEXT);
    checkIfCancelling(lock, CONTEXT);

    // Block the thread for the random number of milliseconds in the interval
    // below. Then update the amount of time which is still left.
    // The delay is in the range of [0..1] through [0..1000] milliseconds depending
    // on the amount of time which is still left.
    util::BlockPost blockPost(0, max(1, min(1000, _delayLeft)));
    int const span = blockPost.wait();
    _delayLeft -= (span < _delayLeft) ? span : _delayLeft;

    // Done if have reached or exceeded the initial delay
    if (0 == _delayLeft) {
        setStatus(lock, protocol::Status::SUCCESS);
        return true;
    }
    return false;
}

}  // namespace lsst::qserv::replica
