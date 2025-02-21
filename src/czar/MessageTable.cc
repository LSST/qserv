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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */

// Class header
#include "czar/MessageTable.h"

// System headers

// Third-party headers
#include "boost/format.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "ccontrol/ConfigMap.h"
#include "ccontrol/UserQuery.h"
#include "czar/CzarErrors.h"
#include "qmeta/MessageStore.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.MessageTable");

#define MAX_MESSAGE_LEN "1024"  // string, for splicing into templates below

std::string const createTmpl(
        "CREATE TABLE IF NOT EXISTS %1% "
        "(chunkId INT, code SMALLINT, message VARCHAR(" MAX_MESSAGE_LEN
        "), "
        "severity ENUM ('INFO', 'ERROR'), timeStamp BIGINT UNSIGNED)"
        "ENGINE=MEMORY");

std::string const createAndLockTmpl(createTmpl + "; LOCK TABLES %1% WRITE;");

std::string const writeTmpl(
        "INSERT INTO %1% (chunkId, code, message, severity, timeStamp) "
        "VALUES (%2%, %3%, '%4$." MAX_MESSAGE_LEN "s', '%5%', %6%)");

// mysql can only unlock all locked tables,
// there is no command to unlock single table
std::string const unlockTmpl("UNLOCK TABLES");

}  // namespace

namespace lsst::qserv::czar {

// Constructors
MessageTable::MessageTable(std::string const& tableName, mysql::MySqlConfig const& resultConfig)
        : _tableName(tableName), _sqlConn(sql::SqlConnectionFactory::make(resultConfig)) {}

// Create the table, do not lock
void MessageTable::create() {
    std::string query = (boost::format(::createTmpl) % _tableName).str();
    sql::SqlErrorObject sqlErr;
    LOGS(_log, LOG_LVL_DEBUG, "creating message table " << _tableName);
    if (not _sqlConn->runQuery(query, sqlErr)) {
        SqlError exc(ERR_LOC, "Failure creating message table", sqlErr);
        LOGS(_log, LOG_LVL_ERROR, exc.message());
        throw exc;
    }
}

// Create and lock the table
void MessageTable::lock() {
    std::string query = (boost::format(::createAndLockTmpl) % _tableName).str();
    sql::SqlErrorObject sqlErr;
    LOGS(_log, LOG_LVL_DEBUG, "locking message table " << _tableName);
    if (not _sqlConn->runQuery(query, sqlErr)) {
        SqlError exc(ERR_LOC, "Failure locking message table", sqlErr);
        LOGS(_log, LOG_LVL_ERROR, exc.message());
        throw exc;
    }
}

// Release lock on message table so that proxy can proceed
void MessageTable::unlock(ccontrol::UserQuery::Ptr const& userQuery) {
    _saveQueryMessages(userQuery);

    sql::SqlErrorObject sqlErr;
    LOGS(_log, LOG_LVL_DEBUG, "unlocking message table " << _tableName);
    if (not _sqlConn->runQuery(::unlockTmpl, sqlErr)) {
        SqlError exc(ERR_LOC, "Failure unlocking message table", sqlErr);
        LOGS(_log, LOG_LVL_ERROR, exc.message());
        throw exc;
    }
}

// store all messages from current session to the table
void MessageTable::_saveQueryMessages(ccontrol::UserQuery::Ptr const& userQuery) {
    if (not userQuery) {
        return;
    }

    auto msgStore = userQuery->getMessageStore();
    int completeCount = 0;
    int cancelCount = 0;
    std::string multiErrStr = "";
    std::string severity = "INFO";

    // Collect information about the query and put it in the message table.
    int msgCount = msgStore->messageCount();
    for (int i = 0; i != msgCount; ++i) {
        const qmeta::QueryMessage& qm = msgStore->getMessage(i);
        std::string src = qm.msgSource;
        if (src == "COMPLETE") {
            ++completeCount;
        } else if (src == "CANCEL") {
            ++cancelCount;
        } else if (src == "MULTIERROR") {
            multiErrStr += qm.description + "\n";
            severity = "ERROR";
        }
    }
    std::string cMsg("Completed chunks=");
    cMsg += std::to_string(completeCount) + " cancelled chunks=" + std::to_string(cancelCount) + "\n";
    cMsg += multiErrStr;
    LOGS(_log, LOG_LVL_DEBUG, " MULTIERROR:" << cMsg);
    std::string summaryQ = (boost::format(::writeTmpl) % _tableName % "-1" % "-1" %
                            _sqlConn->escapeString(cMsg) % severity % std::time(nullptr))
                                   .str();
    sql::SqlErrorObject sqlE;
    if (not _sqlConn->runQuery(summaryQ, sqlE)) {
        SqlError exc(ERR_LOC, "Failure updating message table", sqlE);
        LOGS(_log, LOG_LVL_ERROR, exc.message());
        throw exc;
    }
}

}  // namespace lsst::qserv::czar
