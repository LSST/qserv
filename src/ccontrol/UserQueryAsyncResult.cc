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

// Class header
#include "ccontrol/UserQueryAsyncResult.h"

// System headers

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "qmeta/Exceptions.h"
#include "qmeta/JobStatus.h"
#include "qmeta/QMeta.h"
#include "qmeta/MessageStore.h"
#include "sql/SqlConnection.h"
#include "sql/SqlResults.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQueryAsyncResult");
}

namespace lsst::qserv::ccontrol {

// Constructors
UserQueryAsyncResult::UserQueryAsyncResult(QueryId queryId, qmeta::CzarId qMetaCzarId,
                                           std::shared_ptr<qmeta::QMeta> const& qMeta,
                                           sql::SqlConnection* resultDbConn)
        : UserQuery(),
          _queryId(queryId),
          _qMetaCzarId(qMetaCzarId),
          _qMeta(qMeta),
          _resultDbConn(resultDbConn),
          _messageStore(std::make_shared<qmeta::MessageStore>()) {
    LOGS(_log, LOG_LVL_DEBUG, "UserQueryAsyncResult: QID=" << queryId);

    // get query info from QMeta
    try {
        _qInfo = qMeta->getQueryInfo(queryId);
        LOGS(_log, LOG_LVL_DEBUG,
             "found QMeta record: czar=" << _qInfo.czarId() << " status=" << _qInfo.queryStatus()
                                         << " resultLoc=" << _qInfo.resultLocation()
                                         << " msgTableName=" << _qInfo.msgTableName());
    } catch (qmeta::QueryIdError const& exc) {
        std::string message = "No job found for ID=" + std::to_string(queryId);
        LOGS(_log, LOG_LVL_DEBUG, message);
        _messageStore->addErrorMessage("SYSTEM", message);
    } catch (std::exception const& exc) {
        LOGS(_log, LOG_LVL_ERROR, "error in querying QMeta: " << exc.what());
        std::string message = "Internal failure, error in querying QMeta: ";
        message += exc.what();
        _messageStore->addErrorMessage("SYSTEM", message);
    }
}

// Destructor
UserQueryAsyncResult::~UserQueryAsyncResult() {}

std::string UserQueryAsyncResult::getError() const { return std::string(); }

void UserQueryAsyncResult::submit() {
    _qState = ERROR;

    // if there are messages already it means the error was detected, stop right here
    if (_messageStore->messageCount() > 0) {
        LOGS(_log, LOG_LVL_WARN,
             "UserQueryAsyncResult::submit giving up, messageCount=" << _messageStore->messageCount());
        return;
    }

    // Presently we cannot return query results that originated from different czar
    if (_qInfo.czarId() != _qMetaCzarId) {
        // TODO: tell user which czar was it?
        std::string message = "Query originated from different czar";
        LOGS(_log, LOG_LVL_WARN, "UserQueryAsyncResult::submit giving up, message=" << message);
        _messageStore->addErrorMessage("SYSTEM", message);
        return;
    }

    // TODO: check user name, does not matter now as we are not keeping tack of users.
    // TODO: this is supposed to be used with ASYNC queries only but I can imagine that
    // it could be useful with SYNC too if/when we manage result lifetime properly

    // If query has not finished yet return error
    // TODO: there may be more info available if status is FAILED or ABORTED
    if (_qInfo.queryStatus() != qmeta::QInfo::COMPLETED) {
        std::string message = "Query is still executing (or FAILED)";
        LOGS(_log, LOG_LVL_DEBUG, message);
        _messageStore->addErrorMessage("SYSTEM", message);
        return;
    }

    // Can only return results from mysql tables
    if (_qInfo.resultLocation().compare(0, 6, "table:") != 0) {
        std::string message = "Cannot return result as it is not stored in table.";
        LOGS(_log, LOG_LVL_DEBUG, message);
        _messageStore->addErrorMessage("SYSTEM", message);
        return;
    }
    std::string const resultTableName = _qInfo.resultLocation().substr(6);

    // check that message and result tables exist
    sql::SqlErrorObject sqlErrObj;
    if (!_resultDbConn->tableExists(_qInfo.msgTableName(), sqlErrObj) or
        !_resultDbConn->tableExists(resultTableName, sqlErrObj)) {
        std::string message = "Result or message table does not exist, result is likely expired.";
        LOGS(_log, LOG_LVL_DEBUG, message);
        _messageStore->addErrorMessage("SYSTEM", message);
        return;
    }

    // all checks are OK, copy message table from original query
    // into the message store, at this point original result table must be unlocked
    std::string query = "SELECT chunkId, code, message, severity, timeStamp FROM " + _qInfo.msgTableName();
    sql::SqlResults sqlResults;
    if (!_resultDbConn->runQuery(query, sqlResults, sqlErrObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to retrieve message table data: " << sqlErrObj.errMsg());
        std::string message = "Failed to retrieve message table data.";
        _messageStore->addErrorMessage("SYSTEM_SQL", message);
        return;
    }

    // copy messages
    int count = 0;
    for (auto&& row : sqlResults) {
        try {
            int chunkId = boost::lexical_cast<int>(row[0].first);
            int code = boost::lexical_cast<int>(row[1].first);
            std::string message = row[2].first;
            std::string sevStr = row[3].first;
            int64_t timestampMilli = boost::lexical_cast<double>(row[4].first);
            MessageSeverity sev = sevStr == "INFO" ? MSG_INFO : MSG_ERROR;
            qmeta::JobStatus::Clock::duration duration = std::chrono::milliseconds(timestampMilli);
            qmeta::JobStatus::TimeType timestamp(duration);
            _messageStore->addMessage(chunkId, "DUPLICATE", code, message, sev, timestamp);
        } catch (std::exception const& exc) {
            LOGS(_log, LOG_LVL_ERROR, "Error reading message table data: " << exc.what());
            std::string message = "Error reading message table data.";
            _messageStore->addErrorMessage("SYSTEM", message);
            return;
        }
        ++count;
    }
    LOGS(_log, LOG_LVL_DEBUG, "Copied " << count << " messages from " << _qInfo.msgTableName());

    // Original message table is not useful any more because the result table
    // will be deleted by proxy anyways. Until we have better lifetime management
    // of results I'm going to drop this table now, meaning result can be only
    // retrieved once.
    query = "DROP TABLE " + _qInfo.msgTableName();
    if (!_resultDbConn->runQuery(query, sqlErrObj)) {
        LOGS(_log, LOG_LVL_ERROR, "Failed to drop message table: " << sqlErrObj.errMsg());
        // Users do not care about this error, so don't send it upstream.
    } else {
        LOGS(_log, LOG_LVL_DEBUG, "Deleted message table " << _qInfo.msgTableName());
    }

    // done
    _qState = SUCCESS;
}

QueryState UserQueryAsyncResult::join() { return _qState; }

void UserQueryAsyncResult::kill() {}

void UserQueryAsyncResult::discard() {}

std::shared_ptr<qmeta::MessageStore> UserQueryAsyncResult::getMessageStore() { return _messageStore; }

std::string UserQueryAsyncResult::getResultTableName() const {
    if (_qInfo.resultLocation().compare(0, 6, "table:") == 0) {
        return _qInfo.resultLocation().substr(6);
    } else {
        return std::string();
    }
}

std::string UserQueryAsyncResult::getResultLocation() const { return "table:" + getResultTableName(); }

std::string UserQueryAsyncResult::getResultQuery() const { return _qInfo.resultQuery(); }

}  // namespace lsst::qserv::ccontrol
