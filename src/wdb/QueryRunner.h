// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 LSST Corporation.
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

#ifndef LSST_QSERV_WDB_QUERYRUNNER_H
#define LSST_QSERV_WDB_QUERYRUNNER_H
/**
 * @file
 *
 * @brief QueryAction instances perform single-shot query execution with the
 * result reflected in the db state or returned via a SendChannel. Works with
 * new XrdSsi API.
 *
 * @author Daniel L. Wang, SLAC
 */

// System headers
#include <atomic>
#include <memory>

// Qserv headers
#include "mysql/MySqlConfig.h"
#include "mysql/MySqlConnection.h"
#include "qmeta/types.h"
#include "util/MultiError.h"
#include "wbase/Task.h"
#include "wdb/ChunkResource.h"

namespace lsst::qserv::wcontrol {
class SqlConnMgr;
}  // namespace lsst::qserv::wcontrol

namespace lsst::qserv::wpublish {
class QueriesAndChunks;
}  // namespace lsst::qserv::wpublish

namespace lsst::qserv::wdb {

/// On the worker, run a query related to a Task, hold the resources needed to run the query,
/// and write the results to the supplied SendChannel.
///
class QueryRunner : public wbase::TaskQueryRunner, public std::enable_shared_from_this<QueryRunner> {
public:
    using Ptr = std::shared_ptr<QueryRunner>;
    static QueryRunner::Ptr newQueryRunner(
            wbase::Task::Ptr const& task, ChunkResourceMgr::Ptr const& chunkResourceMgr,
            mysql::MySqlConfig const& mySqlConfig, std::shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr,
            std::shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks);
    // Having more than one copy of this would making tracking its progress difficult.
    QueryRunner(QueryRunner const&) = delete;
    QueryRunner& operator=(QueryRunner const&) = delete;
    ~QueryRunner();

    bool runQuery() override;

    /// Cancel the action (in-progress). This should only be called
    /// by Task::cancel(), so if this needs to be cancelled elsewhere,
    /// call Task::cancel().
    /// This should kill an in progress SQL command.
    void cancel() override;

protected:
    QueryRunner(wbase::Task::Ptr const& task, ChunkResourceMgr::Ptr const& chunkResourceMgr,
                mysql::MySqlConfig const& mySqlConfig,
                std::shared_ptr<wcontrol::SqlConnMgr> const& sqlConnMgr,
                std::shared_ptr<wpublish::QueriesAndChunks> const& queriesAndChunks);

private:
    bool _initConnection();
    void _setDb();

    /// Dispatch with output sent through a SendChannel
    bool _dispatchChannel();
    MYSQL_RES* _primeResult(std::string const& query);  ///< Obtain a result handle for a query.

    wbase::Task::Ptr const _task;  ///< Actual task

    qmeta::CzarId _czarId = 0;  ///< To be replaced with the czarId of the requesting czar.

    /// Resource reservation
    ChunkResourceMgr::Ptr _chunkResourceMgr;
    std::string _dbName;
    std::atomic<bool> _cancelled{false};
    std::atomic<bool> _removedFromThreadPool{false};
    mysql::MySqlConfig const _mySqlConfig;
    std::unique_ptr<mysql::MySqlConnection> _mysqlConn;

    util::MultiError _multiError;  // Error log

    /// Used to limit the number of open MySQL connections.
    std::shared_ptr<wcontrol::SqlConnMgr> const _sqlConnMgr;
    std::shared_ptr<wpublish::QueriesAndChunks> const _queriesAndChunks;
};

}  // namespace lsst::qserv::wdb

#endif
