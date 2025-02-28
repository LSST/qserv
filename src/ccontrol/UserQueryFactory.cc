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

// Class header
#include "ccontrol/UserQueryFactory.h"

// System headers
#include <cassert>
#include <cstdlib>
#include <memory>
#include <string>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "ccontrol/ConfigError.h"
#include "ccontrol/ConfigMap.h"
#include "ccontrol/ParseRunner.h"
#include "ccontrol/UserQueryAsyncResult.h"
#include "ccontrol/UserQueryDrop.h"
#include "ccontrol/UserQueryFlushChunksCache.h"
#include "ccontrol/UserQueryInvalid.h"
#include "ccontrol/UserQueryProcessList.h"
#include "ccontrol/UserQueryQueries.h"
#include "ccontrol/UserQueryResources.h"
#include "ccontrol/UserQuerySelect.h"
#include "ccontrol/UserQuerySelectCountStar.h"
#include "ccontrol/UserQuerySet.h"
#include "ccontrol/UserQueryType.h"
#include "css/CssAccess.h"
#include "css/KvInterfaceImplMem.h"
#include "czar/Czar.h"
#include "mysql/MySqlConfig.h"
#include "parser/ParseException.h"
#include "qdisp/Executive.h"
#include "qmeta/MessageStore.h"
#include "qmeta/QMetaMysql.h"
#include "qmeta/QMetaSelect.h"
#include "qmeta/QStatusMysql.h"
#include "qproc/DatabaseModels.h"
#include "qproc/QuerySession.h"
#include "qproc/SecondaryIndex.h"
#include "query/FromList.h"
#include "query/SelectStmt.h"
#include "rproc/InfileMerger.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"
#include "util/QdispPool.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQueryFactory");
}

namespace lsst::qserv::ccontrol {

using userQuerySharedResourcesPtr = std::shared_ptr<UserQuerySharedResources>;

/**
 * @brief Determine if the table name in the FROM statement refers to PROCESSLIST table.
 *
 * @param stmt SelectStmt representing the query.
 * @param defaultDb Default database name, may be empty.
 * @return true if the query refers only to the PROCESSLIST table.
 * @return false if the query does not refer only to the PROCESSLIST table.
 */
bool _stmtRefersToProcessListTable(query::SelectStmt::Ptr& stmt, std::string defaultDb) {
    auto const& tableRefList = stmt->getFromList().getTableRefList();
    if (tableRefList.size() != 1) return false;
    auto const& tblRef = tableRefList[0];
    std::string const& db = tblRef->getDb().empty() ? defaultDb : tblRef->getDb();
    if (UserQueryType::isProcessListTable(db, tblRef->getTable())) return true;
    return false;
}

/**
 * @brief Determine if the table name in the FROM statement refers to QUERIES table.
 *
 * @param stmt SelectStmt representing the query.
 * @param defaultDb Default database name, may be empty.
 * @return true if the query refers only to the QUERIES table.
 * @return false if the query does not refer only to the QUERIES table.
 */
bool _stmtRefersQueriesTable(query::SelectStmt::Ptr& stmt, std::string defaultDb) {
    auto const& tableRefList = stmt->getFromList().getTableRefList();
    if (tableRefList.size() != 1) return false;
    auto const& tblRef = tableRefList[0];
    std::string const& db = tblRef->getDb().empty() ? defaultDb : tblRef->getDb();
    if (UserQueryType::isQueriesTable(db, tblRef->getTable())) return true;
    return false;
}

/**
 * @brief Make a UserQueryProcessList (or UserQueryInvalid) from given parameters.
 *
 * @param stmt The SelectStmt representing the query.
 * @param sharedResources Resources used by UserQueryFactory to create UserQueries.
 * @param userQueryId Unique string identifying the query.
 * @param resultDb Name of the databse that will contain results.
 * @param aQuery The original query string.
 * @param async If the query is to be run asynchronously.
 * @return std::shared_ptr<UserQuery>, will be a UserQueryProcessList or UserQueryInvalid.
 */
std::shared_ptr<UserQuery> _makeUserQueryProcessList(query::SelectStmt::Ptr& stmt,
                                                     userQuerySharedResourcesPtr& sharedResources,
                                                     std::string const& userQueryId,
                                                     std::string const& resultDb, std::string const& aQuery,
                                                     bool async) {
    if (async) {
        // no point supporting async for these
        return std::make_shared<UserQueryInvalid>("SUBMIT is not allowed with query: " + aQuery);
    }
    LOGS(_log, LOG_LVL_DEBUG, "SELECT query is a PROCESSLIST");
    try {
        return std::make_shared<UserQueryProcessList>(stmt, sharedResources->resultDbConn.get(),
                                                      sharedResources->qMetaSelect,
                                                      sharedResources->qMetaCzarId, userQueryId, resultDb);
    } catch (std::exception const& exc) {
        return std::make_shared<UserQueryInvalid>(exc.what());
    }
}

/**
 * @brief Make a UserQueryQueries (or UserQueryInvalid) from given parameters.
 *
 * @param stmt The SelectStmt representing the query.
 * @param sharedResources Resources used by UserQueryFactory to create UserQueries.
 * @param userQueryId Unique string identifying the query.
 * @param resultDb Name of the databse that will contain results.
 * @param aQuery The original query string.
 * @param async If the query is to be run asynchronously.
 * @return std::shared_ptr<UserQuery>, will be a UserQueryQueries or UserQueryInvalid.
 */
std::shared_ptr<UserQuery> _makeUserQueryQueries(query::SelectStmt::Ptr& stmt,
                                                 userQuerySharedResourcesPtr& sharedResources,
                                                 std::string const& userQueryId, std::string const& resultDb,
                                                 std::string const& aQuery, bool async) {
    if (async) {
        // no point supporting async for these
        return std::make_shared<UserQueryInvalid>("SUBMIT is not allowed with query: " + aQuery);
    }
    LOGS(_log, LOG_LVL_DEBUG, "SELECT query is a QUERIES");
    try {
        return std::make_shared<UserQueryQueries>(stmt, sharedResources->resultDbConn.get(),
                                                  sharedResources->qMetaSelect, sharedResources->qMetaCzarId,
                                                  userQueryId, resultDb);
    } catch (std::exception const& exc) {
        return std::make_shared<UserQueryInvalid>(exc.what());
    }
}

/**
 * @brief Determine if the qmeta database has a metadata table with chunks & row
 *        counts that represents the table in the FROM statement for a SELECT
 *        COUNT(*) query.
 *
 * @param stmt The SelectStmt representing the query.
 * @param sharedResources Resources used by UserQueryFactory to create UserQueries.
 * @param defaultDb Default database name, may be empty.
 * @param rowsTable Output variable, will be set to the name of the rows table if it
 *                  exists, otherwise will be set to an empty string.
 * @return true if the qmeta table containing the row counts is present in qmeta.
 * @return false if the table is not present in qmeta.
 */
bool qmetaHasDataForSelectCountStarQuery(query::SelectStmt::Ptr const& stmt,
                                         userQuerySharedResourcesPtr& sharedResources,
                                         std::string const& defaultDb, std::string& rowsTable) {
    auto const& tableRefList = stmt->getFromList().getTableRefList();
    // by definition a simple COUNT(*) should have exactly one table ref.
    assert(tableRefList.size() > 0);
    auto const& tableRefPtr = tableRefList[0];
    assert(tableRefPtr != nullptr);
    auto fromDb = tableRefPtr->getDb();
    if (fromDb.empty()) {
        fromDb = defaultDb;
    }
    auto const& fromTable = tableRefPtr->getTable();
    rowsTable = fromDb + "__" + fromTable + "__rows";
    // TODO consider using QMetaSelect instead of making a new connection.
    auto cnx = sql::SqlConnectionFactory::make(cconfig::CzarConfig::instance()->getMySqlQmetaConfig());
    sql::SqlErrorObject err;
    auto tableExists = cnx->tableExists(rowsTable, err);
    LOGS(_log, LOG_LVL_DEBUG,
         *stmt << " rows table: " << rowsTable << (tableExists ? " exists" : " does not exist"));
    if (not tableExists) rowsTable = "";
    return tableExists;
}

std::shared_ptr<UserQuerySharedResources> makeUserQuerySharedResources(
        std::shared_ptr<qproc::DatabaseModels> const& dbModels, std::string const& czarName) {
    auto const czarConfig = cconfig::CzarConfig::instance();
    return std::make_shared<UserQuerySharedResources>(
            css::CssAccess::createFromConfig(czarConfig->getCssConfigMap()),
            czarConfig->getMySqlResultConfig(),
            std::make_shared<qproc::SecondaryIndex>(czarConfig->getMySqlQmetaConfig()),
            std::make_shared<qmeta::QMetaMysql>(czarConfig->getMySqlQmetaConfig(),
                                                czarConfig->getMaxMsgSourceStore()),
            std::make_shared<qmeta::QStatusMysql>(czarConfig->getMySqlQStatusDataConfig()),
            std::make_shared<qmeta::QMetaSelect>(czarConfig->getMySqlQmetaConfig()),
            sql::SqlConnectionFactory::make(czarConfig->getMySqlResultConfig()), dbModels, czarName,
            czarConfig->getInteractiveChunkLimit());
}

////////////////////////////////////////////////////////////////////////
UserQueryFactory::UserQueryFactory(qproc::DatabaseModels::Ptr const& dbModels, std::string const& czarName)
        : _userQuerySharedResources(makeUserQuerySharedResources(dbModels, czarName)),
          _qmetaSecondsBetweenUpdates(cconfig::CzarConfig::instance()->getQMetaSecondsBetweenChunkUpdates()),
          _useQservRowCounterOptimization(true),
          _asioIoService() {
    // When czar crashes/exits while some queries are still in flight they
    // are left in EXECUTING state in QMeta. We want to cleanup that state
    // to avoid confusion. Note that when/if clean czar restart is implemented
    // we'll need a new logic to restart query processing.
    _userQuerySharedResources->queryMetadata->cleanup(_userQuerySharedResources->qMetaCzarId);

    // Add logging context with czar ID
    qmeta::CzarId qMetaCzarId = _userQuerySharedResources->qMetaCzarId;
    LOG_MDC_INIT([qMetaCzarId]() { LOG_MDC("CZID", std::to_string(qMetaCzarId)); });

    // BOOST ASIO service is started to process asynchronous timer requests
    // in the dedicated thread. However, before starting the thread we need
    // to attach the ASIO's "work" object to the ASIO I/O service. This is needed
    // to keep the latter busy and prevent the servicing thread from exiting before
    // the destruction of this class  due to a lack of async requests.
    _asioWork.reset(new boost::asio::io_service::work(_asioIoService));

    // Start the timer servicing thread
    _asioTimerThread.reset(new std::thread([&]() { _asioIoService.run(); }));
}

UserQueryFactory::~UserQueryFactory() {
    // Shut down all ongoing (if any) operations on the I/O service
    // to unblock the servicing thread.
    _asioWork.reset();
    _asioIoService.stop();
    _asioTimerThread->join();
}

UserQuery::Ptr UserQueryFactory::newUserQuery(std::string const& aQuery, std::string const& defaultDb,
                                              util::QdispPool::Ptr const& qdispPool,
                                              std::string const& userQueryId, std::string const& msgTableName,
                                              std::string const& resultDb) {
    // result location could potentially be specified by SUBMIT command, for now
    // we keep it empty which means that UserQuerySelect uses default result table.
    std::string resultLocation;

    // First check for SUBMIT and strip it
    std::string query = aQuery;

    std::string stripped;
    bool async = false;
    if (UserQueryType::isSubmit(query, stripped)) {
        // SUBMIT is only allowed with SELECT for now, complain if anything else is there
        if (!UserQueryType::isSelect(stripped)) {
            return std::make_shared<UserQueryInvalid>("SUBMIT only valid with SELECT queries: " + query);
        }
        async = true;
        query = stripped;
    }

    std::string dbName, tableName;
    bool full = false;
    QueryId userJobId = 0;

    if (UserQueryType::isSelect(query)) {
        // Processing regular select query
        bool sessionValid = true;
        std::string errorExtra;

        // Parse SELECT

        ParseRunner::Ptr parser;
        try {
            parser = std::make_shared<ParseRunner>(query);
        } catch (parser::ParseException& e) {
            return std::make_shared<UserQueryInvalid>(std::string("ParseException:") + e.what());
        }
        auto stmt = parser->getSelectStmt();

        // handle special database/table names
        if (_stmtRefersToProcessListTable(stmt, defaultDb)) {
            return _makeUserQueryProcessList(stmt, _userQuerySharedResources, userQueryId, resultDb, aQuery,
                                             async);
        }
        if (_stmtRefersQueriesTable(stmt, defaultDb)) {
            return _makeUserQueryQueries(stmt, _userQuerySharedResources, userQueryId, resultDb, aQuery,
                                         async);
        }

        /// Determine if a SelectStmt is a simple COUNT(*) query and can be run as an optimized query.
        /// It may not be runnable as an optimzed simple COUNT(*) query because:
        /// * The queryMeta tables do not have the required information.
        /// * The option to run optimized COUNT(*) queries is turned off.
        /// * It is not a COUNT(*) query.
        /// * It is a COUNT(*) query but is too complex for the simple optimization.
        std::string rowsTable;
        std::string countSpelling;
        LOGS(_log, LOG_LVL_DEBUG,
             "UseQservRowCounterOptimization: is " << (_useQservRowCounterOptimization ? "on" : "off")
                                                   << ".");
        if (_useQservRowCounterOptimization && UserQueryType::isSimpleCountStar(stmt, countSpelling) &&
            qmetaHasDataForSelectCountStarQuery(stmt, _userQuerySharedResources, defaultDb, rowsTable)) {
            LOGS(_log, LOG_LVL_DEBUG, "make UserQuerySelectCountStar");
            auto uq = std::make_shared<UserQuerySelectCountStar>(
                    query, _userQuerySharedResources->resultDbConn, _userQuerySharedResources->qMetaSelect,
                    _userQuerySharedResources->queryMetadata, userQueryId, rowsTable, resultDb, countSpelling,
                    _userQuerySharedResources->qMetaCzarId, async);
            uq->qMetaRegister(resultLocation, msgTableName);
            return uq;
        }

        // This is a regular SELECT for qserv

        // Currently using the database for results to get schema information.
        auto qs = std::make_shared<qproc::QuerySession>(_userQuerySharedResources->css,
                                                        _userQuerySharedResources->databaseModels, defaultDb,
                                                        _userQuerySharedResources->interactiveChunkLimit);
        try {
            qs->analyzeQuery(query, stmt);
        } catch (...) {
            errorExtra = "Unknown failure occurred setting up QuerySession (query is invalid).";
            LOGS(_log, LOG_LVL_ERROR, errorExtra);
            sessionValid = false;
        }
        if (!qs->getError().empty()) {
            // The `qs` object is passed to the UserQuerySelect object below,
            // so the errors do not need to be added to `errorExtra`.
            LOGS(_log, LOG_LVL_ERROR, "Invalid query: " << qs->getError());
            sessionValid = false;
        }

        auto messageStore = std::make_shared<qmeta::MessageStore>();
        std::shared_ptr<qdisp::Executive> executive;
        std::shared_ptr<rproc::InfileMergerConfig> infileMergerConfig;
        if (sessionValid) {
            executive =
                    qdisp::Executive::create(_qmetaSecondsBetweenUpdates, messageStore, qdispPool,
                                             _userQuerySharedResources->queryStatsData, qs, _asioIoService);
            infileMergerConfig =
                    std::make_shared<rproc::InfileMergerConfig>(_userQuerySharedResources->mysqlResultConfig);
            infileMergerConfig->debugNoMerge = _debugNoMerge;
        }

        auto czarConfig = cconfig::CzarConfig::instance();
        int uberJobMaxChunks = czarConfig->getUberJobMaxChunks();

        // This, effectively invalid, UserQuerySelect object should report errors from both `errorExtra`
        // and errors that the QuerySession `qs` has stored internally.
        auto uq = std::make_shared<UserQuerySelect>(
                qs, messageStore, executive, _userQuerySharedResources->databaseModels, infileMergerConfig,
                _userQuerySharedResources->secondaryIndex, _userQuerySharedResources->queryMetadata,
                _userQuerySharedResources->queryStatsData, _userQuerySharedResources->semaMgrConnections,
                _userQuerySharedResources->qMetaCzarId, errorExtra, async, resultDb, uberJobMaxChunks);
        if (sessionValid) {
            uq->qMetaRegister(resultLocation, msgTableName);
            uq->setupMerger();
            uq->saveResultQuery();
            executive->setUserQuerySelect(uq);
        }
        return uq;
    } else if (UserQueryType::isSelectResult(query, userJobId)) {
        auto uq = std::make_shared<UserQueryAsyncResult>(userJobId, _userQuerySharedResources->qMetaCzarId,
                                                         _userQuerySharedResources->queryMetadata,
                                                         _userQuerySharedResources->resultDbConn.get());
        LOGS(_log, LOG_LVL_DEBUG, "make UserQueryAsyncResult: userJobId=" << userJobId);
        return uq;
    } else if (UserQueryType::isDropTable(query, dbName, tableName)) {
        // processing DROP TABLE
        if (dbName.empty()) {
            dbName = defaultDb;
        }
        auto uq = std::make_shared<UserQueryDrop>(_userQuerySharedResources->css, dbName, tableName,
                                                  _userQuerySharedResources->resultDbConn.get(),
                                                  _userQuerySharedResources->queryMetadata,
                                                  _userQuerySharedResources->qMetaCzarId);
        LOGS(_log, LOG_LVL_DEBUG, "make UserQueryDrop: " << dbName << "." << tableName);
        return uq;
    } else if (UserQueryType::isDropDb(query, dbName)) {
        // processing DROP DATABASE
        auto uq = std::make_shared<UserQueryDrop>(_userQuerySharedResources->css, dbName, std::string(),
                                                  _userQuerySharedResources->resultDbConn.get(),
                                                  _userQuerySharedResources->queryMetadata,
                                                  _userQuerySharedResources->qMetaCzarId);
        LOGS(_log, LOG_LVL_DEBUG, "make UserQueryDrop: db=" << dbName);
        return uq;
    } else if (UserQueryType::isFlushChunksCache(query, dbName)) {
        auto uq = std::make_shared<UserQueryFlushChunksCache>(_userQuerySharedResources->css, dbName,
                                                              _userQuerySharedResources->resultDbConn.get());
        LOGS(_log, LOG_LVL_DEBUG, "make UserQueryFlushChunksCache: " << dbName);
        return uq;
    } else if (UserQueryType::isShowProcessList(query, full)) {
        LOGS(_log, LOG_LVL_DEBUG, "make UserQueryProcessList: full=" << (full ? 'y' : 'n'));
        try {
            return std::make_shared<UserQueryProcessList>(full, _userQuerySharedResources->resultDbConn.get(),
                                                          _userQuerySharedResources->qMetaSelect,
                                                          _userQuerySharedResources->qMetaCzarId, userQueryId,
                                                          resultDb);
        } catch (std::exception const& exc) {
            return std::make_shared<UserQueryInvalid>(exc.what());
        }
    } else if (UserQueryType::isCall(query)) {
        auto parser = std::make_shared<ParseRunner>(
                query, _userQuerySharedResources->makeUserQueryResources(userQueryId, resultDb));
        return parser->getUserQuery();
    } else if (UserQueryType::isSet(query)) {
        ParseRunner::Ptr parser;
        try {
            parser = std::make_shared<ParseRunner>(query);
        } catch (parser::ParseException& e) {
            return std::make_shared<UserQueryInvalid>(std::string("ParseException:") + e.what());
        }
        auto uq = parser->getUserQuery();
        auto setQuery = std::static_pointer_cast<UserQuerySet>(uq);
        if (setQuery->varName() == "QSERV_ROW_COUNTER_OPTIMIZATION") {
            _useQservRowCounterOptimization = setQuery->varValue() != "0";
            LOGS(_log, LOG_LVL_WARN,
                 "QSERV_ROW_COUNTER_OPTIMIZATION=" << (_useQservRowCounterOptimization ? "1" : "0"));
        } else if (setQuery->varName() == "QSERV_DEBUG_CZAR_NO_MERGE") {
            _debugNoMerge = setQuery->varValue() != "0";
            LOGS(_log, LOG_LVL_WARN, "QSERV_DEBUG_CZAR_NO_MERGE=" << (_debugNoMerge ? "1" : "0"));
        }
        return uq;
    } else {
        // something that we don't recognize
        auto uq = std::make_shared<UserQueryInvalid>("Invalid or unsupported query: " + query);
        return uq;
    }
}

}  // namespace lsst::qserv::ccontrol
