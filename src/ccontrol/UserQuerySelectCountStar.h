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

#ifndef LSST_QSERV_CCONTROL_USERQUERYSELECTCOUNTSTAR_H
#define LSST_QSERV_CCONTROL_USERQUERYSELECTCOUNTSTAR_H

// System headers
#include <memory>

// Third-party headers

// Qserv headers
#include "ccontrol/QueryState.h"
#include "ccontrol/UserQuery.h"
#include "qmeta/QMeta.h"

// Forward decl
namespace lsst::qserv {
namespace qmeta {
class MessageStore;
class QMetaSelect;
}  // namespace qmeta
namespace query {
class SelectStmt;
}
namespace sql {
class SqlConnection;
}
}  // namespace lsst::qserv

namespace lsst::qserv::ccontrol {

/// UserQuerySelectCountStar : for handling queries in the form `SELECT COUNT(*) FROM db.tbl;
class UserQuerySelectCountStar : public UserQuery {
public:
    typedef std::shared_ptr<UserQuerySelectCountStar> Ptr;

    UserQuerySelectCountStar(std::string query, std::shared_ptr<sql::SqlConnection> const& resultDbConn,
                             std::shared_ptr<qmeta::QMetaSelect> const& qMetaSelect,
                             std::shared_ptr<qmeta::QMeta> const& queryMetadata,
                             std::string const& userQueryId, std::string const& rowsTable,
                             std::string const& resultDb, std::string const& countSpelling,
                             qmeta::CzarId czarId, bool async);

    virtual ~UserQuerySelectCountStar() {}

    /// @return a non-empty string describing the current error state
    /// Returns an empty string if no errors have been detected.
    std::string getError() const override { return std::string(); }

    /// Begin execution of the query over all ChunkSpecs added so far.
    void submit() override;

    /// Wait until the query has completed execution.
    /// @return the final execution state.
    QueryState join() override;

    /// Stop a query in progress (for immediate shutdowns)
    void kill() override {}

    /// Release resources related to user query
    void discard() override {}

    // Delegate objects
    std::shared_ptr<qmeta::MessageStore> getMessageStore() override { return _messageStore; }

    /// This method should disappear when we start supporting results
    /// in locations other than MySQL tables. We'll switch to getResultLocation()
    /// at that point.
    /// @return Name of the result table for this query, can be empty
    std::string getResultTableName() const { return _resultTableName; }

    /// Result location could be something like "table:table_name" or
    /// "file:/path/to/file.csv".
    /// @return Result location for this query, can be empty
    std::string getResultLocation() const override { return "table:" + _resultTableName; }

    /// @return True if query is async query
    bool isAsync() const override { return _async; }

    /// @return get the SELECT statement to be executed by proxy
    std::string getResultQuery() const override;

    /// @return this query's QueryId.
    QueryId getQueryId() const override { return _qMetaQueryId; }

    /**
     *  @param resultLocation:  Result location, if empty use result table with unique
     *                          name generated from query ID.
     *  @param msgTableName:  Message table name.
     */
    void qMetaRegister(std::string const& resultLocation, std::string const& msgTableName);

private:
    void _qMetaUpdateStatus(qmeta::QInfo::QStatus qStatus);

    std::shared_ptr<sql::SqlConnection> _resultDbConn;
    std::shared_ptr<qmeta::QMetaSelect> _qMetaSelect;
    std::shared_ptr<qmeta::QMeta> const& _queryMetadata;
    std::shared_ptr<qmeta::MessageStore> _messageStore;
    std::string _resultTableName;
    std::string _userQueryId;
    std::string _rowsTable;
    std::string _resultDb;
    std::string _countSpelling;  // keeps track of how "COUNT" is spelled, for the result query.
    std::string _query;          // The original query text (without SUBMIT if async)
    qmeta::CzarId _qMetaCzarId;
    QueryId _qMetaQueryId;
    bool _async;
    QueryState _qState{UNKNOWN};
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERYSELECTCOUNTSTAR_H
