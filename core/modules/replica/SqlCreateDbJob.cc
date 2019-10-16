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

// Class header
#include "replica/SqlCreateDbJob.h"

// System headers
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "replica/Configuration.h"
#include "replica/ServiceProvider.h"
#include "replica/SqlCreateDbRequest.h"
#include "replica/StopRequest.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.SqlCreateDbJob");

} /// namespace

namespace lsst {
namespace qserv {
namespace replica {

string SqlCreateDbJob::typeName() { return "SqlCreateDbJob"; }


SqlCreateDbJob::Ptr SqlCreateDbJob::create(
        string const& database,
        bool allWorkers,
        Controller::Ptr const& controller,
        string const& parentJobId,
        CallbackType const& onFinish,
        Job::Options const& options) {

    return Ptr(new SqlCreateDbJob(
        database,
        allWorkers,
        controller,
        parentJobId,
        onFinish,
        options
    ));
}


SqlCreateDbJob::SqlCreateDbJob(string const& database,
                               bool allWorkers,
                               Controller::Ptr const& controller,
                               string const& parentJobId,
                               CallbackType const& onFinish,
                               Job::Options const& options)
    :   SqlJob(0,
               allWorkers,
               controller,
               parentJobId,
               "SQL_CREATE_DATABASE",
               options),
        _database(database),
        _onFinish(onFinish) {
}


list<pair<string,string>> SqlCreateDbJob::extendedPersistentState() const {
    list<pair<string,string>> result;
    result.emplace_back("database", database());
    result.emplace_back("all_workers", string(allWorkers() ? "1" : "0"));
    return result;
}


list<SqlRequest::Ptr> SqlCreateDbJob::launchRequests(util::Lock const& lock,
                                                     string const& worker,
                                                     size_t maxRequests) {

    // Launch exactly one request per worker unless it was already
    // launched earlier

    list<SqlRequest::Ptr> requests;
    if (not _workers.count(worker) and maxRequests != 0) {
        auto const self = shared_from_base<SqlCreateDbJob>();
        requests.push_back(
            controller()->sqlCreateDb(
                worker,
                database(),
                [self] (SqlCreateDbRequest::Ptr const& request) {
                    self->onRequestFinish(request);
                },
                options(lock).priority,
                true,   /* keepTracking*/
                id()    /* jobId */
            )
        );
        _workers.insert(worker);
    }
    return requests;
}


void SqlCreateDbJob::stopRequest(util::Lock const& lock,
                                 SqlRequest::Ptr const& request) {
    controller()->stopById<StopSqlCreateDbRequest>(
        request->worker(),
        request->id(),
        nullptr,    /* onFinish */
        options(lock).priority,
        true,       /* keepTracking */
        id()        /* jobId */
    );
}


void SqlCreateDbJob::notify(util::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__ << "[" << typeName() << "]");
    notifyDefaultImpl<SqlCreateDbJob>(lock, _onFinish);
}

}}} // namespace lsst::qserv::replica
