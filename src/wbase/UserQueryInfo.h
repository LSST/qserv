// -*- LSST-C++ -*-

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
#ifndef LSST_QSERV_WBASE_USERQUERYINFO_H
#define LSST_QSERV_WBASE_USERQUERYINFO_H

// System headers
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// Qserv headers
#include "global/intTypes.h"

// This header declarations
namespace lsst::qserv::wbase {

class UberJobData;

/// This class contains information about a user query that is effectively the same
/// for all Task's in the user query.
class UserQueryInfo {
public:
    using Ptr = std::shared_ptr<UserQueryInfo>;
    using Map = std::map<QueryId, std::weak_ptr<UserQueryInfo>>;

    UserQueryInfo() = delete;
    UserQueryInfo(UserQueryInfo const&) = delete;
    UserQueryInfo& operator=(UserQueryInfo const&) = delete;

    static Ptr create(QueryId qId, CzarIdType czarId) {
        return std::shared_ptr<UserQueryInfo>(new UserQueryInfo(qId, czarId));
    }

    ~UserQueryInfo() = default;

    std::string cName(const char* func) {
        return std::string("UserQueryInfo::") + func + " qId=" + std::to_string(_qId);
    }

    /// Add a query template to the map of templates for this user query.
    size_t addTemplate(std::string const& templateStr);

    /// Retrieve the template associated with the key 'id' from the map of templates.
    /// @throws Bug if id is out of range.
    std::string getTemplate(size_t id);

    /// Add an UberJobData object to the UserQueryInfo.
    void addUberJob(std::shared_ptr<UberJobData> const& ujData);

    /// Return true if this user query was cancelled by its czar.
    bool getCancelledByCzar() const { return _cancelledByCzar; }

    /// The czar has cancelled this user query, all tasks need to
    /// be killed but there's no need to track UberJob id's anymore.
    void cancelFromCzar();

    /// Cancel all associated tasks and track the killed UberJob id's
    /// The user query itself may still be alive, so the czar may need
    /// information about which UberJobs are dead.
    void cancelAllUberJobs();

    /// Cancel a specific UberJob in this user query.
    void cancelUberJob(UberJobId ujId);

    bool isUberJobDead(UberJobId ujId) const;

    QueryId getQueryId() const { return _qId; }

    CzarIdType getCzarId() const { return _czarId; }

private:
    UserQueryInfo(QueryId qId, CzarIdType czId);

    QueryId const _qId;  ///< The User Query Id number.
    CzarIdType const _czarId;

    /// List of template strings. This is expected to be short, 1 or 2 entries.
    /// This must be a vector. New entries are always added to the end so as not
    /// to alter existing indexes into the vector.
    std::vector<std::string> _templates;
    std::mutex _uqMtx;  ///< protects _templates

    /// Map of all UberJobData objects on this worker for this User Query.
    std::map<UberJobId, std::weak_ptr<UberJobData>> _uberJobMap;
    std::set<UberJobId> _deadUberJobSet;  ///< Set of cancelled UberJob Ids.
    mutable std::mutex _uberJobMapMtx;    ///< protects _uberJobMap, _deadUberJobSet

    std::atomic<bool> _cancelledByCzar{false};
};

}  // namespace lsst::qserv::wbase

#endif  // LSST_QSERV_WBASE_USERQUERYINFO_H
