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
 * MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

// Class header
#include "qdisp/UberJob.h"

// System headers
#include <stdexcept>

// Third-party headers
#include <google/protobuf/arena.h>
#include "nlohmann/json.hpp"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "global/LogContext.h"
#include "http/Client.h"
#include "http/MetaModule.h"
#include "proto/ProtoImporter.h"
#include "proto/worker.pb.h"
#include "qdisp/JobQuery.h"
#include "qmeta/JobStatus.h"
#include "util/Bug.h"
#include "util/common.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace nlohmann;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.UberJob");
}

namespace lsst { namespace qserv { namespace qdisp {

UberJob::Ptr UberJob::create(Executive::Ptr const& executive,
                             std::shared_ptr<ResponseHandler> const& respHandler, int queryId, int uberJobId,
                             qmeta::CzarId czarId,
                             czar::CzarChunkMap::WorkerChunksData::Ptr const& workerData) {
    UberJob::Ptr uJob(new UberJob(executive, respHandler, queryId, uberJobId, czarId, workerData));
    uJob->_setup();
    return uJob;
}

UberJob::UberJob(Executive::Ptr const& executive, std::shared_ptr<ResponseHandler> const& respHandler,
                 int queryId, int uberJobId, qmeta::CzarId czarId,
                 czar::CzarChunkMap::WorkerChunksData::Ptr const& workerData)
        : JobBase(),
          _executive(executive),
          _respHandler(respHandler),
          _queryId(queryId),
          _uberJobId(uberJobId),
          _czarId(czarId),
          _idStr("QID=" + to_string(_queryId) + ":uj=" + to_string(uberJobId)),
          _qdispPool(executive->getQdispPool()),
          _workerData(workerData) {}

void UberJob::_setup() {
    JobBase::Ptr jbPtr = shared_from_this();
    _respHandler->setJobQuery(jbPtr);
}

bool UberJob::addJob(JobQuery::Ptr const& job) {
    bool success = false;
    if (job->setUberJobId(getJobId())) {
        lock_guard<mutex> lck(_jobsMtx);
        _jobs.push_back(job);
        success = true;
    }
    if (!success) {
        // &&&uj not really the right thing to do, but high visibility wanted for now.
        throw util::Bug(ERR_LOC, string("job already in UberJob job=") + job->dump() + " uberJob=" + dump());
    }
    return success;
}

bool UberJob::runUberJob() {
    LOGS(_log, LOG_LVL_WARN, cName(__func__) << "&&& UberJob::runUberJob() start");
    // &&&uj most, if not all, of this should be done in a command in the QDispPool.
    // &&&uk especially the communication parts.
    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() a");
    // Build the uberjob payload for each job.
    nlohmann::json uj;
    unique_lock<mutex> jobsLock(_jobsMtx);
    LOGS(_log, LOG_LVL_WARN,
         getIdStr() << "&&&uj count qid=" << getQueryId() << " ujId=" << getJobId()
                    << " jobs.sz=" << _jobs.size());
    for (auto const& jqPtr : _jobs) {
        LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob ::runUberJob() a1");
        jqPtr->getDescription()->incrAttemptCountScrubResultsJson();
    }

    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() b");
    // Send the uberjob to the worker
    auto const method = http::Method::POST;
    string const url = "http://" + _wContactInfo->wHost + ":" + to_string(_wContactInfo->wPort) + "/queryjob";
    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() c " << url);
    vector<string> const headers = {"Content-Type: application/json"};
    auto const& czarConfig = cconfig::CzarConfig::instance();
    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() c");
    // See xrdsvc::httpWorkerCzarModule::_handleQueryJob for json message parsing.
    json request = {{"version", http::MetaModule::version},
                    {"instance_id", czarConfig->replicationInstanceId()},
                    {"auth_key", czarConfig->replicationAuthKey()},
                    {"worker", _wContactInfo->wId},
                    {"czar",
                     {{"name", czarConfig->name()},
                      {"id", czarConfig->id()},
                      {"management-port", czarConfig->replicationHttpPort()},
                      {"management-host-name", util::get_current_host_fqdn()}}},
                    {"uberjob",
                     {{"queryid", _queryId},
                      {"uberjobid", _uberJobId},
                      {"czarid", _czarId},
                      {"jobs", json::array()}}}};

    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() d " << request);
    auto& jsUberJob = request["uberjob"];
    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() e " << jsUberJob);
    auto& jsJobs = jsUberJob["jobs"];
    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob::runUberJob() f " << jsJobs);
    for (auto const& jbPtr : _jobs) {
        LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob ::runUberJob() f1");
        auto const description = jbPtr->getDescription();
        LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob ::runUberJob() f1a");
        if (description == nullptr) {
            throw util::Bug(ERR_LOC, cName(__func__) + " &&&uj description=null for job=" + jbPtr->getIdStr());
        }
        auto const jsForWorker = jbPtr->getDescription()->getJsForWorker();
        LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob ::runUberJob() f1b");
        if (jsForWorker == nullptr) {
            throw util::Bug(ERR_LOC, getIdStr() + " &&&uj jsForWorker=null for job=" + jbPtr->getIdStr());
        }
        //&&& json jsJob = {{"jobdesc", *(jbPtr->getDescription()->getJsForWorker())}};
        json jsJob = {{"jobdesc", *jsForWorker}};
        jsJobs.push_back(jsJob);
        jbPtr->getDescription()->resetJsForWorker();  // no longer needed.
    }
    jobsLock.unlock();  // unlock so other _jobsMtx threads can advance while this waits for transmit

    LOGS(_log, LOG_LVL_WARN, getIdStr() << "&&& UberJob ::runUberJob() g");
    LOGS(_log, LOG_LVL_WARN, __func__ << " &&&REQ " << request);
    string const requestContext = "Czar: '" + http::method2string(method) + "' request to '" + url + "'";
    LOGS(_log, LOG_LVL_TRACE,
            cName(__func__) << " czarPost url=" << url << " request=" << request.dump() << " headers=" << headers[0]);
    http::Client client(method, url, request.dump(), headers);
    bool transmitSuccess = false;
    string exceptionWhat;
    try {
        json const response = client.readAsJson();
        LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::runUberJob() response=" << response);
        if (0 != response.at("success").get<int>()) {
            LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::runUberJob() success");
            transmitSuccess = true;
        } else {
            LOGS(_log, LOG_LVL_WARN, _idStr << " UberJob::" << __func__ << " response success=0");
        }
    } catch (exception const& ex) {
        LOGS(_log, LOG_LVL_WARN, requestContext + " &&&uj failed, ex: " + ex.what());
        exceptionWhat = ex.what();
    }
    if (!transmitSuccess) {
        LOGS(_log, LOG_LVL_ERROR, "&&&uj UberJob::runUberJob() transmit failure, try to send jobs elsewhere");
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " transmit failure, try to send jobs elsewhere");
        _unassignJobs();  // locks _jobsMtx
        setStatusIfOk(qmeta::JobStatus::RESPONSE_ERROR,
                cName(__func__) + " not transmitSuccess " + exceptionWhat);

    } else {
        LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::runUberJob() register all jobs as transmitted to worker");
        setStatusIfOk(qmeta::JobStatus::REQUEST, cName(__func__) + " transmitSuccess");  // locks _jobsMtx
    }
    return false;
}

void UberJob::prepScrubResults() {
    // &&&uj There's a good chance this will not be needed as incomplete files will not be merged
    //       so you don't have to worry about removing rows from incomplete jobs or uberjobs
    //       from the result table.
    throw util::Bug(ERR_LOC,
                    "&&&uj If needed, should call prepScrubResults for all JobQueries in the UberJob ");
}

void UberJob::_unassignJobs() {
    lock_guard<mutex> lck(_jobsMtx);
    auto exec = _executive.lock();
    if (exec == nullptr) {
        LOGS(_log, LOG_LVL_WARN, cName(__func__) << " exec is null");
        return;
    }
    auto maxAttempts = exec->getMaxAttempts();
    for (auto&& job : _jobs) {
        string jid = job->getIdStr();
        if (!job->unassignFromUberJob(getJobId())) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " could not unassign job=" << jid << " cancelling");
            exec->addMultiError(qmeta::JobStatus::RETRY_ERROR, "unable to re-assign " + jid, util::ErrorCode::INTERNAL);
            exec->squash();
            return;
        }
        auto attempts = job->getAttemptCount();
        if (attempts > maxAttempts) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " job=" << jid << " attempts=" << attempts << " maxAttempts reached, cancelling");
            exec->addMultiError(qmeta::JobStatus::RETRY_ERROR, "max attempts reached " + to_string(attempts) + " job=" + jid, util::ErrorCode::INTERNAL);
            exec->squash();
            return;
        }
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " job=" << jid << " attempts=" << attempts);
    }
    _jobs.clear();
    bool const setFlag = true;
    exec->setFlagFailedUberJob(setFlag);
}

bool UberJob::isQueryCancelled() {
    auto exec = _executive.lock();
    if (exec == nullptr) {
        LOGS(_log, LOG_LVL_WARN, cName(__func__) << " _executive == nullptr");
        return true;  // Safer to assume the worst.
    }
    return exec->getCancelled();
}

bool UberJob::verifyPayload() const {
    proto::ProtoImporter<proto::UberJobMsg> pi;
    if (!pi.messageAcceptable(_payload)) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " Error serializing UberJobMsg.");
        return false;
    }
    return true;
}

bool UberJob::_setStatusIfOk(qmeta::JobStatus::State newState, string const& msg) {
    // must be locked _jobsMtx
    auto currentState = _jobStatus->getState();
    // Setting the same state twice indicates that the system is trying to do something it
    // has already done, so doing it a second time would be an error.
    if (newState <= currentState) {
        LOGS(_log, LOG_LVL_WARN,
             cName(__func__) << " could not change from state="
                        << _jobStatus->stateStr(currentState) << " to " << _jobStatus->stateStr(newState));
        return false;
    }

    // Overwriting errors is probably not a good idea.
    if (currentState >= qmeta::JobStatus::CANCEL && currentState < qmeta::JobStatus::COMPLETE) {
        LOGS(_log, LOG_LVL_WARN,
             cName(__func__) << " already error current="
                        << _jobStatus->stateStr(currentState) << " new=" << _jobStatus->stateStr(newState));
        return false;
    }

    _jobStatus->updateInfo(getIdStr(), newState, msg);
    for (auto&& jq : _jobs) {
        jq->getStatus()->updateInfo(jq->getIdStr(), newState, msg);
    }
    return true;
}

void UberJob::callMarkCompleteFunc(bool success) {
    LOGS(_log, LOG_LVL_DEBUG, "UberJob::callMarkCompleteFunc success=" << success);

    lock_guard<mutex> lck(_jobsMtx);
    // Need to set this uberJob's status, however exec->markCompleted will set
    // the status for each job when it is called.
    // &&&uj JobStatus should have a separate entry for success/failure/incomplete/retry.
    string source = string("UberJob_") + (success ? "SUCCESS" : "FAILED");
    _jobStatus->updateInfo(getIdStr(), qmeta::JobStatus::COMPLETE, source);
    for (auto&& job : _jobs) {
        string idStr = job->getIdStr();
        if (success) {
            job->getStatus()->updateInfo(idStr, qmeta::JobStatus::COMPLETE, source);
        } else {
            job->getStatus()->updateInfoNoErrorOverwrite(idStr, qmeta::JobStatus::RESULT_ERROR, source,
                                                         util::ErrorCode::INTERNAL, "UberJob_failure");
        }
        auto exec = _executive.lock();
        exec->markCompleted(job->getJobId(), success);
    }

    // No longer need these here. Executive should still have copies.
    _jobs.clear();
}

/// Retrieve and process a result file using the file-based protocol
/// Uses a copy of JobQuery::Ptr instead of _jobQuery as a call to cancel() would reset _jobQuery.
json UberJob::importResultFile(string const& fileUrl, uint64_t rowCount, uint64_t fileSize) {
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile a");
    LOGS(_log, LOG_LVL_WARN,
         "&&&uj UberJob::importResultFile fileUrl=" << fileUrl << " rowCount=" << rowCount
                                                    << " fileSize=" << fileSize);

    if (isQueryCancelled()) {
        LOGS(_log, LOG_LVL_WARN, "UberJob::importResultFile import job was cancelled.");
        return _importResultError(true, "cancelled", "Query cancelled");
    }
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile b");

    auto exec = _executive.lock();
    if (exec == nullptr || exec->getCancelled()) {
        LOGS(_log, LOG_LVL_WARN, "UberJob::importResultFile no executive or cancelled");
        return _importResultError(true, "cancelled", "Query cancelled - no executive");
    }
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile c");

    if (exec->isLimitRowComplete()) {
        int dataIgnored = exec->incrDataIgnoredCount();
        if ((dataIgnored - 1) % 1000 == 0) {
            LOGS(_log, LOG_LVL_INFO,
                 "UberJob ignoring, enough rows already "
                         << "dataIgnored=" << dataIgnored);
        }
        return _importResultError(false, "rowLimited", "Enough rows already");
    }

    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile d");

    LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " fileSize=" << fileSize);

    bool const statusSet = setStatusIfOk(qmeta::JobStatus::RESPONSE_READY, getIdStr() + " " + fileUrl);
    if (!statusSet) {
        LOGS(_log, LOG_LVL_WARN, cName(__func__) << " &&&uj setStatusFail could not set status to RESPONSE_READY");
        return _importResultError(false, "setStatusFail", "could not set status to RESPONSE_READY");
    }

    JobBase::Ptr jBaseThis = shared_from_this();
    weak_ptr<UberJob> ujThis = std::dynamic_pointer_cast<UberJob>(jBaseThis);

    // &&&uj lambda may not be the best way to do this.
    // &&&uj check synchronization - may need a mutex for merging.
    auto fileCollectFunc = [ujThis, fileUrl, rowCount](util::CmdData*) {
        LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile::fileCollectFunc a");
        auto ujPtr = ujThis.lock();
        if (ujPtr == nullptr) {
            LOGS(_log, LOG_LVL_DEBUG, "UberJob::importResultFile::fileCollectFunction uberjob ptr is null " << fileUrl);
            return;
        }
        uint64_t resultRows = 0;
        auto [flushSuccess, flushShouldCancel] =
                ujPtr->getRespHandler()->flushHttp(fileUrl, rowCount, resultRows);
        LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile::fileCollectFunc b");
        if (!flushSuccess) {
            // This would probably indicate malformed file+rowCount or
            // writing the result table failed.
            ujPtr->_importResultError(flushShouldCancel, "mergeError", "merging failed");
        }

        // At this point all data for this job have been read, there's no point in
        // having XrdSsi wait for anything.
        LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile::fileCollectFunc c");
        ujPtr->_importResultFinish(resultRows);

        LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::importResultFile::fileCollectFunc end");
    };

    auto cmd = qdisp::PriorityCommand::Ptr(new qdisp::PriorityCommand(fileCollectFunc));
    exec->queueFileCollect(cmd);

    // If the query meets the limit row complete complete criteria, it will start
    // squashing superfluous results so the answer can be returned quickly.

    json jsRet = {{"success", 1}, {"errortype", ""}, {"note", "queued for collection"}};
    return jsRet;
}

json UberJob::workerError(int errorCode, string const& errorMsg) {
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::workerError a");
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::workerError code=" << errorCode << " msg=" << errorMsg);
    LOGS(_log, LOG_LVL_WARN, cName(__func__) << " errcode=" << errorCode << " errmsg=" << errorMsg);

    bool const deleteData = true;
    bool const keepData = !deleteData;
    auto exec = _executive.lock();
    if (exec == nullptr || isQueryCancelled()) {
        LOGS(_log, LOG_LVL_WARN, cName(__func__) << " no executive or cancelled");
        return _workerErrorFinish(deleteData, "cancelled");
    }
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::workerError c");

    if (exec->isLimitRowComplete()) {
        int dataIgnored = exec->incrDataIgnoredCount();
        if ((dataIgnored - 1) % 1000 == 0) {
            LOGS(_log, LOG_LVL_INFO,
                 cName(__func__) << " ignoring, enough rows already "
                         << "dataIgnored=" << dataIgnored);
        }
        return _workerErrorFinish(keepData, "none", "limitRowComplete");
    }

    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::workerError d");

    // Currently there are no detecable recoverable errors from workers. The only error that a worker
    // could send back that may possibly be recoverable would be a missing table error, which is not
    // trivia to detect. A worker local database error may also qualify.
    bool recoverableError = false;
    recoverableError = true; //&&& delete after testing
    if (recoverableError) { // &&& instead of killing the query, try to retry the jobs on a different worker
        /* &&&
         *
         */
        _unassignJobs();

    } else {// &&&
        // Get the error message to the user and kill the user query.
        int errState = util::ErrorCode::MYSQLEXEC;
        getRespHandler()->flushHttpError(errorCode, errorMsg, errState);
        exec->addMultiError(errorCode, errorMsg, errState);
        exec->squash();
    } // &&&

    string errType = to_string(errorCode) + ":" + errorMsg;
    return _workerErrorFinish(deleteData, errType, "");
}

json UberJob::_importResultError(bool shouldCancel, string const& errorType, string const& note) {
    json jsRet = {{"success", 0}, {"errortype", errorType}, {"note", note}};
    ///       In all cases, the worker should delete the file as
    ///       this czar will not ask for it.

    auto exec = _executive.lock();
    if (exec != nullptr) {
        LOGS(_log, LOG_LVL_ERROR,
             cName(__func__) << " shouldCancel=" << shouldCancel
                         << " errorType=" << errorType << " " << note);
        if (shouldCancel) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " failing jobs");
            callMarkCompleteFunc(false);  // all jobs failed, no retry
            exec->squash();
        } else {
            /// - each JobQuery in _jobs needs to be flagged as needing to be
            ///   put in an UberJob and it's attempt count increased and checked
            ///   against the attempt limit.
            /// - executive needs to be told to make new UberJobs until all
            ///   JobQueries are being handled by an UberJob.
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " reassigning jobs");
            _unassignJobs();
            exec->assignJobsToUberJobs();
        }
    } else {
        LOGS(_log, LOG_LVL_INFO,
             cName(__func__) << " already cancelled shouldCancel="
                         << shouldCancel << " errorType=" << errorType << " " << note);
    }
    return jsRet;
}

nlohmann::json UberJob::_importResultFinish(uint64_t resultRows) {
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::_importResultFinish a");
    /// If this is called, the file has been collected and the worker should delete it
    ///
    /// This function should call markComplete for all jobs in the uberjob
    /// and return a "success:1" json message to be sent to the worker.
    bool const statusSet =
            setStatusIfOk(qmeta::JobStatus::RESPONSE_DONE, getIdStr() + " _importResultFinish");
    if (!statusSet) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " failed to set status " << getIdStr());
        return {{"success", 0}, {"errortype", "statusMismatch"}, {"note", "failed to set status"}};
    }
    auto exec = _executive.lock();
    if (exec == nullptr) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " executive is null");
        return {{"success", 0}, {"errortype", "cancelled"}, {"note", "executive is null"}};
    }

    bool const success = true;
    callMarkCompleteFunc(success);  // sets status to COMPLETE
    exec->addResultRows(resultRows);
    exec->checkLimitRowComplete();

    json jsRet = {{"success", 1}, {"errortype", ""}, {"note", ""}};
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::_importResultFinish end");
    return jsRet;
}

nlohmann::json UberJob::_workerErrorFinish(bool deleteData, std::string const& errorType,
                                           std::string const& note) {
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::_workerErrorFinish a");
    /// If this is called, the file has been collected and the worker should delete it
    ///
    /// Should this call markComplete for all jobs in the uberjob???
    /// &&& Only recoverable errors would be: communication failure, or missing table ???
    /// Return a "success:1" json message to be sent to the worker.
    auto exec = _executive.lock();
    if (exec == nullptr) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " executive is null");
        return {{"success", 0}, {"errortype", "cancelled"}, {"note", "executive is null"}};
    }

    json jsRet = {{"success", 1}, {"deletedata", deleteData}, {"errortype", ""}, {"note", ""}};
    LOGS(_log, LOG_LVL_WARN, "&&&uj UberJob::_importResultFinish end");
    return jsRet;
}

std::ostream& UberJob::dumpOS(std::ostream& os) const {
    os << "(jobs sz=" << _jobs.size() << "(";
    lock_guard<mutex> lockJobsMtx(_jobsMtx);
    for (auto const& job : _jobs) {
        JobDescription::Ptr desc = job->getDescription();
        ResourceUnit ru = desc->resource();
        os << ru.db() << ":" << ru.chunk() << ",";
    }
    os << "))";
    return os;
}

}}}  // namespace lsst::qserv::qdisp
