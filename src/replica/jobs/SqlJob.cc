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
#include "replica/jobs/SqlJob.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "global/constants.h"
#include "replica/config/Configuration.h"
#include "replica/contr/Controller.h"
#include "replica/requests/StopRequest.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ChunkedTable.h"
#include "replica/util/ReplicaInfo.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.SqlJob");

}  // namespace

namespace lsst::qserv::replica {

SqlJob::SqlJob(uint64_t maxRows, bool allWorkers, Controller::Ptr const& controller,
               string const& parentJobId, std::string const& jobName, int priority, bool ignoreNonPartitioned,
               bool ignoreDuplicateKey)
        : Job(controller, parentJobId, jobName, priority),
          _maxRows(maxRows),
          _allWorkers(allWorkers),
          _ignoreNonPartitioned(ignoreNonPartitioned),
          _ignoreDuplicateKey(ignoreDuplicateKey) {}

Job::Progress SqlJob::progress() const {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    replica::Lock lock(_mtx, context() + __func__);
    return Progress{_numFinished, _requests.size()};
}

SqlJobResult const& SqlJob::getResultData() const {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    if (state() == State::FINISHED) return _resultData;
    throw logic_error("SqlJob::" + string(__func__) +
                      "  the method can't be called while the job hasn't finished");
}

list<pair<string, string>> SqlJob::persistentLogData() const {
    list<pair<string, string>> result;
    auto const& resultData = getResultData();

    // Per-worker stats
    for (auto&& workerItr : resultData.resultSets) {
        auto&& workerName = workerItr.first;
        auto&& workerResultSets = workerItr.second;
        string workerResultSetsStr;
        for (auto&& resultSet : workerResultSets) {
            for (auto&& resultSetItr : resultSet.queryResultSet) {
                auto&& context = resultSetItr.first;
                auto&& resultSet = resultSetItr.second;
                workerResultSetsStr += "(context=" + context +
                                       ",extended_status=" + status2string(resultSet.extendedStatus) +
                                       ",char_set_name=" + resultSet.charSetName +
                                       ",has_result=" + bool2str(resultSet.hasResult) +
                                       ",fields=" + to_string(resultSet.fields.size()) +
                                       ",rows=" + to_string(resultSet.rows.size()) +
                                       ",error=" + resultSet.error + "),";
            }
        }
        result.emplace_back("worker-stats", "worker=" + workerName + ",result-set=" + workerResultSetsStr);
    }
    return result;
}

json SqlJob::getExtendedErrorReport() const {
    if (state() != State::FINISHED) {
        throw logic_error("SqlJob::" + string(__func__) +
                          "  the method can't be called before the job has finished.");
    }
    if (extendedState() == Job::ExtendedState::SUCCESS) return json();

    json report;
    report["job_state"] = Job::state2string(extendedState());
    report["workers"] = json::object();

    getResultData().iterate([&report](SqlJobResult::Worker const& workerName,
                                      SqlJobResult::Scope const& object,
                                      SqlResultSet::ResultSet const& resultSet) {
        if (resultSet.extendedStatus != ProtocolStatusExt::NONE) {
            report["workers"][workerName][object]["request_status"] = status2string(resultSet.extendedStatus);
            report["workers"][workerName][object]["request_error"] = resultSet.error;
        }
    });
    return report;
}

void SqlJob::stopRequest(replica::Lock const& lock, SqlRequest::Ptr const& request) const {
    auto const noCallBackOnFinish = nullptr;
    bool const keepTracking = true;
    StopRequest::createAndStart(controller(), request->workerName(), request->id(), noCallBackOnFinish,
                                priority(), keepTracking, id());
}

void SqlJob::startImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    auto const workerNames = allWorkers() ? controller()->serviceProvider()->config()->allWorkers()
                                          : controller()->serviceProvider()->config()->workers();

    // Launch the initial batch of requests in the number which won't exceed
    // the number of the service processing threads at each worker multiplied
    // by the number of workers involved into the operation.
    size_t const maxRequestsPerWorker =
            controller()->serviceProvider()->config()->get<size_t>("worker", "num-svc-processing-threads");

    for (auto&& workerName : workerNames) {
        _resultData.resultSets[workerName] = list<SqlResultSet>();
        auto const requests = launchRequests(lock, workerName, maxRequestsPerWorker);
        _requests.insert(_requests.cend(), requests.cbegin(), requests.cend());
    }

    // In case if no workers or database are present in the Configuration
    // at this time.
    if (_requests.size() == 0) processResultAndFinish(lock, ExtendedState::SUCCESS);
}

void SqlJob::cancelImpl(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    // The algorithm will also clear resources taken by various
    // locally created objects.
    // To ensure no lingering "side effects" will be left after cancelling this
    // job the request cancellation should be also followed (where it makes a sense)
    // by stopping the request at corresponding worker service.
    for (auto&& ptr : _requests) {
        ptr->cancel();
        if (ptr->state() != Request::State::FINISHED) stopRequest(lock, ptr);
    }
    _requests.clear();
}

void SqlJob::onRequestFinish(SqlRequest::Ptr const& request) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "  worker=" << request->workerName());
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context() + __func__);
    if (state() == State::FINISHED) return;
    _numFinished++;
    // Update stats, including the result sets since they may carry
    // MySQL-specific errors reported by failed queries.
    _resultData.resultSets[request->workerName()].push_back(request->responseData());

    // Try submitting a replacement request for the same worker. If none
    // would be launched then evaluate for the completion condition of the job.
    auto const requests = launchRequests(lock, request->workerName());
    auto itr = _requests.insert(_requests.cend(), requests.cbegin(), requests.cend());
    if (_requests.cend() == itr) {
        if (_requests.size() == _numFinished) {
            size_t numSuccess = 0;
            for (auto&& ptr : _requests) {
                if (ptr->extendedState() == Request::ExtendedState::SUCCESS) {
                    numSuccess++;
                } else if (ptr->extendedServerStatus() == ProtocolStatusExt::MULTIPLE) {
                    // These special conditions are counted as a success for tables
                    // that might be successfully processed before by another instance
                    // the same job.
                    auto&& responseData = request->responseData();
                    if (responseData.hasErrors()) {
                        if (ignoreNonPartitioned()) {
                            if (responseData.allErrorsOf(ProtocolStatusExt::NOT_PARTITIONED_TABLE)) {
                                numSuccess++;
                            }
                        }
                        if (ignoreDuplicateKey()) {
                            if (responseData.allErrorsOf(ProtocolStatusExt::DUPLICATE_KEY)) {
                                numSuccess++;
                            }
                        }
                    }
                }
            }
            processResultAndFinish(
                    lock, numSuccess == _numFinished ? ExtendedState::SUCCESS : ExtendedState::FAILED);
        }
    }
}

void SqlJob::processResultAndFinish(replica::Lock const& lock, ExtendedState extendedState) {
    finish(lock, extendedState);
}

vector<vector<string>> SqlJob::distributeTables(vector<string> const& allTables, size_t numBins) {
    // If the total number of tables if less than the number of bins
    // then we won't be constructing empty bins.
    vector<vector<string>> tablesPerBin(min(numBins, allTables.size()));
    if (not tablesPerBin.empty()) {
        // The trivial 'round-robin'
        for (size_t i = 0; i < allTables.size(); ++i) {
            auto const bin = i % tablesPerBin.size();
            tablesPerBin[bin].push_back(allTables[i]);
        }
    }
    return tablesPerBin;
}

vector<string> SqlJob::workerTables(string const& workerName, string const& databaseName,
                                    string const& tableName, bool allTables, bool overlapTablesOnly,
                                    bool includeProtoTable) const {
    vector<string> tables;
    if (_isPartitioned(databaseName, tableName)) {
        // The prototype table for creating chunks and chunk overlap tables
        if (allTables || includeProtoTable) {
            tables.push_back(tableName);
        }
        // Always include the "dummy" chunk even if it won't be explicitly found
        // in the replica collection. This chunk must be present at all workers.
        bool const overlap = true;
        if (allTables || overlapTablesOnly) {
            tables.push_back(ChunkedTable(tableName, lsst::qserv::DUMMY_CHUNK, overlap).name());
        }
        if (allTables || !overlapTablesOnly) {
            tables.push_back(ChunkedTable(tableName, lsst::qserv::DUMMY_CHUNK, !overlap).name());
        }

        // Locate all chunks registered on the worker. These chunks will be used
        // to build names of the corresponding chunk-specific partitioned tables.
        vector<ReplicaInfo> replicas;
        controller()->serviceProvider()->databaseServices()->findWorkerReplicas(replicas, workerName,
                                                                                databaseName);

        for (auto&& replica : replicas) {
            auto const chunk = replica.chunk();
            // Avoiding the "dummy" chunk as it was already forced to be in
            // the collection.
            if (chunk != lsst::qserv::DUMMY_CHUNK) {
                if (allTables or overlapTablesOnly) {
                    tables.push_back(ChunkedTable(tableName, chunk, overlap).name());
                }
                if (allTables or not overlapTablesOnly) {
                    tables.push_back(ChunkedTable(tableName, chunk, !overlap).name());
                }
            }
        }
    } else {
        tables.push_back(tableName);
    }
    return tables;
}

vector<string> SqlJob::workerTables(string const& workerName, TransactionId const& transactionId,
                                    string const& tableName, bool allTables, bool overlapTablesOnly,
                                    bool includeProtoTable) const {
    vector<string> tables;
    auto const databaseServices = controller()->serviceProvider()->databaseServices();
    TransactionInfo const transaction = databaseServices->transaction(transactionId);

    // Locate all contributions into the table made at the given worker.
    vector<TransactionContribInfo> const contribs =
            databaseServices->transactionContribs(transaction.id, tableName, workerName);

    if (_isPartitioned(transaction.database, tableName)) {
        // The prototype table for creating chunks and chunk overlap tables
        if (allTables || includeProtoTable) {
            tables.push_back(tableName);
        }
        // Always include the "dummy" chunk even if it won't be explicitly found
        // in the replica collection. This chunk must be present at all workers.
        bool const overlap = true;
        if (allTables || overlapTablesOnly) {
            tables.push_back(ChunkedTable(tableName, lsst::qserv::DUMMY_CHUNK, overlap).name());
        }
        if (allTables || !overlapTablesOnly) {
            tables.push_back(ChunkedTable(tableName, lsst::qserv::DUMMY_CHUNK, !overlap).name());
        }
        for (auto&& contrib : contribs) {
            // Avoiding the "dummy" chunk as it was already forced to be in
            // the collection.
            if (contrib.chunk != lsst::qserv::DUMMY_CHUNK) {
                if (allTables || overlapTablesOnly) {
                    tables.push_back(ChunkedTable(tableName, contrib.chunk, overlap).name());
                }
                if (allTables || !overlapTablesOnly) {
                    tables.push_back(ChunkedTable(tableName, contrib.chunk, !overlap).name());
                }
            }
        }
    } else {
        for (auto&& contrib : contribs) {
            tables.push_back(contrib.table);
        }
    }
    return tables;
}

bool SqlJob::_isPartitioned(string const& databaseName, string const& tableName) const {
    return controller()
            ->serviceProvider()
            ->config()
            ->databaseInfo(databaseName)
            .findTable(tableName)
            .isPartitioned;
}

}  // namespace lsst::qserv::replica
