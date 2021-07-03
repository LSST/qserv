// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2017 AURA/LSST.
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
/**
  * @file
  *
  * @brief Interface for managing the execution of user queries, that is,
  * queries as they are submitted by the user. The generation of smaller
  * chunk-level queries is handled here or by delegate classes.
  *
  * Basic usage:
  *
  * After constructing a UserQuery object ...
  *
  * getRestrictors() -- retrieve restrictors to be passed to spatial region selection code in another layer.
  *
  * getDominantDb() -- retrieve the "dominantDb", that is, the database whose
  * partitioning will be used for chunking and dispatch.
  *
  * getDbStriping() -- retrieve the striping parameters of the dominantDb.
  *
  * getError() -- See if there are errors
  *
  * getExecDesc() -- see how execution is progressing
  *
  * addChunk() -- Add a chunk number (and subchunks, as appropriate) to be
  * dispatched during submit(). The czar uses getRestrictors and getDbStriping
  * to query a region selector over a chunk number generator and an emptychunks
  * list to compute the relevant chunk numbers.
  *
  * submit() -- send the query (in generated fragments) to the cluster for
  * execution.
  *
  * join() -- block until query execution is complete (or encounters errors)
  *
  * kill() -- stop a query in progress
  *
  * discard() -- release resources for this query.
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "ccontrol/UserQuerySelect.h"

// System headers
#include <cassert>
#include <chrono>
#include <memory>

// Third-party headers
#include <boost/algorithm/string/replace.hpp>

#include "qdisp/QdispPool.h"
// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/MergingHandler.h"
#include "ccontrol/TmpTableName.h"
#include "ccontrol/UserQueryError.h"
#include "global/constants.h"
#include "global/LogContext.h"
#include "global/MsgReceiver.h"
#include "proto/worker.pb.h"
#include "proto/ProtoImporter.h"
#include "qdisp/Executive.h"
#include "qdisp/MessageStore.h"
#include "qmeta/QMeta.h"
#include "qmeta/Exceptions.h"
#include "qproc/geomAdapter.h"
#include "qproc/IndexMap.h"
#include "qproc/QuerySession.h"
#include "qproc/TaskMsgFactory.h"
#include "query/ColumnRef.h"
#include "query/FromList.h"
#include "query/JoinRef.h"
#include "query/QueryTemplate.h"
#include "query/SelectList.h"
#include "query/SelectStmt.h"
#include "query/ValueExpr.h"
#include "query/ValueFactor.h"
#include "rproc/InfileMerger.h"
#include "sql/Schema.h"
#include "util/IterableFormatter.h"
#include "util/ThreadPriority.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQuerySelect");
} // namespace

namespace lsst {
namespace qserv {

/// A class that can be used to parameterize a ProtoImporter<TaskMsg> for
/// debugging purposes
class ProtoPrinter {
public:
    ProtoPrinter() {}
    virtual void operator()(std::shared_ptr<proto::TaskMsg> m) {
        std::cout << "Got taskmsg ok";
    }
    virtual ~ProtoPrinter() {}
};

/// Factory to create chunkid-specific MsgReceiver objs linked to the right
/// messagestore
class ChunkMsgReceiver : public MsgReceiver {
public:
    virtual void operator()(int code, std::string const& msg) {
            messageStore->addMessage(chunkId, code, msg);
        }
    static std::shared_ptr<ChunkMsgReceiver>
    newInstance(int chunkId,
                std::shared_ptr<qdisp::MessageStore> ms) {
        std::shared_ptr<ChunkMsgReceiver> r = std::make_shared<ChunkMsgReceiver>();
        r->chunkId = chunkId;
        r->messageStore = ms;
        return r;
    }

    int chunkId;
    std::shared_ptr<qdisp::MessageStore> messageStore;
};

////////////////////////////////////////////////////////////////////////
// UserQuerySelect implementation
namespace ccontrol {

/// Constructor
UserQuerySelect::UserQuerySelect(std::shared_ptr<qproc::QuerySession> const& qs,
                                 std::shared_ptr<qdisp::MessageStore> const& messageStore,
                                 std::shared_ptr<qdisp::Executive> const& executive,
                                 std::shared_ptr<qproc::DatabaseModels> const& dbModels,
                                 std::shared_ptr<rproc::InfileMergerConfig> const& infileMergerConfig,
                                 std::shared_ptr<qproc::SecondaryIndex> const& secondaryIndex,
                                 std::shared_ptr<qmeta::QMeta> const& queryMetadata,
                                 std::shared_ptr<qmeta::QStatus> const& queryStatsData,
                                 std::shared_ptr<util::SemaMgr> const& semaMgrConn,
                                 qmeta::CzarId czarId,
                                 std::shared_ptr<qdisp::QdispPool> const& qdispPool,
                                 std::string const& errorExtra,
                                 bool async,
                                 std::string const& resultDb)
    :  _qSession(qs), _messageStore(messageStore), _executive(executive),
       _databaseModels(dbModels), _infileMergerConfig(infileMergerConfig),
       _secondaryIndex(secondaryIndex),
       _queryMetadata(queryMetadata), _queryStatsData(queryStatsData),
       _semaMgrConn(semaMgrConn),
       _qMetaCzarId(czarId), _qdispPool(qdispPool),
       _errorExtra(errorExtra), _resultDb(resultDb), _async(async) {
}

std::string UserQuerySelect::getError() const {
    std::string div = (_errorExtra.size() && _qSession->getError().size())
        ? " " : "";
    return _qSession->getError() + div + _errorExtra;
}

/// Attempt to kill query in progress.
void UserQuerySelect::kill() {
    LOGS(_log, LOG_LVL_DEBUG, "UserQuerySelect kill");
    std::lock_guard<std::mutex> lock(_killMutex);
    if (!_killed) {
        _killed = true;
        try {
            // make a copy of executive pointer to keep it alive and avoid race
            // with pointer being reset in discard() method
            std::shared_ptr<qdisp::Executive> exec = _executive;
            if (exec != nullptr) {
                exec->squash();
            }
        } catch(UserQueryError const &e) {
            // Silence merger discarding errors, because this object is being
            // released. Client no longer cares about merger errors.
        }
        _qMetaUpdateStatus(qmeta::QInfo::ABORTED);
    }
}


std::string
UserQuerySelect::_getResultOrderBy() const {
    return _qSession->getResultOrderBy();
}


std::string UserQuerySelect::getResultQuery() const {
    query::SelectList selectList;
    auto const& valueExprList = *_qSession->getStmt().getSelectList().getValueExprList();
    for (auto const& valueExpr : valueExprList) {
        if (valueExpr->isStar()) {
            auto useSelectList = std::make_shared<query::SelectList>();
            useSelectList->addValueExpr(valueExpr);
            query::SelectStmt starStmt(useSelectList, _qSession->getStmt().getFromList().clone());
            sql::Schema schema;
            if (not _infileMerger->getSchemaForQueryResults(starStmt, schema)) {
                _errorExtra = "Internal error getting schema for query results:" +
                    _infileMerger->getError().getMsg();
            }
            for (auto const& column : schema.columns) {
                selectList.addValueExpr(query::ValueExpr::newColumnExpr(column.name));
            }
        } else {
            // Add a column that describes the top-level ValueExpr.
            // If the value is a column ref _and_ there was not a user defined alias, then the TablePlugin will
            // have assigned an alias that included the table name. We don't want that table name to appear
            // in the results in that case, so just assign the column.
            // Otherwise, use the alias.
            std::shared_ptr<query::ValueExpr> newValueExpr;
            if (valueExpr->isColumnRef() && not valueExpr->getAliasIsUserDefined()) {
                newValueExpr = query::ValueExpr::newColumnExpr(valueExpr->getAlias());
                newValueExpr->setAlias(valueExpr->getColumnRef()->getColumn());
            } else {
                newValueExpr = query::ValueExpr::newColumnExpr(valueExpr->getAlias());
                newValueExpr->setAlias(valueExpr->getAlias());
            }
            selectList.addValueExpr(newValueExpr);
        }
    }

    // The SELECT list needs to define aliases in the result query, so that the columns we are selecting from
    // the result table that may be mangled by internal handling of the query are restored to the column name
    // that the user expects, by way of the alias defined here.
    query::QueryTemplate qt(query::QueryTemplate::DEFINE_VALUE_ALIAS_USE_TABLE_ALIAS);
    selectList.renderTo(qt);

    std::string resultQuery =  "SELECT " + qt.sqlFragment() + " FROM " + _resultDb + "."
        + getResultTableName();
    std::string orderBy = _getResultOrderBy();
    if (not orderBy.empty()) {
        resultQuery += " " + orderBy;
    }
    LOGS(_log, LOG_LVL_DEBUG, "made result query:" << resultQuery);
    return resultQuery;
}


/// Begin running on all chunks added so far.
void UserQuerySelect::submit() {
    _qSession->finalize();

    // Using the QuerySession, generate query specs (text, db, chunkId) and then
    // create query messages and send them to the async query manager.
    LOGS(_log, LOG_LVL_DEBUG, "UserQuerySelect beginning submission");
    assert(_infileMerger);

    auto taskMsgFactory = std::make_shared<qproc::TaskMsgFactory>(_qMetaQueryId);
    TmpTableName ttn(_qMetaQueryId, _qSession->getOriginal());
    std::vector<int> chunks;
    std::mutex chunksMtx;
    int sequence = 0;

    auto queryTemplates = _qSession->makeQueryTemplates();

    LOGS(_log, LOG_LVL_DEBUG, "first query template:" <<
        (queryTemplates.size() > 0 ? queryTemplates[0].sqlFragment() : "none produced."));

    // Writing query for each chunk, stop if query is cancelled.
    // attempt to change priority, requires root
    bool increaseThreadPriority = false;  // TODO: add to configuration
    util::ThreadPriority threadPriority(pthread_self());
    if (increaseThreadPriority) {
        threadPriority.storeOriginalValues();
        threadPriority.setPriorityPolicy(10);
    }

    // Add QStatsTmp table entry
    try {
        _queryStatsData->queryStatsTmpRegister(_qMetaQueryId, _qSession->getChunksSize());
    } catch (qmeta::SqlError const& e) {
        LOGS(_log, LOG_LVL_WARN, "Failed queryStatsTmpRegister " << e.what());
    }

    _executive->setScanInteractive(_qSession->getScanInteractive());

    for(auto i = _qSession->cQueryBegin(), e = _qSession->cQueryEnd();
            i != e && !_executive->getCancelled(); ++i) {
        auto& chunkSpec = *i;

        std::function<void(util::CmdData*)> funcBuildJob =
                [this, sequence,     // sequence must be a copy
                 &chunkSpec, &queryTemplates,
                 &chunks, &chunksMtx, &ttn,
                 &taskMsgFactory](util::CmdData*) {

            QSERV_LOGCONTEXT_QUERY(_qMetaQueryId);

            qproc::ChunkQuerySpec::Ptr cs;
            {
                std::lock_guard<std::mutex> lock(chunksMtx);
                cs = _qSession->buildChunkQuerySpec(queryTemplates, chunkSpec);
                chunks.push_back(cs->chunkId);
            }
            std::string chunkResultName = ttn.make(cs->chunkId);

            std::shared_ptr<ChunkMsgReceiver> cmr = ChunkMsgReceiver::newInstance(cs->chunkId, _messageStore);
            ResourceUnit ru;
            ru.setAsDbChunk(cs->db, cs->chunkId);
            qdisp::JobDescription::Ptr jobDesc = qdisp::JobDescription::create(
                    _executive->getId(), sequence, ru,
                    std::make_shared<MergingHandler>(cmr, _infileMerger, chunkResultName),
                    taskMsgFactory, cs, chunkResultName);
            _executive->add(jobDesc);
        };

        auto cmd = std::make_shared<qdisp::PriorityCommand>(funcBuildJob);
        _executive->queueJobStart(cmd);
        ++sequence;
    }

    // attempt to restore original thread priority, requires root
    if (increaseThreadPriority) {
        threadPriority.restoreOriginalValues();
    }

    LOGS(_log, LOG_LVL_DEBUG, "total jobs in query=" << sequence);
    _executive->waitForAllJobsToStart();

    // we only care about per-chunk info for ASYNC queries
    if (_async) {
        std::lock_guard<std::mutex> lock(chunksMtx);
        _qMetaAddChunks(chunks);
    }
}


/// Block until a submit()'ed query completes.
/// @return the QueryState indicating success or failure
QueryState UserQuerySelect::join() {
    bool successful = _executive->join(); // Wait for all data
    // Since all data are in, run final SQL commands like GROUP BY.
    if (!_infileMerger->finalize()) {
        successful = false;
        LOGS(_log, LOG_LVL_ERROR, "InfileMerger::finalize failed");
        // Error: 1105 SQLSTATE: HY000 (ER_UNKNOWN_ERROR) Message: Unknown error
        _messageStore->addMessage(-1, 1105, "Failure while merging result",
                MessageSeverity::MSG_ERROR);
    }
    try {
        _discardMerger();
    } catch (std::exception const& exc) {
        // exception here means error in qserv logic, we do not want to leak
        // it or expose it to user, just dump it to log
        LOGS(_log, LOG_LVL_ERROR, "exception from _discardMerger: "<< exc.what());
    }
    if (successful) {
        _qMetaUpdateStatus(qmeta::QInfo::COMPLETED);
        LOGS(_log, LOG_LVL_DEBUG, "Joined everything (success)");
        return SUCCESS;
    } else if (_killed) {
        // status is already set to ABORTED
        LOGS(_log, LOG_LVL_ERROR, "Joined everything (killed)");
        return ERROR;
    } else {
        _qMetaUpdateStatus(qmeta::QInfo::FAILED);
        LOGS(_log, LOG_LVL_ERROR, "Joined everything (failure!)");
        return ERROR;
    }
}

/// Release resources held by the merger
void UserQuerySelect::_discardMerger() {
    _infileMergerConfig.reset();
    if (_infileMerger && !_infileMerger->isFinished()) {
        throw UserQueryError(getQueryIdString() + " merger unfinished, cannot discard");
    }
    _infileMerger.reset();
}

/// Release resources.
void UserQuerySelect::discard() {
    {
        std::lock_guard<std::mutex> lock(_killMutex);
        if (_killed) {
            return;
        }
    }
    // Make sure resources are released.
    if (_executive && _executive->getNumInflight() > 0) {
        throw UserQueryError(getQueryIdString() + " Executive unfinished, cannot discard");
    }
    _executive.reset();
    _messageStore.reset();
    _qSession.reset();
    try {
        _discardMerger();
    } catch(UserQueryError const &e) {
        // Silence merger discarding errors, because this object is being released.
        // client no longer cares about merger errors.
    }
    LOGS(_log, LOG_LVL_DEBUG, "Discarded UserQuerySelect");
}

/// Setup merger (for results handling and aggregation)
void UserQuerySelect::setupMerger() {
    LOGS(_log, LOG_LVL_TRACE, "Setup merger");
    _infileMergerConfig->targetTable = _resultTable;
    _infileMergerConfig->mergeStmt = _qSession->getMergeStmt();
    LOGS(_log, LOG_LVL_DEBUG, "setting mergeStmt:" <<
        (_infileMergerConfig->mergeStmt != nullptr ?
            _infileMergerConfig->mergeStmt->getQueryTemplate().sqlFragment() : "nullptr"));
    _infileMerger = std::make_shared<rproc::InfileMerger>(*_infileMergerConfig, _databaseModels, _semaMgrConn);

    auto&& preFlightStmt = _qSession->getPreFlightStmt();
    if (preFlightStmt == nullptr) {
        _qMetaUpdateStatus(qmeta::QInfo::FAILED);
        _errorExtra = "Could not create results table for query (no worker queries).";
        return;
    }
    if (not _infileMerger->makeResultsTableForQuery(*preFlightStmt)) {
        _errorExtra = _infileMerger->getError().getMsg();
        _qMetaUpdateStatus(qmeta::QInfo::FAILED);
    }

    _expandSelectStarInMergeStatment(_infileMergerConfig->mergeStmt);

    _infileMerger->setMergeStmtFromList(_infileMergerConfig->mergeStmt);
}


void UserQuerySelect::_expandSelectStarInMergeStatment(std::shared_ptr<query::SelectStmt> const& mergeStmt) {
    if (nullptr != mergeStmt) {
        auto& selectList = *(mergeStmt->getSelectList().getValueExprList());
        for (auto valueExprItr = selectList.begin(); valueExprItr != selectList.end(); ++valueExprItr) {
            auto& valueExpr = *valueExprItr;
            if (valueExpr->isStar()) {
                auto valueExprVec = std::make_shared<query::ValueExprPtrVector>();
                valueExprVec->push_back(valueExpr);
                auto starStmt = query::SelectStmt(std::make_shared<query::SelectList>(valueExprVec),
                                                  mergeStmt->getFromListPtr());
                sql::Schema schema;
                if (not _infileMerger->getSchemaForQueryResults(starStmt, schema)) {
                    throw UserQueryError(getQueryIdString() + " Couldn't get schema for merge query.");
                }
                // Only use the column names retured by the SELECT* (not the database or table name) because
                // when performing the merge the columns will be in the merge table (not the table that was
                // originally queried).
                query::ValueExprPtrVector starColumns;
                for (auto const& column : schema.columns) {
                    starColumns.push_back(query::ValueExpr::newColumnExpr("", "", "", column.name));
                }
                valueExprItr = selectList.insert(valueExprItr, starColumns.begin(), starColumns.end());
                std::advance(valueExprItr, starColumns.size());
                // erase the STAR ValueExpr becasue it's been replaced with named columns.
                valueExprItr = selectList.erase(valueExprItr);
                if (valueExprItr == selectList.end()) break;
            }
        }
    }
}


void UserQuerySelect::saveResultQuery() {
    _queryMetadata->saveResultQuery(_qMetaQueryId, getResultQuery());
}

void UserQuerySelect::setupChunking() {
    LOGS(_log, LOG_LVL_TRACE, "Setup chunking");
    // Do not throw exceptions here, set _errorExtra .
    std::shared_ptr<qproc::IndexMap> im;
    std::string dominantDb = _qSession->getDominantDb();
    if (dominantDb.empty() || !_qSession->validateDominantDb()) {
        // TODO: Revisit this for L3
        throw UserQueryError(getQueryIdString() + " Couldn't determine dominantDb for dispatch");
    }

    std::shared_ptr<IntSet const> eSet = _qSession->getEmptyChunks();
    {
        eSet = _qSession->getEmptyChunks();
        if (!eSet) {
            eSet = std::make_shared<IntSet>();
            LOGS(_log, LOG_LVL_WARN, "Missing empty chunks info for " << dominantDb);
        }
    }
    // FIXME add operator<< for QuerySession
    LOGS(_log, LOG_LVL_TRACE, "_qSession: " << _qSession);
    if (_qSession->hasChunks()) {
        auto areaRestrictors = _qSession->getAreaRestrictors();
        auto secIdxRestrictors = _qSession->getSecIdxRestrictors();
        css::StripingParams partStriping = _qSession->getDbStriping();

        im = std::make_shared<qproc::IndexMap>(partStriping, _secondaryIndex);
        qproc::ChunkSpecVector csv;
        if (areaRestrictors != nullptr || secIdxRestrictors != nullptr) {
            csv = im->getChunks(areaRestrictors, secIdxRestrictors);
        } else { // Unrestricted: full-sky
            csv = im->getAllChunks();
        }

        LOGS(_log, LOG_LVL_TRACE, "Chunk specs: " << util::printable(csv));
        // Filter out empty chunks
        for(qproc::ChunkSpecVector::const_iterator i=csv.begin(), e=csv.end();
            i != e;
            ++i) {
            if (eSet->count(i->chunkId) == 0) { // chunk not in empty?
                _qSession->addChunk(*i);
            }
        }
    } else {
        LOGS(_log, LOG_LVL_TRACE, "No chunks added, QuerySession will add dummy chunk");
    }
    _qSession->setScanInteractive();
}

// register query in qmeta database
void UserQuerySelect::qMetaRegister(std::string const& resultLocation, std::string const& msgTableName)
{
    qmeta::QInfo::QType qType = _async ? qmeta::QInfo::ASYNC : qmeta::QInfo::SYNC;
    std::string user = "anonymous";    // we do not have access to that info yet

    std::string qTemplate;
    auto const& stmtVector = _qSession->getStmtParallel();
    for (auto itr = stmtVector.begin(); itr != stmtVector.end(); ++ itr) {
        auto stmt = *itr;
        if (stmt) {
            if (not qTemplate.empty()) {
                // if there is more than one statement separate them by
                // special token
                qTemplate += " /*QSEPARATOR*/; ";
            }
            qTemplate += stmt->getQueryTemplate().sqlFragment();
        }
    }

    std::string qMerge;
    auto mergeStmt = _qSession->getMergeStmt();
    if (mergeStmt) {
        qMerge = mergeStmt->getQueryTemplate().sqlFragment();
    }
    _resultLoc = resultLocation;
    if (_resultLoc.empty()) {
        // Special token #QID# is replaced with query ID later.
        _resultLoc = "table:result_#QID#";
    }
    qmeta::QInfo qInfo(qType, _qMetaCzarId, user, _qSession->getOriginal(),
                       qTemplate, qMerge, _resultLoc, msgTableName, "");

    // find all table names used by statement (which appear in FROM ... [JOIN ...])
    qmeta::QMeta::TableNames tableNames;
    const auto& tables = _qSession->getStmt().getFromList().getTableRefList();
    for (auto itr = tables.begin(); itr != tables.end(); ++ itr) {
        // add table name
        tableNames.push_back(std::make_pair((*itr)->getDb(), (*itr)->getTable()));

        // add its joins if any
        const auto& joins = (*itr)->getJoins();
        for (auto jtr = joins.begin(); jtr != joins.end(); ++ jtr) {
            const auto& right = (*jtr)->getRight();
            if (right) {
                tableNames.push_back(std::make_pair(right->getDb(), right->getTable()));
            }
        }
    }

    // register query, save its ID
    _qMetaQueryId = _queryMetadata->registerQuery(qInfo, tableNames);
    _queryIdStr = QueryIdHelper::makeIdStr(_qMetaQueryId);
    // Add logging context with query ID
    QSERV_LOGCONTEXT_QUERY(_qMetaQueryId);
    LOGS(_log, LOG_LVL_DEBUG, "UserQuery registered " << _qSession->getOriginal());

    // update #QID# with actual query ID
    boost::replace_all(_resultLoc, "#QID#", std::to_string(_qMetaQueryId));

    // guess query result location
    if (_resultLoc.compare(0, 6, "table:") == 0) {
        _resultTable = _resultLoc.substr(6);
    } else {
        // we only support results going to tables for now, abort for anything else
        std::string const msg = "Unexpected result location '" + _resultLoc + "'";
        _messageStore->addMessage(-1, 1146, msg, MessageSeverity::MSG_ERROR);
        throw UserQueryError(getQueryIdString() + _errorExtra);
    }

    if (_executive != nullptr) {
        _executive->setQueryId(_qMetaQueryId);
    } else {
        LOGS(_log, LOG_LVL_WARN, "No Executive, assuming invalid query");
    }

    // Note that ordering is important here, this check must happen after
    // query is registered in qmeta
    for (auto itr = tableNames.begin(); itr != tableNames.end(); ++ itr) {
        if (not _qSession->containsTable(itr->first, itr->second)) {
            // table either does not exist or it is being deleted, we must stop
            // here but we must mark query as failed
            _qMetaUpdateStatus(qmeta::QInfo::FAILED);

            // Throwing exception stops submit() but it does not set any
            // error condition, only prints error message to the log. To communicate
            // error message to caller we need to set _errorExtra
            std::string const msg = "Table '" + itr->first + "." + itr->second + "' does not exist";
            _messageStore->addMessage(-1, 1146, msg, MessageSeverity::MSG_ERROR);
            throw UserQueryError(getQueryIdString() + _errorExtra);
        }
    }
}


// update query status in QMeta
void UserQuerySelect::_qMetaUpdateStatus(qmeta::QInfo::QStatus qStatus)
{
    _queryMetadata->completeQuery(_qMetaQueryId, qStatus);
    // Remove the row for temporary query statistics.
    try {
        _queryStatsData->queryStatsTmpRemove(_qMetaQueryId);
    } catch (qmeta::SqlError const&) {
        LOGS(_log, LOG_LVL_WARN, "queryStatsTmp remove failed " << _queryIdStr);
    }
}

// add chunk information to qmeta
void UserQuerySelect::_qMetaAddChunks(std::vector<int> const& chunks)
{
    _queryMetadata->addChunks(_qMetaQueryId, chunks);
}


/// Return this query's QueryId string.
std::string UserQuerySelect::getQueryIdString() const {
    return _queryIdStr;
}

}}} // lsst::qserv::ccontrol
