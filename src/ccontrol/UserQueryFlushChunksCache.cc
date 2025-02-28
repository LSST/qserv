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
#include "ccontrol/UserQueryFlushChunksCache.h"

// System headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "css/CssAccess.h"
#include "css/EmptyChunks.h"
#include "qmeta/MessageStore.h"
#include "sql/SqlConnection.h"
#include "sql/SqlErrorObject.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.ccontrol.UserQueryFlushChunksCache");
}

namespace lsst::qserv::ccontrol {

// Constructor
UserQueryFlushChunksCache::UserQueryFlushChunksCache(std::shared_ptr<css::CssAccess> const& css,
                                                     std::string const& dbName,
                                                     sql::SqlConnection* resultDbConn)
        : _css(css),
          _dbName(dbName),
          _resultDbConn(resultDbConn),
          _qState(UNKNOWN),
          _messageStore(std::make_shared<qmeta::MessageStore>()) {}

std::string UserQueryFlushChunksCache::getError() const { return std::string(); }

// Attempt to kill in progress.
void UserQueryFlushChunksCache::kill() {}

// Submit or execute the query.
void UserQueryFlushChunksCache::submit() {
    LOGS(_log, LOG_LVL_INFO, "Flushing empty chunks for db: " << _dbName);

    // reset empty chunk cache , this does not throw
    auto emptyChunks = _css->getEmptyChunks();
    if (emptyChunks != nullptr) {
        emptyChunks->clearCache(_dbName);
        _qState = SUCCESS;
    }
}

// Block until a submit()'ed query completes.
QueryState UserQueryFlushChunksCache::join() {
    // everything should be done in submit()
    return _qState;
}

// Release resources.
void UserQueryFlushChunksCache::discard() {
    // no resources
}

}  // namespace lsst::qserv::ccontrol
