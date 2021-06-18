/*
 * LSST Data Management System
 * Copyright 2015-2016 AURA/LSST.
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

// System headers
#include <memory>
#include <sstream>

// Third-party headers

// Class header
#include "qdisp/JobQuery.h"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/LogContext.h"
#include "qdisp/Executive.h"
#include "qdisp/QueryRequest.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.JobQuery");
} // anonymous namespace

namespace lsst {
namespace qserv {
namespace qdisp {

using namespace std;


JobQuery::JobQuery(Executive::Ptr const& executive, JobDescription::Ptr const& jobDescription,
                   JobStatus::Ptr const& jobStatus,
                   shared_ptr<MarkCompleteFunc> const& markCompleteFunc,
                   QueryId qid) :
          _executive(executive), _jobDescription(jobDescription),
          _markCompleteFunc(markCompleteFunc), _jobStatus(jobStatus),
          _qid(qid),
          _idStr(QueryIdHelper::makeIdStr(qid, getIdInt())) {
    _qdispPool = executive->getQdispPool();
    LOGS(_log, LOG_LVL_TRACE, "JobQuery desc=" << _jobDescription);
}

JobQuery::~JobQuery() {
    LOGS(_log, LOG_LVL_DEBUG, "~JobQuery");
}

/** Attempt to run the job on a worker.
 * @return - false if it can not setup the job or the maximum number of attempts has been reached.
 */
bool JobQuery::runJob() {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getIdInt());
    LOGS(_log, LOG_LVL_DEBUG, " runJob " << *this);
    auto executive = _executive.lock();
    if (executive == nullptr) {
        LOGS(_log, LOG_LVL_ERROR, "runJob failed executive==nullptr");
        return false;
    }
    bool cancelled = executive->getCancelled();
    bool handlerReset = _jobDescription->respHandler()->reset();
    if (!cancelled && handlerReset) {
        auto criticalErr = [this, &executive](string const& msg) {
            LOGS(_log, LOG_LVL_ERROR, msg << " "
                 << _jobDescription << " Canceling user query!");
            executive->squash(); // This should kill all jobs in this user query.
        };

        LOGS(_log, LOG_LVL_DEBUG, "runJob checking attempt=" << _jobDescription->getAttemptCount());
        lock_guard<recursive_mutex> lock(_rmutex);
        if (_jobDescription->getAttemptCount() < _getMaxAttempts()) {
            bool okCount = _jobDescription->incrAttemptCountScrubResults();
            if (!okCount) {
                criticalErr("hit structural max of retries");
                return false;
            }
            if (!_jobDescription->verifyPayload()) {
                criticalErr("bad payload");
                return false;
            }
        } else {
            LOGS(_log, LOG_LVL_DEBUG, "runJob max retries");
            criticalErr("hit maximum number of retries");
            return false;
        }

        // At this point we are all set to actually run the query. We create a
        // a shared pointer to this object to prevent it from escaping while we
        // are trying to start this whole process. We also make sure we record
        // whether or not we are in SSI as cancellation handling differs.
        //
        LOGS(_log, LOG_LVL_TRACE, "runJob calls StartQuery()");
        JobQuery::Ptr jq(dynamic_pointer_cast<JobQuery>(shared_from_this()));
        _inSsi = true;
        if (executive->startQuery(jq)) {
           _jobStatus->updateInfo(_idStr, JobStatus::REQUEST);
           return true;
        }
        _inSsi = false;
    }
    LOGS(_log, LOG_LVL_WARN, "runJob failed. cancelled=" << cancelled
              << " reset=" << handlerReset);
    return false;
}

/// Cancel response handling. Return true if this is the first time cancel has been called.
bool JobQuery::cancel() {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getIdInt());
    LOGS(_log, LOG_LVL_DEBUG, "JobQuery::cancel()");
    if (_cancelled.exchange(true) == false) {
        lock_guard<recursive_mutex> lock(_rmutex);
        // If _inSsi is true then this query request has been passed to SSI and
        // _queryRequestPtr cannot be a nullptr. Cancellation is complicated.
        bool cancelled = false;
        if (_inSsi) {
            LOGS(_log, LOG_LVL_DEBUG, "cancel QueryRequest in progress");
            if (_queryRequestPtr->cancel()) {
                LOGS(_log, LOG_LVL_DEBUG, "cancelled by QueryRequest");
                cancelled = true;
            } else {
                LOGS(_log, LOG_LVL_DEBUG, "QueryRequest could not cancel");
            }
        }
        if (!cancelled) {
            ostringstream os;
            os << _idStr << " cancel QueryRequest=" << _queryRequestPtr ;
            LOGS(_log, LOG_LVL_DEBUG, os.str());
            getDescription()->respHandler()->errorFlush(os.str(), -1);
            auto executive = _executive.lock();
            if (executive == nullptr) {
                LOGS(_log, LOG_LVL_ERROR, " can't markComplete cancelled, executive == nullptr");
                return false;
            }
            executive->markCompleted(getIdInt(), false);
        }
        _jobDescription->respHandler()->processCancel();
        return true;
    }
    LOGS(_log, LOG_LVL_TRACE, "cancel, skipping, already cancelled.");
    return false;
}


/// @return true if this job's executive has been cancelled.
/// There is enough delay between the executive being cancelled and the executive
/// cancelling all the jobs that it makes a difference. If either the executive,
/// or the job has cancelled, proceeding is probably not a good idea.
bool JobQuery::isQueryCancelled() {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getIdInt());
    auto exec = _executive.lock();
    if (exec == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "_executive == nullptr");
        return true; // Safer to assume the worst.
    }
    return exec->getCancelled();
}


std::string const& JobQuery::getPayload() const {
    return _jobDescription->payload();
}


void JobQuery::callMarkCompleteFunc(bool success) {
    _markCompleteFunc->operator()(success);
}


ostream& JobQuery::dumpOS(ostream &os) const {
    return os << "{" << getIdStr() << _jobDescription << " " << _jobStatus << "}";
}


}}} // end namespace
