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

#ifndef LSST_QSERV_CCONTROL_USERQUERYINVALID_H
#define LSST_QSERV_CCONTROL_USERQUERYINVALID_H

// System headers
#include <memory>
#include <mutex>

// Third-party headers

// Qserv headers
#include "ccontrol/UserQuery.h"
#include "qmeta/MessageStore.h"
#include "qmeta/types.h"

// Forward decl

namespace lsst::qserv::ccontrol {

/// UserQueryInvalid : implementation of the UserQuery which is used
/// to indicate invalid queries.
class UserQueryInvalid : public UserQuery {
public:
    UserQueryInvalid(std::string const& message)
            : _message(message), _messageStore(std::make_shared<qmeta::MessageStore>()) {}

    UserQueryInvalid(UserQueryInvalid const&) = delete;
    UserQueryInvalid& operator=(UserQueryInvalid const&) = delete;

    // Accessors

    /// @return a non-empty string describing the current error state
    /// Returns an empty string if no errors have been detected.
    virtual std::string getError() const override { return _message; }

    /// Begin execution of the query over all ChunkSpecs added so far.
    virtual void submit() override {}

    /// Wait until the query has completed execution.
    /// @return the final execution state.
    virtual QueryState join() override { return ERROR; }

    /// Stop a query in progress (for immediate shutdowns)
    virtual void kill() override {}

    /// Release resources related to user query
    virtual void discard() override {}

    // Delegate objects
    virtual std::shared_ptr<qmeta::MessageStore> getMessageStore() override { return _messageStore; }

private:
    std::string const _message;
    std::shared_ptr<qmeta::MessageStore> _messageStore;
};

}  // namespace lsst::qserv::ccontrol

#endif  // LSST_QSERV_CCONTROL_USERQUERYINVALID_H
