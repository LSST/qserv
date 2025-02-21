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

#ifndef LSST_QSERV_CCONTROL_USERQUERYSET_H
#define LSST_QSERV_CCONTROL_USERQUERYSET_H

// System headers
#include <memory>

// Third-party headers

// Qserv headers
#include "ccontrol/QueryState.h"
#include "ccontrol/UserQuery.h"

namespace lsst::qserv::ccontrol {

/// UserQuerySet : for handling administrative queries like "SET GLOBAL var = value"
/// This can be expanded to support other administrative queries if desired.
/// See the grammar in MySqlParser.g4 for a summary of administrative queries.
class UserQuerySet : public UserQuery {
public:
    typedef std::shared_ptr<UserQuerySet> Ptr;

    UserQuerySet(std::string const& varName, std::string const& varValue);

    virtual ~UserQuerySet() {}

    /// Get the name of the variable being set.
    std::string const& varName() const { return _varName; }

    /// Get the value of the variable being set.
    std::string const& varValue() const { return _varValue; }

    /// @return a non-empty string describing the current error state
    /// Returns an empty string if no errors have been detected.
    std::string getError() const override { return std::string(); }

    /// Begin execution of the query over all ChunkSpecs added so far.
    void submit() override {};

    /// Wait until the query has completed execution.
    /// @return the final execution state.
    QueryState join() override { return _qState; }

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
    std::string getResultTableName() const { return ""; }

    /// Result location could be something like "table:table_name" or
    /// "file:/path/to/file.csv".
    /// @return Result location for this query, can be empty
    std::string getResultLocation() const override { return ""; }

    /// @return True if query is async query
    bool isAsync() const override { return false; }

    /// @return get the SELECT statement to be executed by proxy
    std::string getResultQuery() const override { return ""; };

private:
    std::string _varName;
    std::string _varValue;
    QueryState _qState{SUCCESS};
    std::shared_ptr<qmeta::MessageStore> _messageStore;
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERYSET_H
