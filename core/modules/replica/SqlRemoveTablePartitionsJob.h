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
#ifndef LSST_QSERV_REPLICA_SQLREMOVETABLEPARTITIONSJOB_H
#define LSST_QSERV_REPLICA_SQLREMOVETABLEPARTITIONSJOB_H

// System headers
#include <functional>
#include <list>
#include <set>
#include <string>
#include <tuple>

// Qserv headers
#include "replica/SqlJob.h"

// This header declarations
namespace lsst {
namespace qserv {
namespace replica {

/**
 * Class SqlRemoveTablePartitionsJob represents a tool which will broadcast
 * the same request for removing MySQL partitions from existing table from all
 * worker databases of a setup.
 * 
 * Note, that the algorithm treats regular and partitioned tables quite differently.
 * For the regular tables it will indeed broadcast exactly the same request
 * (to the exact table specified as the corresponding parameter of the job)
 * to all workers. The regular tables must be present at all workers.
 * The partitioned (chunked) tables will be treated quite differently. First of
 * all, the name of a table specified as a parameter of the class will be treated
 * as a class of the tables, and a group of table-specific AND(!) chunk-specific
 * requests will be generated for such table. For example, if the table name is:
 *
 *   'Object'
 * 
 * and the following table replicas existed for the table at a time of the request:
 * 
 *    worker | chunk
 *   --------+-----------------------
 *      A    |  123
 *   --------+-----------------------
 *      B    |  234
 *   --------+-----------------------
 *      C    |  234
 *      D    |  345
 *
 * then the low-level requests will be sent for the following tables to
 * the corresponding workers:
 * 
 *    worker | table
 *   --------+-----------------------
 *      A    | Object
 *      A    | Object_123
 *      A    | ObjectFullOverlap_123
 *   --------+-----------------------
 *      B    | Object
 *      B    | Object_234
 *      B    | ObjectFullOverlap_234
 *   --------+-----------------------
 *      C    | Object
 *      C    | Object_234
 *      C    | ObjectFullOverlap_234
 *   --------+-----------------------
 *      D    | Object
 *      D    | Object_345
 *      D    | ObjectFullOverlap_345
 */
class SqlRemoveTablePartitionsJob : public SqlJob {
public:
    /// The pointer type for instances of the class
    typedef std::shared_ptr<SqlRemoveTablePartitionsJob> Ptr;

    /// The function type for notifications on the completion of the request
    typedef std::function<void(Ptr)> CallbackType;

    /// @return the unique name distinguishing this class from other types of jobs
    static std::string typeName();

    /**
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * @param database
     *   the name of a database from which a table will be deleted
     *
     * @param table
     *   the name of an existing table to be affected by the operation
     *
     * @param allWorkers
     *   engage all known workers regardless of their status. If the flag
     *   is set to 'false' then only 'ENABLED' workers which are not in
     *   the 'READ-ONLY' state will be involved into the operation.
     *
     * @param controller
     *   is needed launching requests and accessing the Configuration
     *
     * @param parentJobId
     *   (optional) identifier of a parent job
     *
     * @param onFinish
     *   (optional) callback function to be called upon a completion of the job
     *
     * @param options
     *   (optional) defines the job priority, etc.
     *
     * @return
     *   pointer to the created object
     */
    static Ptr create(std::string const& database,
                      std::string const& table,
                      bool allWorkers,
                      Controller::Ptr const& controller,
                      std::string const& parentJobId=std::string(),
                      CallbackType const& onFinish=nullptr,
                      Job::Options const& options=defaultOptions());

    // Default construction and copy semantics are prohibited

    SqlRemoveTablePartitionsJob() = delete;
    SqlRemoveTablePartitionsJob(SqlRemoveTablePartitionsJob const&) = delete;
    SqlRemoveTablePartitionsJob& operator=(SqlRemoveTablePartitionsJob const&) = delete;

    ~SqlRemoveTablePartitionsJob() final = default;

    // Trivial get methods

    std::string const& database() const { return _database; }
    std::string const& table()    const { return _table; }

    /// @see Job::extendedPersistentState()
    std::list<std::pair<std::string,std::string>> extendedPersistentState() const final;

protected:
    /// @see Job::notify()
    void notify(util::Lock const& lock) final;

    /// @see SqlJob::launchRequests()
    std::list<SqlRequest::Ptr> launchRequests(util::Lock const& lock,
                                              std::string const& worker,
                                              size_t maxRequests) final;

    /// @see SqlJob::stopRequest()
    void stopRequest(util::Lock const& lock,
                     SqlRequest::Ptr const& request) final;

private:
    /// @see SqlRemoveTablePartitionsJob::create()
    SqlRemoveTablePartitionsJob(std::string const& database,
                                std::string const& table,
                                bool allWorkers,
                                Controller::Ptr const& controller,
                                std::string const& parentJobId,
                                CallbackType const& onFinish,
                                Job::Options const& options);

    // Input parameters

    std::string const _database;
    std::string const _table;

    CallbackType _onFinish;     /// @note is reset when the job finishes

    /// Is set in the constructor by pulling table status from the Configuration
    bool _isPartitioned = false;

    /// A collection of per-worker tables for which the remote operations are
    /// required. Each worker-specific sub-collections gets initialized
    /// just once upon the very first request to the request launching method
    /// in a context of the corresponding worker. Hence there are three states
    /// of the sub-collections:
    ///
    /// - (initial) no worker key exists. At this state the algorithm would
    ///   initialize the sub-collection if called at the request launching method
    ///   for the first time in a context of the worker.
    /// - (populated) will be used for making requests to the worker. Each time
    ///   a request for a table is sent to the worker the table gets removed from
    ///   from the sub-collection
    /// - (empty) the worker key exists. This means no tables to be processed for
    ///   by the worker exists. The tables have either been all processed, or
    ///   the collection was made empty upon the initialization.
    std::map<std::string, std::list<std::string>> _workers2tables;
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_REPLICA_SQLREMOVETABLEPARTITIONSJOB_H
