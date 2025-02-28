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
#ifndef LSST_QSERV_QDISP_UBERJOB_H
#define LSST_QSERV_QDISP_UBERJOB_H

// System headers

// Qserv headers
#include "qmeta/types.h"
#include "czar/CzarChunkMap.h"  // Need nested class. TODO:UJ Make non-nested?
#include "czar/CzarRegistry.h"  // Need nested class. TODO:UJ Make non-nested?
#include "qdisp/Executive.h"
#include "qmeta/JobStatus.h"

namespace lsst::qserv::util {
class QdispPool;
}

namespace lsst::qserv::qdisp {

class JobQuery;

/// This class is a contains x number of jobs that need to go to the same worker
/// from a single user query, and contact information for the worker. It also holds
/// some information common to all jobs.
/// The UberJob constructs the message to send to the worker and handles collecting
/// and merging the results.
/// When this UberJobCompletes, all the Jobs it contains are registered as completed.
/// If this UberJob fails, it will be destroyed, un-assigning all of its Jobs.
/// Those Jobs will need to be reassigned to new UberJobs, or the query cancelled.
class UberJob : public std::enable_shared_from_this<UberJob> {
public:
    using Ptr = std::shared_ptr<UberJob>;

    static Ptr create(std::shared_ptr<Executive> const& executive,
                      std::shared_ptr<ResponseHandler> const& respHandler, int queryId, int uberJobId,
                      qmeta::CzarId czarId, czar::CzarChunkMap::WorkerChunksData::Ptr const& workerData);

    UberJob() = delete;
    UberJob(UberJob const&) = delete;
    UberJob& operator=(UberJob const&) = delete;

    virtual ~UberJob() {};

    std::string cName(const char* funcN) const { return std::string("UberJob::") + funcN + " " + getIdStr(); }

    bool addJob(std::shared_ptr<JobQuery> const& job);

    /// Make a json version of this UberJob and send it to its worker.
    virtual void runUberJob();

    /// Kill this UberJob and unassign all Jobs so they can be used in a new UberJob if needed.
    void killUberJob();

    QueryId getQueryId() const { return _queryId; }
    UberJobId getUjId() const { return _uberJobId; }
    std::string const& getIdStr() const { return _idStr; }
    std::shared_ptr<ResponseHandler> getRespHandler() { return _respHandler; }
    std::shared_ptr<qmeta::JobStatus> getStatus() { return _jobStatus; }
    bool isQueryCancelled();
    void callMarkCompleteFunc(bool success);  ///< call markComplete for all jobs in this UberJob.
    std::shared_ptr<Executive> getExecutive() { return _executive.lock(); }

    /// Return false if not ok to set the status to newState, otherwise set the state for
    /// this UberJob and all jobs it contains to newState.
    /// This is used both to set status and prevent the system from repeating operations
    /// that have already happened. If it returns false, the thread calling this
    /// should stop processing.
    bool setStatusIfOk(qmeta::JobStatus::State newState, std::string const& msg) {
        std::lock_guard<std::mutex> jobLock(_jobsMtx);
        return _setStatusIfOk(newState, msg);
    }

    int getJobCount() const { return _jobs.size(); }

    /// TODO:UJ may not need,
    void prepScrubResults();

    /// Set the worker information needed to send messages to the worker believed to
    /// be responsible for the chunks handled in this UberJob.
    void setWorkerContactInfo(protojson::WorkerContactInfo::Ptr const& wContactInfo) {
        _wContactInfo = wContactInfo;
    }

    protojson::WorkerContactInfo::Ptr getWorkerContactInfo() { return _wContactInfo; }

    /// Get the data for the worker that should handle this UberJob.
    czar::CzarChunkMap::WorkerChunksData::Ptr getWorkerData() { return _workerData; }

    /// Queue the lambda function to collect and merge the results from the worker.
    /// @return a json message indicating success unless the query has been
    ///         cancelled, limit row complete, or similar.
    nlohmann::json importResultFile(std::string const& fileUrl, uint64_t rowCount, uint64_t fileSize);

    /// Handle an error from the worker.
    nlohmann::json workerError(int errorCode, std::string const& errorMsg);

    void setResultFileSize(uint64_t fileSize) { _resultFileSize = fileSize; }
    uint64_t getResultFileSize() { return _resultFileSize; }

    std::ostream& dumpOS(std::ostream& os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream& os, UberJob const& uj);

protected:
    UberJob(std::shared_ptr<Executive> const& executive, std::shared_ptr<ResponseHandler> const& respHandler,
            int queryId, int uberJobId, qmeta::CzarId czarId, int rowLimit,
            czar::CzarChunkMap::WorkerChunksData::Ptr const& workerData);

private:
    /// Used to setup elements that can't be done in the constructor.
    void _setup();

    /// @see setStatusIfOk
    /// note: _jobsMtx must be locked before calling.
    bool _setStatusIfOk(qmeta::JobStatus::State newState, std::string const& msg);

    /// unassign all Jobs in this UberJob and set the Executive flag to indicate that Jobs need
    /// reassignment. The list of _jobs is cleared, so multiple calls of this should be harmless.
    void _unassignJobs();

    /// Import and error from trying to collect results.
    /// TODO:UJ The strings for errorType should have a centralized location in the code - global or util
    nlohmann::json _importResultError(bool shouldCancel, std::string const& errorType,
                                      std::string const& note);

    /// Let the executive know that all Jobs in UberJob are complete.
    void _importResultFinish(uint64_t resultRows);

    /// Let the Executive know about errors while handling results.
    nlohmann::json _workerErrorFinish(bool successful, std::string const& errorType = std::string(),
                                      std::string const& note = std::string());

    std::vector<std::shared_ptr<JobQuery>> _jobs;  ///< List of Jobs in this UberJob.
    mutable std::mutex _jobsMtx;                   ///< Protects _jobs, _jobStatus
    std::atomic<bool> _started{false};
    qmeta::JobStatus::Ptr _jobStatus{new qmeta::JobStatus()};  // TODO:UJ Maybe the JobStatus class should be
                                                               // changed to better represent UberJobs

    std::string _payload;  ///< XrdSsi message to be sent to the _workerResource. TODO:UJ remove when possible

    std::weak_ptr<Executive> _executive;
    std::shared_ptr<ResponseHandler> _respHandler;
    QueryId const _queryId;
    UberJobId const _uberJobId;
    qmeta::CzarId const _czarId;
    int const _rowLimit;
    uint64_t _resultFileSize = 0;

    std::string const _idStr;

    // Map of workerData
    czar::CzarChunkMap::WorkerChunksData::Ptr _workerData;  // TODO:UJ this may not be needed

    // Contact information for the target worker.
    protojson::WorkerContactInfo::Ptr _wContactInfo;  // TODO:UJ Maybe change to ActiveWorker?
};

}  // namespace lsst::qserv::qdisp

#endif  // LSST_QSERV_QDISP_UBERJOB_H
