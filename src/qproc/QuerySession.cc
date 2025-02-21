// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2017 AURA/LSST.
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
 * @brief Implementation of the class QuerySession, which is a
 * container for input query state (and related state available prior
 * to execution). Also includes QuerySession::Iter and initQuerySession()
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "qproc/QuerySession.h"

// System headers
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <sstream>
#include <stdexcept>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/ParseRunner.h"
#include "css/CssAccess.h"
#include "css/CssError.h"
#include "css/EmptyChunks.h"
#include "global/constants.h"
#include "global/stringTypes.h"
#include "parser/ParseException.h"
#include "qana/AggregatePlugin.h"
#include "qana/AnalysisError.h"
#include "qana/DuplSelectExprPlugin.h"
#include "qana/MatchTablePlugin.h"
#include "qana/PostPlugin.h"
#include "qana/QservRestrictorPlugin.h"
#include "qana/QueryMapping.h"
#include "qana/QueryPlugin.h"
#include "qana/ScanTablePlugin.h"
#include "qana/TablePlugin.h"
#include "qana/WherePlugin.h"
#include "qproc/DatabaseModels.h"
#include "qproc/QueryProcessingBug.h"
#include "query/AreaRestrictor.h"
#include "query/QueryContext.h"
#include "query/SecIdxRestrictor.h"
#include "query/SelectList.h"
#include "query/SelectStmt.h"
#include "query/SelectList.h"
#include "query/typedefs.h"
#include "sql/SqlException.h"
#include "util/IterableFormatter.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qproc.QuerySession");

std::string printParallel(lsst::qserv::query::SelectStmtPtrVector const& p) {
    std::string ret;
    for (auto&& selectStmtPtr : p) {
        ret += "        ";
        ret += selectStmtPtr->getQueryTemplate().sqlFragment();
        ret += "\n";
    }
    return ret;
}

#define LOG_STATEMENTS(LEVEL, PRETEXT)                                                                      \
    LOGS(_log, LEVEL,                                                                                       \
         '\n' << "  " << PRETEXT << '\n'                                                                    \
              << "    stmt:" << (_stmt != nullptr ? _stmt->getQueryTemplate().sqlFragment() : "nullptr")    \
              << '\n'                                                                                       \
              << "    stmtParallel:" << '\n'                                                                \
              << printParallel(_stmtParallel) << "    stmtPreFlight:"                                       \
              << (_stmtPreFlight != nullptr ? _stmtPreFlight->getQueryTemplate().sqlFragment() : "nullptr") \
              << '\n'                                                                                       \
              << "    stmtMerge:"                                                                           \
              << (_stmtMerge != nullptr ? _stmtMerge->getQueryTemplate().sqlFragment() : "nullptr") << '\n' \
              << "    needsMerge:" << (needsMerge() ? "true" : "false"));
}  // namespace

namespace lsst::qserv::qproc {

////////////////////////////////////////////////////////////////////////
// class QuerySession
////////////////////////////////////////////////////////////////////////

std::shared_ptr<query::SelectStmt> QuerySession::parseQuery(std::string const& statement) {
    ccontrol::ParseRunner::Ptr parser;
    try {
        parser = std::make_shared<ccontrol::ParseRunner>(statement);
    } catch (parser::ParseException const& e) {
        LOGS(_log, LOG_LVL_DEBUG, "parse exception: " << e.what());
        _original = statement;
        _error = std::string("ParseException:") + e.what();
        return nullptr;
    }
    return parser->getSelectStmt();
}

// Analyze SQL query issued by user
void QuerySession::analyzeQuery(std::string const& sql, std::shared_ptr<query::SelectStmt> const& stmt) {
    _original = sql;
    _stmt = stmt;
    _isFinal = false;
    _initContext();
    assert(_context.get());

    try {
        _preparePlugins();
        _applyLogicPlugins();
        _generateConcrete();
        _applyConcretePlugins();

        LOGS(_log, LOG_LVL_TRACE, "Query Plugins applied: " << *this);
        LOGS(_log, LOG_LVL_TRACE, "ORDER BY clause for result query: " << getResultOrderBy());

    } catch (QueryProcessingBug& b) {
        _error = std::string("QuerySession bug:") + b.what();
    } catch (qana::AnalysisError& e) {
        _error = std::string("AnalysisError:") + e.what();
    } catch (css::NoSuchDb& e) {
        _error = std::string("NoSuchDb(css):") + e.what();
    } catch (css::NoSuchTable& e) {
        _error = std::string("NoSuchTable(css):") + e.what();
    } catch (sql::NoSuchDb& e) {
        _error = std::string("NoSuchDb(sql):") + e.what();
    } catch (sql::NoSuchTable& e) {
        _error = std::string("NoSuchTable(sql):") + e.what();
    } catch (util::Bug& b) {
        _error = std::string("Qserv bug:") + b.what();
    } catch (std::exception const& e) {
        _error = std::string("analyzeQuery unexpected:") + e.what();
    }
}

bool QuerySession::needsMerge() const {
    // Aggregate: having an aggregate fct spec in the select list.
    // Stmt itself knows whether aggregation is present. More
    // generally, aggregation is a separate pass. In computing a
    // multi-pass execution, the statement makes use of a (proper,
    // probably) subset of its components to compose each pass. Right
    // now, the only goal is to support aggregation using two passes.
    return _context->needsMerge;
}

bool QuerySession::hasChunks() const { return _context->hasChunks(); }

query::AreaRestrictorVecPtr QuerySession::getAreaRestrictors() const { return _context->areaRestrictors; }

query::SecIdxRestrictorVecPtr QuerySession::getSecIdxRestrictors() const {
    return _context->secIdxRestrictors;
}

std::string QuerySession::getResultOrderBy() const {
    std::string orderBy;
    if (_stmt->hasOrderBy()) {
        orderBy = _stmt->getOrderBy().sqlFragment();
    }
    LOGS(_log, LOG_LVL_TRACE, "getResultOrderBy: " << orderBy);
    return orderBy;
}

void QuerySession::addChunk(ChunkSpec const& cs) {
    LOGS(_log, LOG_LVL_TRACE, "Add chunk: " << cs);
    _context->chunkCount += 1;
    _chunks.push_back(cs);
}

void QuerySession::setScanInteractive() {
    // Default is for interactive scan.
    if (_context->chunkCount > _interactiveChunkLimit) {
        _scanInteractive = false;
    }
}

void QuerySession::setDummy() {
    _isDummy = true;
    // Clear out chunk counts and _chunks, and replace with dummy chunk.
    _context->chunkCount = 1;
    _chunks.clear();
    IntVector v;
    v.push_back(1);  // Dummy subchunk
    _chunks.push_back(ChunkSpec(DUMMY_CHUNK, v));
}

std::string const& QuerySession::getDominantDb() const {
    return _context->dominantDb;  // parsed query's dominant db (via TablePlugin)
}

bool QuerySession::containsDb(std::string const& dbName) const { return _context->containsDb(dbName); }

bool QuerySession::containsTable(std::string const& dbName, std::string const& tableName) const {
    return _context->containsTable(dbName, tableName);
}

bool QuerySession::validateDominantDb() const { return _context->containsDb(_context->dominantDb); }

css::StripingParams QuerySession::getDbStriping() { return _context->getDbStriping(); }

std::shared_ptr<IntSet const> QuerySession::getEmptyChunks() {
    // FIXME: do we need to catch an exception here?
    if (_css != nullptr) {
        LOGS(_log, LOG_LVL_TRACE, "QuerySession::getEmptyChunks " << _context->dominantDb);
        std::shared_ptr<IntSet const> result = _css->getEmptyChunks()->getEmpty(_context->dominantDb);
        return result;
    } else {
        LOGS(_log, LOG_LVL_WARN, "QuerySession::getEmptyChunks no _css");
        std::shared_ptr<IntSet const> res;
        return res;
    }
}

/// Returns the merge statment, if appropriate.
/// If a post-execution merge fixup is not needed, return a NULL pointer.
std::shared_ptr<query::SelectStmt> QuerySession::getMergeStmt() const {
    if (_context->needsMerge) {
        return _stmtMerge;
    } else {
        return nullptr;
    }
}

void QuerySession::finalize() {
    if (_isFinal) {
        return;
    }
    QueryPluginPtrVector::iterator i;
    for (i = _plugins->begin(); i != _plugins->end(); ++i) {
        (**i).applyFinal(*_context);
    }
    // Make up for no chunks (chunk-less query): add the dummy chunk.
    if (_chunks.empty()) {
        setDummy();
    }
}

QuerySession::QuerySession(Test& t) : _css(t.css), _defaultDb(t.defaultDb) {
    sql::SqlConfig sqlConfig(t.sqlConfig);
    _databaseModels = DatabaseModels::create(sqlConfig, sqlConfig);
    _initContext();
}

QuerySession::QuerySession() {
    sql::SqlConfig sqlConfig(sql::SqlConfig::MOCK);
    /// For this testing, the local sql instance is also the master.
    _databaseModels = DatabaseModels::create(sqlConfig, sqlConfig);
}

void QuerySession::_initContext() {
    _context = std::make_shared<query::QueryContext>(_defaultDb, _css, _databaseModels);
}

void QuerySession::_preparePlugins() {
    _plugins = std::make_shared<QueryPluginPtrVector>();

    _plugins->push_back(std::make_shared<qana::DuplSelectExprPlugin>());
    _plugins->push_back(std::make_shared<qana::WherePlugin>());
    _plugins->push_back(std::make_shared<qana::AggregatePlugin>());
    _plugins->push_back(std::make_shared<qana::TablePlugin>());
    _plugins->push_back(std::make_shared<qana::MatchTablePlugin>());
    _plugins->push_back(std::make_shared<qana::QservRestrictorPlugin>());
    _plugins->push_back(std::make_shared<qana::PostPlugin>());
    _plugins->push_back(std::make_shared<qana::ScanTablePlugin>(_interactiveChunkLimit));

    QueryPluginPtrVector::iterator i;
    for (i = _plugins->begin(); i != _plugins->end(); ++i) {
        (**i).prepare();
    }
}

void QuerySession::_applyLogicPlugins() {
    QueryPluginPtrVector::iterator i;
    for (auto&& plugin : *_plugins) {
        plugin->applyLogical(*_stmt, *_context);
        LOG_STATEMENTS(LOG_LVL_TRACE, "applied logical:" << plugin->name());
    }
}

void QuerySession::_generateConcrete() {
    _hasMerge = false;
    _isDummy = false;
    // In making a statement concrete, the query's execution is split
    // into a parallel portion and a merging/aggregation portion.  In
    // many cases, not much needs to be done for the latter, since
    // nearly all of the query can be parallelized.
    // If the query requires aggregation, the select list needs to get
    // converted into a parallel portion, and the merging includes the
    // post-parallel steps to merge sub-results.  When the statement
    // results in merely a collection of unordered concatenated rows,
    // the merge statement can be left empty, signifying that the sub
    // results can be concatenated directly into the output table.
    //

    // Needs to copy SelectList, since the parallel statement's
    // version will get updated by plugins. Plugins probably need
    // access to the original as a reference.
    auto parallelStmt = _stmt->clone();
    parallelStmt->setHaving(nullptr);
    _stmtParallel.push_back(parallelStmt);
    LOGS(_log, LOG_LVL_TRACE,
         "Parallel statement initialized with: \"" << _stmtParallel[0]->getQueryTemplate() << "\"");

    // Copy SelectList and Mods, but not FROM, and perhaps not
    // WHERE(???). Conceptually, we want to copy the parts that are
    // needed during merging and aggregation.
    _stmtMerge = _stmt->copyMerge();
    LOGS(_log, LOG_LVL_TRACE,
         "Merge statement initialized with: \"" << _stmtMerge->getQueryTemplate() << "\" " << *_stmtMerge);

    LOG_STATEMENTS(LOG_LVL_TRACE, "did generateConcrete:");
    // TableMerger needs to be integrated into this design.
}

void QuerySession::_applyConcretePlugins() {
    qana::QueryPlugin::Plan p(*_stmt, _stmtParallel, _stmtPreFlight, *_stmtMerge, _hasMerge);
    for (auto&& plugin : *_plugins) {
        plugin->applyPhysical(p, *_context);
        LOG_STATEMENTS(LOG_LVL_TRACE, "did applyConcretePlugins:" << plugin->name());
    }
}

/// Some code useful for debugging.
void QuerySession::print(std::ostream& os) const {
    query::QueryTemplate par;
    if (not _stmtParallel.empty()) {
        par = _stmtParallel.front()->getQueryTemplate();
    }
    query::QueryTemplate mer;
    if (_stmtMerge != nullptr) {
        mer = _stmtMerge->getQueryTemplate();
    }
    os << "QuerySession description:";
    os << "  original: \"" << this->_original << "\"";
    os << "  has chunks: " << this->hasChunks();
    os << "  chunks: " << util::printable(this->_chunks);
    os << "  needs merge: " << this->needsMerge();
    os << "  1st parallel statement: \"" << par << "\"";
    os << "  merge statement: \"" << mer << "\"";
    os << "  scanRating:" << _context->scanInfo->scanRating;
    for (auto const& tbl : _context->scanInfo->infoTables) {
        os << "  ScanTable: " << tbl.db << "." << tbl.table << " lock=" << tbl.lockInMemory
           << " rating=" << tbl.scanRating;
    }
}

std::vector<query::QueryTemplate> QuerySession::makeQueryTemplates() {
    std::vector<query::QueryTemplate> queryTemplates;
    for (auto stmtIter = _stmtParallel.begin(), e = _stmtParallel.end(); stmtIter != e; ++stmtIter) {
        queryTemplates.push_back((*stmtIter)->getQueryTemplate());
    }
    return queryTemplates;
}

std::vector<std::string> QuerySession::_buildChunkQueries(query::QueryTemplate::Vect const& queryTemplates,
                                                          ChunkSpec const& chunkSpec) const {
    std::vector<std::string> chunkQueries;
    // This logic may be pushed over to the qserv worker in the future.
    if (_stmtParallel.empty() || !_stmtParallel.front()) {
        throw QueryProcessingBug(ERR_LOC, "Attempted buildChunkQueries without _stmtParallel");
    }

    if (!_context->queryMapping) {
        throw QueryProcessingBug(ERR_LOC, "Missing QueryMapping in _context");
    }

    for (auto&& queryTemplate : queryTemplates) {
        std::string str = _context->queryMapping->apply(chunkSpec, queryTemplate);
        chunkQueries.push_back(std::move(str));
    }
    return chunkQueries;
}

std::ostream& operator<<(std::ostream& out, QuerySession const& querySession) {
    querySession.print(out);
    return out;
}

protojson::ScanInfo::Ptr QuerySession::getScanInfo() const { return _context->scanInfo; }

ChunkQuerySpec::Ptr QuerySession::buildChunkQuerySpec(query::QueryTemplate::Vect const& queryTemplates,
                                                      ChunkSpec const& chunkSpec,
                                                      bool fillInChunkIdTag) const {
    auto cQSpec = std::make_shared<ChunkQuerySpec>(_context->dominantDb, chunkSpec.chunkId,
                                                   _context->scanInfo, _scanInteractive);
    // Reset subChunkTables
    qana::QueryMapping const& queryMapping = *(_context->queryMapping);
    DbTableSet const& sTables = queryMapping.getSubChunkTables();
    cQSpec->subChunkTables = sTables;
    // Build queries.
    if (!_context->hasSubChunks()) {
        cQSpec->queries = _buildChunkQueries(queryTemplates, chunkSpec);
    } else {
        if (chunkSpec.shouldSplit()) {
            ChunkSpecFragmenter frag(chunkSpec);
            ChunkSpec s = frag.get();
            cQSpec->queries = _buildChunkQueries(queryTemplates, s);
            cQSpec->subChunkIds.assign(s.subChunks.begin(), s.subChunks.end());
            frag.next();
            cQSpec->nextFragment = _buildFragment(queryTemplates, frag);
        } else {
            cQSpec->queries = _buildChunkQueries(queryTemplates, chunkSpec);
            cQSpec->subChunkIds.assign(chunkSpec.subChunks.begin(), chunkSpec.subChunks.end());
        }
    }
    // For a unit test, replace the CHUNK_TAG string with the chunk id number.
    if (fillInChunkIdTag) {
        string chunkIdStr = to_string(chunkSpec.chunkId);
        for (auto&& qs : cQSpec->queries) {
            boost::algorithm::replace_all(qs, CHUNK_TAG, chunkIdStr);
            LOGS(_log, LOG_LVL_DEBUG, "QuerySession::" << __func__ << " " << qs);
        }
    }
    return cQSpec;
}

std::shared_ptr<ChunkQuerySpec> QuerySession::_buildFragment(query::QueryTemplate::Vect const& queryTemplates,
                                                             ChunkSpecFragmenter& f) const {
    std::shared_ptr<ChunkQuerySpec> first;
    std::shared_ptr<ChunkQuerySpec> last;
    while (!f.isDone()) {
        if (last.get()) {
            last->nextFragment = std::make_shared<ChunkQuerySpec>();
            last = last->nextFragment;
        } else {
            last = std::make_shared<ChunkQuerySpec>();
            first = last;
        }
        ChunkSpec s = f.get();
        last->subChunkIds.assign(s.subChunks.begin(), s.subChunks.end());
        last->queries = _buildChunkQueries(queryTemplates, s);
        f.next();
    }
    return first;
}

}  // namespace lsst::qserv::qproc
