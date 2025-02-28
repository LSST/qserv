/*
 * LSST Data Management System
 * Copyright 2015-2016 LSST Corporation.
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
 *  Created on: Sep 3, 2015
 *      Author: jgates
 */

#ifndef LSST_QSERV_QDISP_JOBQUERY_H_
#define LSST_QSERV_QDISP_JOBQUERY_H_

// System headers
#include <atomic>
#include <memory>
#include <mutex>

// Qserv headers
#include "qdisp/Executive.h"
#include "qdisp/JobDescription.h"
#include "qdisp/ResponseHandler.h"
#include "util/InstanceCount.h"
#include "util/Mutex.h"

namespace lsst::qserv::qdisp {

class QueryRequest;

/// This class is used to describe and monitor the queries for a
/// chunk on the worker.
class JobQuery {
public:
    typedef std::shared_ptr<JobQuery> Ptr;

    /// Factory function to make certain a shared_ptr is used and _setup is called.
    static JobQuery::Ptr create(Executive::Ptr const& executive, JobDescription::Ptr const& jobDescription,
                                qmeta::JobStatus::Ptr const& jobStatus, QueryId qid) {
        Ptr jq = Ptr(new JobQuery(executive, jobDescription, jobStatus, qid));
        return jq;
    }

    virtual ~JobQuery();

    QueryId getQueryId() const { return _qid; }
    JobId getJobId() const { return _jobDescription->id(); }
    std::string const& getIdStr() const { return _idStr; }
    JobDescription::Ptr getDescription() { return _jobDescription; }
    qmeta::JobStatus::Ptr getStatus() { return _jobStatus; }

    bool cancel(bool superfluous = false);
    bool isQueryCancelled();

    std::shared_ptr<Executive> getExecutive() { return _executive.lock(); }

    /// If the UberJob is unassigned, change the _uberJobId to ujId.
    bool setUberJobId(UberJobId ujId) {
        VMUTEX_NOT_HELD(_jqMtx);
        std::lock_guard lock(_jqMtx);
        return _setUberJobId(ujId);
    }

    UberJobId getUberJobId() const {
        VMUTEX_NOT_HELD(_jqMtx);
        std::lock_guard lock(_jqMtx);
        return _getUberJobId();
    }

    bool isInUberJob() const {
        VMUTEX_NOT_HELD(_jqMtx);
        std::lock_guard lock(_jqMtx);
        return _isInUberJob();
    }

    int getAttemptCount() const;

    /// If ujId is the current owner, clear ownership.
    /// @return true if job is unassigned.
    bool unassignFromUberJob(UberJobId ujId);

    std::ostream& dumpOS(std::ostream& os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream& os, JobQuery const& jq);

protected:
    /// Make a copy of the job description. JobQuery::_setup() must be called after creation.
    /// Do not call this directly, use create.
    JobQuery(Executive::Ptr const& executive, JobDescription::Ptr const& jobDescription,
             qmeta::JobStatus::Ptr const& jobStatus, QueryId qid);

    /// @return true if _uberJobId was set, it can only be set if it is unassigned
    ///         or by the current owner.
    /// NOTE: _jqMtx must be held before calling this
    bool _setUberJobId(UberJobId ujId);

    /// NOTE: _jqMtx must be held before calling this
    UberJobId _getUberJobId() const {
        VMUTEX_HELD(_jqMtx);
        return _uberJobId;
    }

    /// NOTE: _jqMtx must be held before calling this
    bool _isInUberJob() const {
        VMUTEX_HELD(_jqMtx);
        return _uberJobId >= 0;
    }

    // Values that don't change once set.
    std::weak_ptr<Executive> _executive;
    /// The job description needs to survive until the task is complete.
    JobDescription::Ptr _jobDescription;

    // JobStatus has its own mutex.
    qmeta::JobStatus::Ptr _jobStatus;  ///< Points at status in Executive::_statusMap

    QueryId const _qid;        // User query id
    std::string const _idStr;  ///< Identifier string for logging.

    // Values that need mutex protection
    mutable MUTEX _jqMtx;  ///< protects _jobDescription, _queryRequestPtr, _uberJobId

    // Cancellation
    std::atomic<bool> _cancelled{false};  ///< Lock to make sure cancel() is only called once.

    /// The UberJobId that this job is assigned to. Values less than zero
    /// indicate this job is unassigned. To prevent race conditions,
    /// an UberJob may only unassign a job if it has the same ID as
    /// _uberJobId.
    /// All jobs must be unassigned before they can be reassigned.
    UberJobId _uberJobId = -1;
};

}  // namespace lsst::qserv::qdisp

#endif /* LSST_QSERV_QDISP_JOBQUERY_H_ */
