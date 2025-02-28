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
#include <sstream>

// Third-party headers

// Class header
#include "qdisp/JobQuery.h"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/LogContext.h"
#include "qdisp/Executive.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.JobQuery");
}  // anonymous namespace

using namespace std;

namespace lsst::qserv::qdisp {

JobQuery::JobQuery(Executive::Ptr const& executive, JobDescription::Ptr const& jobDescription,
                   qmeta::JobStatus::Ptr const& jobStatus, QueryId qid)
        : _executive(executive),
          _jobDescription(jobDescription),
          _jobStatus(jobStatus),
          _qid(qid),
          _idStr(QueryIdHelper::makeIdStr(qid, getJobId())) {
    LOGS(_log, LOG_LVL_TRACE, "JobQuery desc=" << _jobDescription);
}

JobQuery::~JobQuery() { LOGS(_log, LOG_LVL_TRACE, "~JobQuery QID=" << _idStr); }

/// Cancel response handling. Return true if this is the first time cancel has been called.
bool JobQuery::cancel(bool superfluous) {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getJobId());
    if (_cancelled.exchange(true) == false) {
        LOGS(_log, LOG_LVL_TRACE, "JobQuery::cancel() " << superfluous);
        VMUTEX_NOT_HELD(_jqMtx);
        lock_guard lock(_jqMtx);

        ostringstream os;
        os << _idStr << " cancel";
        LOGS(_log, LOG_LVL_DEBUG, os.str());
        if (!superfluous) {
            getDescription()->respHandler()->errorFlush(os.str(), -1);
        }
        auto executive = _executive.lock();
        if (executive == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, " can't markComplete cancelled, executive == nullptr");
            return false;
        }
        executive->markCompleted(getJobId(), false);
        if (!superfluous) {
            _jobDescription->respHandler()->processCancel();
        }
        return true;
    }
    LOGS(_log, LOG_LVL_TRACE, "JobQuery::cancel, skipping, already cancelled.");
    return false;
}

/// @return true if this job's executive has been cancelled.
/// There is enough delay between the executive being cancelled and the executive
/// cancelling all the jobs that it makes a difference. If either the executive,
/// or the job has cancelled, proceeding is probably not a good idea.
bool JobQuery::isQueryCancelled() {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getJobId());
    auto exec = _executive.lock();
    if (exec == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "_executive == nullptr");
        return true;  // Safer to assume the worst.
    }
    return exec->getCancelled();
}

bool JobQuery::_setUberJobId(UberJobId ujId) {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getJobId());
    VMUTEX_HELD(_jqMtx);
    if (_uberJobId >= 0 && ujId != _uberJobId) {
        LOGS(_log, LOG_LVL_DEBUG,
             __func__ << " couldn't change UberJobId as ujId=" << ujId << " is owned by " << _uberJobId);
        return false;
    }
    _uberJobId = ujId;
    return true;
}

bool JobQuery::unassignFromUberJob(UberJobId ujId) {
    QSERV_LOGCONTEXT_QUERY_JOB(getQueryId(), getJobId());
    VMUTEX_NOT_HELD(_jqMtx);
    lock_guard lock(_jqMtx);
    if (_uberJobId < 0) {
        LOGS(_log, LOG_LVL_INFO, __func__ << " UberJobId already unassigned. attempt by ujId=" << ujId);
        return true;
    }
    if (_uberJobId != ujId) {
        LOGS(_log, LOG_LVL_ERROR,
             __func__ << " couldn't change UberJobId as ujId=" << ujId << " is owned by " << _uberJobId);
        return false;
    }
    _uberJobId = -1;

    auto exec = _executive.lock();
    // Do not increase the attempt count as it should have been increased when the job was started.
    return true;
}

int JobQuery::getAttemptCount() const {
    VMUTEX_NOT_HELD(_jqMtx);
    lock_guard lock(_jqMtx);
    return _jobDescription->getAttemptCount();
}

ostream& JobQuery::dumpOS(ostream& os) const {
    return os << "{" << getIdStr() << _jobDescription << " " << _jobStatus << "}";
}

std::string JobQuery::dump() const {
    std::ostringstream os;
    dumpOS(os);
    return os.str();
}

std::ostream& operator<<(std::ostream& os, JobQuery const& jq) { return jq.dumpOS(os); }

}  // namespace lsst::qserv::qdisp
