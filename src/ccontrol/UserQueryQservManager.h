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

#ifndef LSST_QSERV_CCONTROL_USERQUERYQSERVMANAGER_H
#define LSST_QSERV_CCONTROL_USERQUERYQSERVMANAGER_H

// System headers
#include <memory>
#include <string>
#include <vector>

// Third-party headers

// Qserv headers
#include "ccontrol/UserQuery.h"
#include "ccontrol/UserQueryError.h"
#include "ccontrol/UserQueryResources.h"
#include "ccontrol/QueryState.h"
#include "global/intTypes.h"

namespace lsst::qserv::qmeta {
class MessageStore;
}  // namespace lsst::qserv::qmeta

namespace lsst::qserv::ccontrol {

// UserQueryQservManager is for handling queries with the form `CALL QSERV_MANAGER("...")`
class UserQueryQservManager : public UserQuery {
public:
    UserQueryQservManager(std::shared_ptr<UserQueryResources> const& queryResources,
                          std::string const& value);

    ~UserQueryQservManager() override = default;

    UserQueryQservManager(UserQueryQservManager const&) = delete;

    UserQueryQservManager& operator=(UserQueryQservManager const&) = delete;

    /// @return a non-empty string describing the current error state
    /// Returns an empty string if no errors have been detected.
    std::string getError() const override { return std::string(); }

    /// Begin execution of the query over all ChunkSpecs added so far.
    void submit() override;

    /// Wait until the query has completed execution.
    /// @return the final execution state.
    QueryState join() override { return _qState; }

    /// Stop a query in progress (for immediate shutdowns)
    void kill() override {}

    /// Release resources related to user query
    void discard() override {}

    // Delegate objects
    std::shared_ptr<qmeta::MessageStore> getMessageStore() override { return _messageStore; }

    std::string getResultLocation() const override { return "table:" + _resultTableName; }

    /// @return get the SELECT statement to be executed by proxy
    std::string getResultQuery() const override;

private:
    std::string const _value;
    std::string _resultTableName;
    std::shared_ptr<qmeta::MessageStore> _messageStore;
    QueryState _qState{UNKNOWN};
    std::string _resultDb;
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERYQSERVMANAGER_H
