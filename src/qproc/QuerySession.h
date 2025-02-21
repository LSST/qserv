// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2017 LSST Corporation.
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

#ifndef LSST_QSERV_QPROC_QUERYSESSION_H
#define LSST_QSERV_QPROC_QUERYSESSION_H

/**
 * @file
 *
 * @author Daniel L. Wang, SLAC
 */

// System headers
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// Third-party headers
#include "boost/iterator/iterator_facade.hpp"

// Qserv headers
#include "css/CssAccess.h"
#include "global/intTypes.h"
#include "qana/QueryPlugin.h"
#include "qproc/ChunkQuerySpec.h"
#include "qproc/ChunkSpec.h"
#include "qproc/DatabaseModels.h"
#include "query/QueryTemplate.h"
#include "query/typedefs.h"
#include "sql/SqlConfig.h"

// Forward declarations
namespace lsst::qserv {
namespace css {
class StripingParams;
}
namespace query {
class SelectStmt;
class QueryContext;
}  // namespace query
}  // namespace lsst::qserv

namespace lsst::qserv::qproc {

///  QuerySession contains state and behavior for operating on user queries. It
///  contains much of the query analysis-side responsibility, including the text
///  of the original query, a parsed query  tree, and other user state/context.
class QuerySession {
public:
    typedef std::shared_ptr<QuerySession> Ptr;

    // null constructor should only be used by parser unit tests.
    QuerySession();

    QuerySession(std::shared_ptr<css::CssAccess> const& css, qproc::DatabaseModels::Ptr const& dbModels,
                 std::string const& defaultDb, int const interactiveChunkLimit)
            : _css(css),
              _defaultDb(defaultDb),
              _databaseModels(dbModels),
              _interactiveChunkLimit(interactiveChunkLimit) {}

    std::shared_ptr<query::SelectStmt> parseQuery(std::string const& statement);

    std::string const& getOriginal() const { return _original; }

    /**
     * @brief Analyze SQL query using parsed query
     *
     * @param sql: the sql query text
     * @param stmt: parsed select statement
     */
    void analyzeQuery(std::string const& sql, std::shared_ptr<query::SelectStmt> const& stmt);

    bool needsMerge() const;
    bool hasChunks() const;

    /**
     * @brief Get the Area Restrictors
     */
    query::AreaRestrictorVecPtr getAreaRestrictors() const;

    /**
     * @brief Get the Secondary Index Restrictors
     */
    query::SecIdxRestrictorVecPtr getSecIdxRestrictors() const;

    void addChunk(ChunkSpec const& cs);
    void setDummy();

    query::SelectStmt const& getStmt() const { return *_stmt; }

    query::SelectStmtPtrVector const& getStmtParallel() const { return _stmtParallel; }

    /**
     * @brief Get the SelectStmt that will be executed on workers, without chunking annotations.
     *
     * @return std::shared_ptr<query::SelectStmt> const&
     */
    std::shared_ptr<query::SelectStmt> const& getPreFlightStmt() const { return _stmtPreFlight; }

    /** @brief Return the ORDER BY clause to be used in the result query statement.
     *
     *  Indeed, MySQL results order is undefined with simple "SELECT *" clause.
     *  This parameter is set during query analysis.
     *
     *  @return: a string containing a SQL "ORDER BY" clause, or an empty string if this clause doesn't exists
     *  @see QuerySession::analyzeQuery()
     */
    std::string getResultOrderBy() const;

    /// Dominant database is the database that will be used for query
    /// dispatch. This is distinct from the default database, which is what is
    /// used for unqualified table and column references
    std::string const& getDominantDb() const;
    bool containsDb(std::string const& dbName) const;
    bool containsTable(std::string const& dbName, std::string const& tableName) const;
    bool validateDominantDb() const;
    css::StripingParams getDbStriping();
    std::shared_ptr<IntSet const> getEmptyChunks();
    std::string const& getError() const { return _error; }

    std::shared_ptr<query::SelectStmt> getMergeStmt() const;

    /// Build the template for the worker queries.
    /// @param fillinChunIdTag - When true replace the CHUNK_TAG string with the chunk id number.
    ///         Template strings sent to the worker should not fill in the tag, but unit tests
    ///         need it filled in.
    ChunkQuerySpec::Ptr buildChunkQuerySpec(query::QueryTemplate::Vect const& queryTemplates,
                                            ChunkSpec const& chunkSpec, bool fillInChunkIdTag = true) const;

    /// Finalize a query after chunk coverage has been updated
    void finalize();
    // Iteration
    ChunkSpecVector::iterator cQueryBegin() { return _chunks.begin(); }
    ChunkSpecVector::iterator cQueryEnd() { return _chunks.end(); }
    int getChunksSize() const { return _chunks.size(); }

    // For test harnesses.
    struct Test {
        Test() : cfgNum(0), defaultDb("LSST"), sqlConfig(sql::SqlConfig(sql::SqlConfig::MOCK)) {}
        Test(int cfgNum_, std::shared_ptr<css::CssAccess> css_, std::string defaultDb_,
             sql::SqlConfig const& sqlConfig_)
                : cfgNum(cfgNum_), css(css_), defaultDb(defaultDb_), sqlConfig(sqlConfig_) {}
        int cfgNum;
        std::shared_ptr<css::CssAccess> css;
        std::string defaultDb;
        sql::SqlConfig sqlConfig;
    };
    explicit QuerySession(Test& t);  ///< Debug constructor
    std::shared_ptr<query::QueryContext> dbgGetContext() { return _context; }

    query::QueryTemplate::Vect makeQueryTemplates();

    void setScanInteractive();
    bool getScanInteractive() const { return _scanInteractive; }

    protojson::ScanInfo::Ptr getScanInfo() const;

    /**
     *  Print query session to stream.
     *
     *  Used for debugging
     *
     *  @params out:    stream to update
     */
    void print(std::ostream& out) const;

private:
    typedef std::vector<qana::QueryPlugin::Ptr> QueryPluginPtrVector;

    // Pipeline helpers
    void _initContext();
    void _preparePlugins();
    void _applyLogicPlugins();
    void _generateConcrete();
    void _applyConcretePlugins();

    std::vector<std::string> _buildChunkQueries(query::QueryTemplate::Vect const& queryTemplates,
                                                ChunkSpec const& chunkSpec) const;
    std::shared_ptr<ChunkQuerySpec> _buildFragment(query::QueryTemplate::Vect const& queryTemplates,
                                                   ChunkSpecFragmenter& f) const;

    // Fields
    std::shared_ptr<css::CssAccess> _css;                    ///< Metadata access
    std::string _defaultDb;                                  ///< User db context
    std::string _original;                                   ///< Original user query
    std::shared_ptr<qproc::DatabaseModels> _databaseModels;  ///< Source of schema information
    std::shared_ptr<query::QueryContext> _context;           ///< Analysis context
    std::shared_ptr<query::SelectStmt> _stmt;                ///< Logical query statement

    /// Group of parallel statements (not a sequence)
    /**
     * Store the template used to generate queries on the workers
     * Example:
     *    - input user query:
     *    select sum(pm_declErr), chunkId as f1, chunkId AS f1, avg(pm_declErr)
     *    from LSST.Object where bMagF > 20.0 GROUP BY chunkId;
     *    - template for worker queries:
     *    SELECT sum(pm_declErr) AS QS1_SUM,chunkId AS f1,chunkId AS f1,
     *    COUNT(pm_declErr) AS QS2_COUNT,SUM(pm_declErr) AS QS3_SUM
     *    FROM LSST.Object_%CC% AS QST_1_ WHERE bMagF>20.0 GROUP BY chunkId
     *
     */
    query::SelectStmtPtrVector _stmtParallel;

    /**
     * Store the query used on local workers, but it does not get modified for chunking the way the worker
     * query templates do (with e.g. `%CC%`). This is used to run the "preflight" query on the local copy of
     * the data table, to get the schema used to create the results table.
     */
    query::SelectStmtPtr _stmtPreFlight;

    /**
     * Store the query used to aggregate results on the czar.
     * Aggregation is optional, so this variable may be empty
     * It will run against a table named: result_ID_m, where ID is an integer
     * Example:
     *    - input user query:
     *    select sum(pm_declErr), chunkId as f1, chunkId AS f1, avg(pm_declErr)
     *    from LSST.Object where bMagF > 20.0 GROUP BY chunkId;
     *    - merge query:
     *    SELECT SUM(QS1_SUM),f1 AS f1,f1 AS f1,(SUM(QS3_SUM)/SUM(QS2_COUNT))
     *    GROUP BY chunkId
     *
     */
    query::SelectStmtPtr _stmtMerge;

    bool _hasMerge = false;
    bool _isDummy = false;  ///< Use dummy chunk, disabling subchunks or any real chunks
    std::string _tmpTable;
    std::string _resultTable;
    std::string _error;
    int _isFinal = 0;  ///< Has query analysis/optimization completed?

    ChunkSpecVector _chunks;                         ///< Chunk coverage
    std::shared_ptr<QueryPluginPtrVector> _plugins;  ///< Analysis plugin chain

    /// Maximum number of chunks in an interactive query.
    int const _interactiveChunkLimit = 10;  // Value of 10 only used in unit tests.
    bool _scanInteractive = true;           ///< True if the query can be considered interactive.
};

/**
 *  Output operator for QuerySession
 *
 *  @param out
 *  @param querySession
 *  @return an output stream, with no newline at the end
 */
std::ostream& operator<<(std::ostream& out, QuerySession const& querySession);

}  // namespace lsst::qserv::qproc

#endif  // LSST_QSERV_QPROC_QUERYSESSION_H
