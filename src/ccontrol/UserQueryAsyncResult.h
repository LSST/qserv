/*
 * LSST Data Management System
 * Copyright 2017 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */
#ifndef LSST_QSERV_CCONTROL_USERQUERYASYNCRESULT_H
#define LSST_QSERV_CCONTROL_USERQUERYASYNCRESULT_H

// System headers

// Third-party headers

// Qserv headers
#include "ccontrol/UserQuery.h"
#include "qmeta/QInfo.h"
#include "qmeta/types.h"

namespace lsst::qserv::qmeta {
class MessageStore;
}

namespace lsst::qserv::qmeta {
class QMeta;
}

namespace lsst::qserv::sql {
class SqlConnection;
}

namespace lsst::qserv::ccontrol {

/// @addtogroup ccontrol

/**
 *  @ingroup ccontrol
 *
 *  @brief UserQuery implementation for returning results of async queries.
 */

class UserQueryAsyncResult : public UserQuery {
public:
    /**
     *  Constructor for "SELECT * FROM QSERV_RESULT(QID)".
     *
     *  @param queryId:       Query ID for which to return result
     *  @param qMetaCzarId:   ID for current czar
     *  @param qMetaSelect:   QMetaSelect instance
     *  @param resultDbConn:  Connection to results database
     */
    UserQueryAsyncResult(QueryId queryId, qmeta::CzarId qMetaCzarId,
                         std::shared_ptr<qmeta::QMeta> const& qMeta, sql::SqlConnection* resultDbConn);

    // Destructor
    ~UserQueryAsyncResult();

    UserQueryAsyncResult(UserQueryAsyncResult const&) = delete;
    UserQueryAsyncResult& operator=(UserQueryAsyncResult const&) = delete;

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
    std::shared_ptr<qmeta::MessageStore> getMessageStore() override;

    /// This method should disappear when we start supporting results
    /// in locations other than MySQL tables. We'll switch to getResultLocation()
    /// at that point.
    /// @return Name of the result table for this query, can be empty
    std::string getResultTableName() const override;

    /// Result location could be something like "table:table_name" or
    /// "file:/path/to/file.csv".
    /// @return Result location for this query, can be empty
    std::string getResultLocation() const override;

    /// @return get the SELECT statement, to be executed by proxy, from metadata
    std::string getResultQuery() const override;

protected:
private:
    QueryId _queryId;
    qmeta::CzarId _qMetaCzarId;
    std::shared_ptr<qmeta::QMeta> _qMeta;
    sql::SqlConnection* _resultDbConn;
    qmeta::QInfo _qInfo;
    std::shared_ptr<qmeta::MessageStore> _messageStore;
    QueryState _qState = UNKNOWN;
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERYASYNCRESULT_H
