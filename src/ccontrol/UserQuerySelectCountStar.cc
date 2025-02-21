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

#include "UserQuerySelectCountStar.h"

#include "boost/lexical_cast.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/UserQueryError.h"
#include "ccontrol/UserQueryType.h"
#include "qmeta/MessageStore.h"
#include "qmeta/QInfo.h"
#include "qmeta/QMetaSelect.h"
#include "query/SelectStmt.h"
#include "sql/SqlConnection.h"
#include "sql/SqlResults.h"

namespace {

std::string g_nextResultTableId(std::string const& userQueryId) {
    return "qserv_result_countstar_" + userQueryId;
}

LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQuerySelectCountStar");

}  // end namespace

using boost::bad_lexical_cast;
using boost::lexical_cast;

namespace lsst::qserv::ccontrol {

UserQuerySelectCountStar::UserQuerySelectCountStar(std::string query,
                                                   std::shared_ptr<sql::SqlConnection> const& resultDbConn,
                                                   std::shared_ptr<qmeta::QMetaSelect> const& qMetaSelect,
                                                   std::shared_ptr<qmeta::QMeta> const& queryMetadata,
                                                   std::string const& userQueryId,
                                                   std::string const& rowsTable, std::string const& resultDb,
                                                   std::string const& countSpelling, qmeta::CzarId czarId,
                                                   bool async)
        : _resultDbConn(resultDbConn),
          _qMetaSelect(qMetaSelect),
          _queryMetadata(queryMetadata),
          _messageStore(std::make_shared<qmeta::MessageStore>()),
          _resultTableName(::g_nextResultTableId(userQueryId)),
          _userQueryId(userQueryId),
          _rowsTable(rowsTable),
          _resultDb(resultDb),
          _countSpelling(countSpelling),
          _query(query),
          _qMetaCzarId(czarId),
          _async(async) {}

// Submit or execute the query.
void UserQuerySelectCountStar::submit() {
    // Query the database:
    std::string query = "SELECT num_rows from " + _rowsTable;
    std::unique_ptr<sql::SqlResults> results;
    try {
        results = _qMetaSelect->select(query);
    } catch (std::exception const& exc) {
        LOGS(_log, LOG_LVL_ERROR, "Failed while querying QMeta: " << exc.what());
        _messageStore->addMessage(-1, "COUNTSTAR", 1051, "Internal error querying metadata.",
                                  MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }

    // Get the one column ("num_rows") from results:
    std::vector<std::string> values;
    sql::SqlErrorObject errObj;
    if (not results->extractFirstColumn(values, errObj)) {
        LOGS(_log, LOG_LVL_ERROR,
             "Failed to extract chunk row counts from query result: " << errObj.errMsg());
        _messageStore->addMessage(-1, "COUNTSTAR", 1051, "Internal error extracting chunk row counts.",
                                  MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }

    // Convert the column values to int and accumuate in row_count:
    uint64_t row_count(0);
    for (auto const& value : values) {
        try {
            auto add_rows = lexical_cast<uint64_t>(value);
            if (UINT64_MAX - row_count < add_rows) {
                LOGS(_log, LOG_LVL_ERROR, "The number of rows exceeded capacity.");
                _messageStore->addMessage(-1, "COUNTSTAR", 1051, "The number of rows exceeded capacity.",
                                          MessageSeverity::MSG_ERROR);
                _qState = ERROR;
                return;
            }
            row_count += add_rows;
        } catch (bad_lexical_cast const& exc) {
            LOGS(_log, LOG_LVL_ERROR,
                 "Failed to convert chunk row count \"" << value << "\" to unsigned int: " << exc.what(););
            _messageStore->addMessage(-1, "COUNTSTAR", 1051,
                                      "Internal error converting chunk row count to unsigned int.",
                                      MessageSeverity::MSG_ERROR);
            _qState = ERROR;
            return;
        }
    }

    // Create a result table, with one column (row_count) and one row (the total number of rows):
    std::string createTable = "CREATE TABLE " + _resultTableName + "(row_count BIGINT UNSIGNED)";
    LOGS(_log, LOG_LVL_DEBUG, "creating result table: " << createTable);
    if (!_resultDbConn->runQuery(createTable, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to create result table: " << errObj.errMsg());
        _messageStore->addMessage(-1, "COUNTSTAR", 1051, "Internal error, failed to create result table.",
                                  MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }

    // Insert the total row count into the result table:
    std::string insertRow = "INSERT INTO " + _resultTableName + " VALUES (";
    try {
        insertRow += lexical_cast<std::string>(row_count);
    } catch (bad_lexical_cast const& exc) {
        LOGS(_log, LOG_LVL_ERROR,
             "Failed to convert the row count \"" << row_count << "\" to string: " << exc.what());
        _messageStore->addMessage(-1, "COUNTSTAR", 1051,
                                  "Internal error converting total row count to string.",
                                  MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }
    insertRow += ")";
    LOGS(_log, LOG_LVL_DEBUG, "inserting row count into result table: " << insertRow);
    if (!_resultDbConn->runQuery(insertRow, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to insert row count into result table: " << errObj.errMsg());
        _messageStore->addMessage(-1, "COUNTSTAR", 1051,
                                  "Internal failure, failed to insert the row count into the result table.",
                                  MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }

    _qState = SUCCESS;
}

std::string UserQuerySelectCountStar::getResultQuery() const {
    return "SELECT row_count as '" + _countSpelling + "(*)' FROM " + _resultDb + "." + getResultTableName();
}

void UserQuerySelectCountStar::qMetaRegister(std::string const& resultLocation,
                                             std::string const& msgTableName) {
    qmeta::QInfo::QType qType = _async ? qmeta::QInfo::ASYNC : qmeta::QInfo::SYNC;
    std::string user = "anonymous";  // we do not have access to that info yet
    std::string qTemplate = "template";
    std::string qMerge = "merge";
    int chunkCount = 0;
    qmeta::QInfo qInfo(qType, _qMetaCzarId, user, _query, qTemplate, qMerge, getResultLocation(),
                       msgTableName, getResultQuery(), chunkCount);
    qmeta::QMeta::TableNames tableNames;
    _qMetaQueryId = _queryMetadata->registerQuery(qInfo, tableNames);
}

QueryState UserQuerySelectCountStar::join() {
    // bytes and rows collected from workers should be 0.
    _queryMetadata->completeQuery(_qMetaQueryId,
                                  _qState == SUCCESS ? qmeta::QInfo::COMPLETED : qmeta::QInfo::FAILED);
    return _qState;
}

}  // namespace lsst::qserv::ccontrol
