// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2017 LSST Corporation.
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

#ifndef LSST_QSERV_CCONTROL_USERQUERYPROCESSLIST_H
#define LSST_QSERV_CCONTROL_USERQUERYPROCESSLIST_H

// System headers
#include <memory>
#include <mutex>

// Third-party headers

// Qserv headers
#include "ccontrol/UserQuery.h"
#include "qmeta/QMetaSelect.h"
#include "qmeta/types.h"

// Forward decl
namespace lsst::qserv::qmeta {
class QMeta;
}

namespace lsst::qserv::query {
class SelectStmt;
}

namespace lsst::qserv::sql {
class SqlConnection;
}

namespace lsst::qserv::ccontrol {

/// UserQueryProcessList : implementation of the UserQuery for SHOWPROCESS statements.
class UserQueryProcessList : public UserQuery {
public:
    /**
     *  Constructor for "SELECT ... FROM  INFORMATION_SCHEMA.PROCESSLIST ...".
     *
     *  @param statement:     Parsed SELECT statement
     *  @param resultDbConn:  Connection to results database
     *  @param qMetaSelect:   QMetaSelect instance
     *  @param qMetaCzarId:   Czar ID for QMeta queries
     *  @param userQueryId:   Unique string identifying query
     */
    UserQueryProcessList(std::shared_ptr<query::SelectStmt> const& statement,
                         sql::SqlConnection* resultDbConn,
                         std::shared_ptr<qmeta::QMetaSelect> const& qMetaSelect, qmeta::CzarId qMetaCzarId,
                         std::string const& userQueryId, std::string const& resultDb);

    /**
     *  Constructor for "SHOW [FULL] PROCESSLIST".
     *
     *  @param full:          True if FULL is in query
     *  @param resultDbConn:  Connection to results database
     *  @param qMetaSelect:   QMetaSelect instance
     *  @param qMetaCzarId:   Czar ID for QMeta queries
     *  @param userQueryId:   Unique string identifying query
     */
    UserQueryProcessList(bool full, sql::SqlConnection* resultDbConn,
                         std::shared_ptr<qmeta::QMetaSelect> const& qMetaSelect, qmeta::CzarId qMetaCzarId,
                         std::string const& userQueryId, std::string const& resultDb);

    UserQueryProcessList(UserQueryProcessList const&) = delete;
    UserQueryProcessList& operator=(UserQueryProcessList const&) = delete;

    // Accessors

    /// @return a non-empty string describing the current error state
    /// Returns an empty string if no errors have been detected.
    std::string getError() const override;

    /// Begin execution of the query over all ChunkSpecs added so far.
    void submit() override;

    /// Wait until the query has completed execution.
    /// @return the final execution state.
    QueryState join() override;

    /// Stop a query in progress (for immediate shutdowns)
    void kill() override;

    /// Release resources related to user query
    void discard() override;

    // Delegate objects
    std::shared_ptr<qmeta::MessageStore> getMessageStore() override { return _messageStore; }

    /// @return Name of the result table for this query, can be empty
    std::string getResultTableName() const override { return _resultTableName; }

    /// @return Result location for this query, can be empty
    std::string getResultLocation() const override { return "table:" + _resultTableName; }

    /// @return get the SELECT statement to be executed by proxy
    std::string getResultQuery() const override;

private:
    /// @return ORDER BY part of SELECT statement that gets executed by the proxy
    std::string _getResultOrderBy() const { return _orderBy; }

    sql::SqlConnection* _resultDbConn;
    std::shared_ptr<qmeta::QMetaSelect> _qMetaSelect;
    qmeta::CzarId const _qMetaCzarId;  ///< Czar ID in QMeta database
    QueryState _qState = UNKNOWN;
    std::shared_ptr<qmeta::MessageStore> _messageStore;
    std::string _resultTableName;
    std::string _query;  ///< query to execute on QMeta database
    std::string _orderBy;
    std::string _resultDb;
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERYPROCESSLIST_H
