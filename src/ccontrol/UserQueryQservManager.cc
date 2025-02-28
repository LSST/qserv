// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2019 LSST Corporation.
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
#include "ccontrol/UserQueryQservManager.h"

// System headers
#include <list>
#include <stdexcept>

// Third party headers
#include <nlohmann/json.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "cconfig/CzarConfig.h"
#include "qmeta/MessageStore.h"
#include "sql/SqlBulkInsert.h"
#include "sql/SqlConnection.h"
#include "sql/SqlConnectionFactory.h"

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQueryQservManager");

}

namespace lsst::qserv::ccontrol {

UserQueryQservManager::UserQueryQservManager(shared_ptr<UserQueryResources> const& queryResources,
                                             string const& value)
        : _value(value),
          _resultTableName("qserv_manager_" + queryResources->userQueryId),
          _messageStore(make_shared<qmeta::MessageStore>()),
          _resultDb(queryResources->resultDb) {}

void UserQueryQservManager::submit() {
    LOGS(_log, LOG_LVL_TRACE, "processing command: " << _value);

    // IMPORTANT: make a new connection each time since a state of the database service
    // is not deterministic and the SQL library available to Czar is not terribly reliable
    // (not able to properly handle disconnects).
    auto const czarConfig = cconfig::CzarConfig::instance();
    auto const resultDbConn = sql::SqlConnectionFactory::make(czarConfig->getMySqlResultConfig());

    // Create the table.
    string const createTable = "CREATE TABLE " + _resultTableName + "(`result` BLOB)";
    vector<string> resColumns;  // This must match the schema in the CREATE TABLE statement.
    resColumns.push_back("result");

    LOGS(_log, LOG_LVL_TRACE, "creating result table: " << createTable);
    sql::SqlErrorObject errObj;
    if (!resultDbConn->runQuery(createTable, errObj)) {
        LOGS(_log, LOG_LVL_ERROR, "failed to create result table: " << errObj.errMsg());
        string const message = "Internal failure, failed to create result table: " + errObj.errMsg();
        _messageStore->addMessage(-1, "SQL", 1051, message, MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }

    // Return a value of the original input (which includeds quotes).
    list<vector<string>> rows;
    vector<string> row = {_value};
    rows.push_back(move(row));

    // Ingest row(s) into the table.
    bool success = true;
    sql::SqlBulkInsert bulkInsert(resultDbConn.get(), _resultTableName, resColumns);
    for (auto const& row : rows) {
        success = success && bulkInsert.addRow(row, errObj);
        if (!success) break;
    }
    if (success) success = bulkInsert.flush(errObj);
    if (!success) {
        LOGS(_log, LOG_LVL_ERROR, "error updating result table: " << errObj.errMsg());
        string const message = "Internal failure, error updating result table: " + errObj.errMsg();
        _messageStore->addMessage(-1, "SQL", 1051, message, MessageSeverity::MSG_ERROR);
        _qState = ERROR;
        return;
    }
    _qState = SUCCESS;
}

string UserQueryQservManager::getResultQuery() const {
    string ret = "SELECT * FROM " + _resultDb + "." + _resultTableName;
    return ret;
}

}  // namespace lsst::qserv::ccontrol
