// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015 LSST Corporation.
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

#ifndef LSST_QSERV_CCONTROL_USERQUERY_H
#define LSST_QSERV_CCONTROL_USERQUERY_H
/**
 * @file
 *
 * @brief Umbrella container for user query state
 *
 * @author Daniel L. Wang, SLAC
 */

// System headers
#include <memory>

// Third-party headers

// Qserv headers
#include "ccontrol/QueryState.h"
#include "global/intTypes.h"
#include "qmeta/types.h"

// Forward decl
namespace lsst::qserv::qmeta {
class MessageStore;
}  // namespace lsst::qserv::qmeta

namespace lsst::qserv::ccontrol {

/// UserQuery : interface for user query data. Not thread-safe, although
/// its delegates are thread-safe as appropriate.
class UserQuery {
public:
    typedef std::shared_ptr<UserQuery> Ptr;

    virtual ~UserQuery() {}

    /// @return a non-empty string describing the current error state
    /// Returns an empty string if no errors have been detected.
    virtual std::string getError() const = 0;

    /// Begin execution of the query over all ChunkSpecs added so far.
    virtual void submit() = 0;

    /// Wait until the query has completed execution.
    /// @return the final execution state.
    virtual QueryState join() = 0;

    /// Stop a query in progress (for immediate shutdowns)
    virtual void kill() = 0;

    /// Release resources related to user query
    virtual void discard() = 0;

    // Delegate objects
    virtual std::shared_ptr<qmeta::MessageStore> getMessageStore() = 0;

    /// This method should disappear when we start supporting results
    /// in locations other than MySQL tables. We'll switch to getResultLocation()
    /// at that point.
    /// @return Name of the result table for this query, can be empty
    virtual std::string getResultTableName() const { return std::string(); }

    /// Result location could be something like "table:table_name" or
    /// "file:/path/to/file.csv".
    /// @return Result location for this query, can be empty
    virtual std::string getResultLocation() const { return std::string(); }

    /// @return this query's QueryId.
    virtual QueryId getQueryId() const { return QueryId(0); }

    /// @return this query's QueryId string. Many query types do not have valid Id numbers.
    virtual std::string getQueryIdString() const {
        // return a string indicating this query has no QueryId.
        return QueryIdHelper::makeIdStr(0, true);
    }

    /// @return True if query is async query
    virtual bool isAsync() const { return false; }

    /// @return get the SELECT statement to be executed by proxy
    virtual std::string getResultQuery() const { return std::string(); }
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERY_H
