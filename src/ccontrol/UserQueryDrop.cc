// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 AURA/LSST.
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
#include "ccontrol/UserQueryDrop.h"

// System headers
#include <ctime>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "css/CssAccess.h"
#include "css/CssError.h"
#include "qmeta/Exceptions.h"
#include "qmeta/MessageStore.h"
#include "qmeta/QMeta.h"
#include "sql/SqlConnection.h"
#include "sql/SqlErrorObject.h"
#include "util/IterableFormatter.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQueryDrop");
}

namespace lsst::qserv::ccontrol {

// Constructor
UserQueryDrop::UserQueryDrop(std::shared_ptr<css::CssAccess> const& css, std::string const& dbName,
                             std::string const& tableName, sql::SqlConnection* resultDbConn,
                             std::shared_ptr<qmeta::QMeta> const& queryMetadata, qmeta::CzarId qMetaCzarId)
        : _css(css),
          _dbName(dbName),
          _tableName(tableName),
          _resultDbConn(resultDbConn),
          _queryMetadata(queryMetadata),
          _qMetaCzarId(qMetaCzarId),
          _qState(UNKNOWN),
          _messageStore(std::make_shared<qmeta::MessageStore>()),
          _sessionId(0) {}

std::string UserQueryDrop::getError() const { return std::string(); }

// Attempt to kill in progress.
void UserQueryDrop::kill() {}

// Submit or execute the query.
void UserQueryDrop::submit() {
    // Just mark this db/table in CSS with special status, watcher
    // will take care of the actual delete process

    LOGS(_log, LOG_LVL_INFO, "About to drop: " << _dbName << "." << _tableName);

    // check current status of table or db, if not READY then fail
    if (not _checkStatus()) {
        return;
    }

    // Add this query to QMeta so that progress can be tracked,
    // QMeta needs to be updated by watcher when it finishes with the table
    // so we embed query id into CSS table status below
    qmeta::QInfo::QType qType = qmeta::QInfo::ASYNC;
    std::string user = "anonymous";  // we do not have access to that info yet
    std::string query;
    if (_tableName.empty()) {
        query = "DROP DATABASE " + _dbName;
    } else {
        query = "DROP TABLE " + _dbName + "." + _tableName;
    }
    qmeta::QInfo qInfo(qType, _qMetaCzarId, user, query, "", "", "", "", "", 0);
    qmeta::QMeta::TableNames tableNames;
    QueryId qMetaQueryId = 0;
    try {
        qMetaQueryId = _queryMetadata->registerQuery(qInfo, tableNames);
    } catch (qmeta::QMetaError const& exc) {
        // not fatal, just print error message and continue
        LOGS(_log, LOG_LVL_WARN, "QMeta failure (non-fatal): " << exc.what());
    }

    // update status to trigger watcher
    std::string newStatus = css::KEY_STATUS_DROP_PFX + std::to_string(::time(nullptr)) +
                            ":qid=" + std::to_string(qMetaQueryId);
    LOGS(_log, LOG_LVL_DEBUG, "new db/table status: " << newStatus);
    try {
        // TODO: it's better to do it in one atomic operation with
        // getStatus, but CSS API does not have this option yet
        if (_tableName.empty()) {
            _css->setDbStatus(_dbName, newStatus);
        } else {
            _css->setTableStatus(_dbName, _tableName, newStatus);
        }
        _qState = SUCCESS;
    } catch (css::NoSuchDb const& exc) {
        // Has it disappeared already?
        LOGS(_log, LOG_LVL_ERROR, "database disappeared from CSS");
        std::string message = std::string("Unknown database ") + _dbName;
        _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
        _qState = ERROR;
    } catch (css::NoSuchTable const& exc) {
        // Has it disappeared already?
        LOGS(_log, LOG_LVL_ERROR, "table disappeared from CSS");
        std::string message = "Unknown table " + _dbName + "." + _tableName;
        _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
        _qState = ERROR;
    } catch (css::CssError const& exc) {
        LOGS(_log, LOG_LVL_ERROR, "CSS failure: " << exc.what());
        std::string message = "CSS error: " + std::string(exc.what());
        _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
        _qState = ERROR;
    }

    // if failed then update status in QMeta
    if (_qState == ERROR) {
        if (qMetaQueryId) {
            try {
                // rows and bytes collected from workers should be 0.
                _queryMetadata->completeQuery(qMetaQueryId, qmeta::QInfo::FAILED);
            } catch (qmeta::QMetaError const& exc) {
                // not fatal, just print error message and continue
                LOGS(_log, LOG_LVL_WARN, "QMeta failure (non-fatal): " << exc.what());
            }
        }
    }
}

// Block until a submit()'ed query completes.
QueryState UserQueryDrop::join() {
    // everything should be done in submit()
    return _qState;
}

// Release resources.
void UserQueryDrop::discard() {
    // no resources
}

bool UserQueryDrop::_checkStatus() {
    // check current status of table or db, if not READY then fail
    try {
        if (_tableName.empty()) {
            // check database status
            auto statusMap = _css->getDbStatus();
            LOGS(_log, LOG_LVL_DEBUG, "all db status: " << util::printable(statusMap));
            if (statusMap.count(_dbName) != 1) {
                std::string message = "Unknown database " + _dbName;
                _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
                _qState = ERROR;
                return false;
            }
            LOGS(_log, LOG_LVL_DEBUG, "db status: " << statusMap[_dbName]);
            if (statusMap[_dbName] != css::KEY_STATUS_READY) {
                std::string message =
                        "Unexpected status for database: " + _dbName + ": " + statusMap[_dbName];
                _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
                _qState = ERROR;
                return false;
            }
        } else {
            // check table status
            auto statusMap = _css->getTableStatus(_dbName);
            LOGS(_log, LOG_LVL_DEBUG, "all table status: " << util::printable(statusMap));
            if (statusMap.count(_tableName) != 1) {
                std::string message = "Unknown table " + _dbName + "." + _tableName;
                _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
                _qState = ERROR;
                return false;
            }
            LOGS(_log, LOG_LVL_DEBUG, "table status: " << statusMap[_tableName]);
            if (statusMap[_tableName] != css::KEY_STATUS_READY) {
                std::string message = "Unexpected status for table: " + _dbName + "." + _tableName + ": " +
                                      statusMap[_tableName];
                _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
                _qState = ERROR;
                return false;
            }
        }
    } catch (css::CssError const& exc) {
        LOGS(_log, LOG_LVL_ERROR, "css failure: " << exc.what());
        std::string message = "CSS error: " + std::string(exc.what());
        _messageStore->addMessage(-1, "CSS", 1051, message, MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return false;
    }
    return true;
}

}  // namespace lsst::qserv::ccontrol
