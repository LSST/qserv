/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
 *
 * JobDescription.cc
 *      Author: jgates
 */

// Class header
#include "qdisp/JobDescription.h"

// System headers
#include <sstream>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "proto/worker.pb.h"
#include "util/Bug.h"
#include "qdisp/Executive.h"
#include "qdisp/ResponseHandler.h"
#include "qproc/ChunkQuerySpec.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.JobDescription");
}

namespace lsst::qserv::qdisp {

JobDescription::JobDescription(qmeta::CzarId czarId, QueryId qId, JobId jobId, ResourceUnit const& resource,
                               shared_ptr<ResponseHandler> const& respHandler,
                               shared_ptr<qproc::ChunkQuerySpec> const& chunkQuerySpec,
                               string const& chunkResultName, bool mock)
        : _czarId(czarId),
          _queryId(qId),
          _jobId(jobId),
          _qIdStr(QueryIdHelper::makeIdStr(_queryId, _jobId)),
          _resource(resource),
          _respHandler(respHandler),
          _chunkQuerySpec(chunkQuerySpec),
          _chunkResultName(chunkResultName),
          _mock(mock) {}

bool JobDescription::incrAttemptCount(std::shared_ptr<Executive> const& exec, bool increase) {
    if (increase) {
        ++_attemptCount;
    }

    if (exec != nullptr) {
        int maxAttempts = exec->getMaxAttempts();
        if (_attemptCount > 0) {
            LOGS(_log, LOG_LVL_INFO, "JoBDescription::" << __func__ << " attempts=" << _attemptCount);
        }
        if (_attemptCount > maxAttempts) {
            LOGS(_log, LOG_LVL_ERROR,
                 "JoQDescription::" << __func__ << " attempts(" << _attemptCount << ") > maxAttempts("
                                    << maxAttempts << ") cancelling");
            exec->addMultiError(qmeta::JobStatus::RETRY_ERROR,
                                "max attempts reached " + to_string(_attemptCount) + " " + _qIdStr,
                                util::ErrorCode::INTERNAL);
            exec->squash(string("incrAttemptCount ") + to_string(_attemptCount));
            return false;
        }
    }

    if (_attemptCount >= MAX_JOB_ATTEMPTS) {
        LOGS(_log, LOG_LVL_ERROR, "attemptCount greater than maximum number of retries " << _attemptCount);
        return false;
    }

    return true;
}

bool JobDescription::getScanInteractive() const { return _chunkQuerySpec->scanInteractive; }

int JobDescription::getScanRating() const { return _chunkQuerySpec->scanInfo->scanRating; }

ostream& operator<<(ostream& os, JobDescription const& jd) {
    os << "job(id=" << jd._jobId << " ru=" << jd._resource.path() << " attemptCount=" << jd._attemptCount
       << ")";
    return os;
}

}  // namespace lsst::qserv::qdisp
