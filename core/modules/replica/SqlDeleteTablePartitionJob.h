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
#ifndef LSST_QSERV_REPLICA_SQLDELETETABLEPARTITIONJOB_H
#define LSST_QSERV_REPLICA_SQLDELETETABLEPARTITIONJOB_H

// System headers
#include <cstdint>
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
 * Class SqlDeleteTablePartitionJob represents a tool which will broadcast
 * the same request for removing a MySQL partition corresponding to a given
 * super-transaction from existing table from all worker databases of a setup.
 * Result sets are collected in the above defined data structure.
 */
class SqlDeleteTablePartitionJob : public SqlJob {
public:
    /// The pointer type for instances of the class
    typedef std::shared_ptr<SqlDeleteTablePartitionJob> Ptr;

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
     * @param transactionId
     *   an identifier of a super-transaction corresponding to a MySQL partition
     *   to be dropped. The transaction must exist, and it should be in
     *   the ABORTED state.
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
                      uint32_t transactionId,
                      bool allWorkers,
                      Controller::Ptr const& controller,
                      std::string const& parentJobId=std::string(),
                      CallbackType const& onFinish=nullptr,
                      Job::Options const& options=defaultOptions());

    // Default construction and copy semantics are prohibited

    SqlDeleteTablePartitionJob() = delete;
    SqlDeleteTablePartitionJob(SqlDeleteTablePartitionJob const&) = delete;
    SqlDeleteTablePartitionJob& operator=(SqlDeleteTablePartitionJob const&) = delete;

    ~SqlDeleteTablePartitionJob() final = default;

    // Trivial get methods

    std::string const& database() const { return _database; }
    std::string const& table()    const { return _table; }

    uint32_t transactionId() const { return _transactionId; }


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
    /// @see SqlDeleteTablePartitionJob::create()
    SqlDeleteTablePartitionJob(std::string const& database,
                               std::string const& table,
                               uint32_t transactionId,
                               bool allWorkers,
                               Controller::Ptr const& controller,
                               std::string const& parentJobId,
                               CallbackType const& onFinish,
                               Job::Options const& options);

    // Input parameters

    std::string const _database;
    std::string const _table;
    uint32_t    const _transactionId;

    CallbackType _onFinish;     /// @note is reset when the job finishes

    /// A registry of workers to mark those for which request has been sent.
    /// The registry prevents duplicate requests because exactly one
    /// such request is permitted to be sent to each worker.
    std::set<std::string> _workers;
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_REPLICA_SQLDELETETABLEPARTITIONJOB_H
