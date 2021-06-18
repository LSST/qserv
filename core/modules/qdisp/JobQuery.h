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
#include "qdisp/JobBase.h"
#include "qdisp/JobDescription.h"
#include "qdisp/ResponseHandler.h"
#include "util/InstanceCount.h"

namespace lsst {
namespace qserv {
namespace qdisp {

class QdispPool;
class QueryRequest;

/** This class is used to describe, monitor, and control a single query to a worker.
 *
 */
class JobQuery : public JobBase {
public:
    typedef std::shared_ptr<JobQuery> Ptr;

    /// Factory function to make certain a shared_ptr is used and _setup is called.
    static JobQuery::Ptr create(Executive::Ptr const& executive, JobDescription::Ptr const& jobDescription,
            JobStatus::Ptr const& jobStatus, std::shared_ptr<MarkCompleteFunc> const& markCompleteFunc,
            QueryId qid) {
        Ptr jq = std::make_shared<JobQuery>(executive, jobDescription, jobStatus, markCompleteFunc, qid);
        jq->_setup();
        return jq;
    }

    virtual ~JobQuery();
    virtual bool runJob();

    QueryId getQueryId() const override {return _qid; }
    int getIdInt() const override { return _jobDescription->id(); }
    std::string const& getPayload() const override;
    std::string const& getIdStr() const override { return _idStr; }
    std::shared_ptr<ResponseHandler> getRespHandler() override { return _jobDescription->respHandler(); }
    bool getScanInteractive() const override { return _jobDescription->getScanInteractive(); }

    JobDescription::Ptr getDescription() { return _jobDescription; }
    JobStatus::Ptr getStatus() override { return _jobStatus; }

    void setQueryRequest(std::shared_ptr<QueryRequest> const& qr) override {
        std::lock_guard<std::recursive_mutex> lock(_rmutex);
        _queryRequestPtr = qr;
    }
    std::shared_ptr<QueryRequest> getQueryRequest() {
        std::lock_guard<std::recursive_mutex> lock(_rmutex);
        return _queryRequestPtr;
    }

    void callMarkCompleteFunc(bool success) override;

    bool cancel();
    bool isQueryCancelled() override;

    Executive::Ptr getExecutive() { return _executive.lock(); }

    std::shared_ptr<QdispPool> getQdispPool() override { return _qdispPool; }

    std::ostream& dumpOS(std::ostream &os) const override;

    /// Make a copy of the job description. JobQuery::_setup() must be called after creation.
    /// Do not call this directly, use create.
    JobQuery(Executive::Ptr const& executive, JobDescription::Ptr const& jobDescription,
        JobStatus::Ptr const& jobStatus, std::shared_ptr<MarkCompleteFunc> const& markCompleteFunc,
        QueryId qid);

    /// Set to true if this job is part of an UberJob
    void setInUberJob(bool inUberJob) { _inUberJob = inUberJob; };

    /// @return true if this job is part of an UberJob.
    bool inUberJob() const { return _inUberJob; }

protected:
    void _setup() {
        JobBase::Ptr jbPtr = shared_from_this();
        _jobDescription->respHandler()->setJobQuery(jbPtr);
    }

    int _getRunAttemptsCount() const {
        std::lock_guard<std::recursive_mutex> lock(_rmutex);
        return _jobDescription->getAttemptCount();
    }
    int _getMaxAttempts() const { return 5; } // Arbitrary value until solid value with reason determined.
    int _getAttemptSleepSeconds() const { return 30; } // As above or until added to config file.

    // Values that don't change once set.
    std::weak_ptr<Executive>  _executive;
    /// The job description needs to survive until the task is complete.
    JobDescription::Ptr _jobDescription;
    std::shared_ptr<MarkCompleteFunc> _markCompleteFunc;

    // JobStatus has its own mutex.
    JobStatus::Ptr _jobStatus; ///< Points at status in Executive::_statusMap

    QueryId const _qid; // User query id
    std::string const _idStr; ///< Identifier string for logging.

    // Values that need mutex protection
    mutable std::recursive_mutex _rmutex; ///< protects _jobDescription,
                                          ///< _queryRequestPtr, and _inSsi

    // SSI items
    std::shared_ptr<QueryRequest> _queryRequestPtr;
    bool _inSsi{false};

    // Cancellation
    std::atomic<bool> _cancelled {false}; ///< Lock to make sure cancel() is only called once.

    std::shared_ptr<QdispPool> _qdispPool;

    /// True if this job is part of an UberJob.
    std::atomic<bool> _inUberJob{false}; ///< TODO:UJ There are probably several places this should be checked
};

}}} // end namespace

#endif /* LSST_QSERV_QDISP_JOBQUERY_H_ */
