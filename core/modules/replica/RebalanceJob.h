/*
 * LSST Data Management System
 * Copyright 2017 LSST Corporation.
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
#ifndef LSST_QSERV_REPLICA_REBALANCEJOB_H
#define LSST_QSERV_REPLICA_REBALANCEJOB_H

/// RebalanceJob.h declares:
///
/// struct RebalanceJobResult
/// class  RebalanceJob
///
/// (see individual class documentation for more information)

// System headers
#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <string>

// Qserv headers
#include "replica/Job.h"
#include "replica/FindAllJob.h"
#include "replica/ReplicaInfo.h"
#include "replica/MoveReplicaJob.h"

// This header declarations

namespace lsst {
namespace qserv {
namespace replica {

/**
 * The structure RebalanceJobResult represents a combined result received
 * from worker services upon a completion of the job.
 */
struct RebalanceJobResult {

    /// Results reported by workers upon the successfull completion
    /// of the new replica creation requests
    std::list<ReplicaInfo> createdReplicas;

    /// New replica creation results groupped by: chunk number, database, worker
    std::map<unsigned int,                  // chunk
             std::map<std::string,          // database
                      std::map<std::string, // destination worker
                               ReplicaInfo>>> createdChunks;

    /// Results reported by workers upon the successfull completion
    /// of the replica deletion requests
    std::list<ReplicaInfo> deletedReplicas;

    /// Replica deletion results groupped by: chunk number, database, worker
    std::map<unsigned int,                  // chunk
             std::map<std::string,          // database
                      std::map<std::string, // source worker
                               ReplicaInfo>>> deletedChunks;

    /// Per-worker flags indicating if the corresponidng replica retreival
    /// request succeeded.
    std::map<std::string, bool> workers;

    /// Replication plan
    ///
    /// ATTENTION: if the job is run in the 'estimateOnly' mode the plan and
    /// relevant variables defined after the plan are captured at the first (and only)
    /// iteration of the job. For the real rebalance regime these contain parameters
    /// of the last planning only.
    std::map<unsigned int,                  // chunk
             std::map<std::string,          // source worker
                      std::string>> plan;   // destination worker

    // Parameters of the planner

    size_t totalWorkers    {0};     // not counting workers which failed to report chunks
    size_t totalGoodChunks {0};     // good chunks reported by the precursor job
    size_t avgChunks       {0};     // per worker average
};

/**
  * Class RebalanceJob represents a tool which will rebalance replica disposition
  * accross worker nodes in order to achieve close-to-equal dstribution of chunks
  * across workers.
  *
  * These are basic requirements to the algorithm:
  *
  * - key metrics for the algorithm are:
  *     + a database family to be rebalanced
  *     + total number of replicas within a database family
  *     + the total number and names of workers which are available (up and running)
  *     + the average number of replicas per worker node
  *
  * - rebalance each database family independently of each other because
  *   this should still yeld an equal distribution of chunks accross any database
  *
  * - a subject of each move is (chunk,all databases of the family) residing
  *   on a node
  *
  * - the operation deals with 'good' (meaning 'colocated' and 'complete')
  *   chunk replicas only
  *
  * - the operation won't affect the number of replicas, it will only
  *   move replicas between workers
  *
  * - when rebalancing is over then investigate two options: finish it and launch
  *   it again externally using some sort of a scheduler, or have an internal ASYNC
  *   timer (based on Boost ASIO).
  *
  * - in the pilot implementation replica disposition should be requested directly
  *   from the worker nodes using precursor FindAllJob. More advanced implementation
  *   may switch to pulling this informtion from a database. That would work better
  *   at a presence of other activities keeping the database content updated.
  *
  * - [TO BE CONFIRMED] at a each iteration a limited number (from the Configuration?)
  *   of replicas will be processed. Then chunk disposition will be recomputed to adjust
  *   for other parallel activities (replication, purge, etc.).
  */
class RebalanceJob
    :   public Job  {

public:

    /// The pointer type for instances of the class
    typedef std::shared_ptr<RebalanceJob> Ptr;

    /// The function type for notifications on the completon of the request
    typedef std::function<void(Ptr)> CallbackType;

    /// @return default options object for this type of a request
    static Job::Options const& defaultOptions();

    /**
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * @param databaseFamily - the name of a database family
     * @param estimateOnly   - do not perform any changes to chunk disposition. Just produce an estimate report.
     * @param controller     - for launching requests
     * @param parentJobId    - optional identifier of a parent job
     * @param onFinish       - a callback function to be called upon a completion of the job
     * @param onFinish       - callback function to be called upon a completion of the job
     * @param options        - job options
     *
     * @return pointer to the created object
     */
    static Ptr create(std::string const& databaseFamily,
                      bool estimateOnly,
                      Controller::Ptr const& controller,
                      std::string const& parentJobId=std::string(),
                      CallbackType onFinish=nullptr,
                      Job::Options const& options=defaultOptions());

    // Default construction and copy semantics are prohibited

    RebalanceJob() = delete;
    RebalanceJob(RebalanceJob const&) = delete;
    RebalanceJob& operator=(RebalanceJob const&) = delete;

    ~RebalanceJob() = default;

    /// @return the name of a database defining a scope of the operation
    std::string const& databaseFamily() const { return _databaseFamily; }

    /// @return the estimate mode option
    bool estimateOnly() const { return _estimateOnly; }

    /**
     * Return the result of the operation.
     *
     * IMPORTANT NOTES:
     * - the method should be invoked only after the job has finished (primary
     *   status is set to Job::Status::FINISHED). Otherwise exception
     *   std::logic_error will be thrown
     *
     * - the result will be extracted from requests which have successfully
     *   finished. Please, verify the primary and extended status of the object
     *   to ensure that all requests have finished.
     *
     * @return the data structure to be filled upon the completin of the job.
     *
     * @throws std::logic_error - if the job dodn't finished at a time
     *                            when the method was called
     */
    RebalanceJobResult const& getReplicaData() const;

    /**
     * @see Job::extendedPersistentState()
     */
    std::list<std::pair<std::string,std::string>> extendedPersistentState() const override;

protected:

    /**
     * Construct the job with the pointer to the services provider.
     *
     * @see RebalanceJob::create()
     */
    RebalanceJob(std::string const& databaseFamily,
                 bool estimateOnly,
                 Controller::Ptr const& controller,
                 std::string const& parentJobId,
                 CallbackType onFinish,
                 Job::Options const& options);

    /**
      * @see Job::startImpl()
      */
    void startImpl(util::Lock const& lock) final;

    /**
      * @see Job::startImpl()
      */
    void cancelImpl(util::Lock const& lock) final;

    /**
      * @see Job::notify()
      */
    void notify(util::Lock const& lock) final;

    /**
     * The calback function to be invoked on a completion of the precursor job
     * which harvests chunk disposition accross relevant worker nodes.
     */
    void onPrecursorJobFinish();

    /**
     * The calback function to be invoked on a completion of each replica
     * creation request.
     *
     * @param request - a pointer to a request
     */
    void onJobFinish(MoveReplicaJob::Ptr const& job);

    /**
     * Submit a batch of the replica movement job
     *
     * This method implements a load balancing algorithm which tries to
     * prevent excessive use of resources by controllers and to avoid
     * "hot spots" or underutilization at workers.
     *
     * @param lock    - the lock must be acquired by a caller of the method
     * @param numJobs - desired number of jobs to submit
     *
     * @retun actual number of submitted jobs
     */
    size_t launchNextJobs(util::Lock const& lock,
                          size_t numJobs);

protected:

    /// The name of the database
    std::string _databaseFamily;

    /// Estimate mode option
    bool _estimateOnly;

    /// Client-defined function to be called upon the completion of the job
    CallbackType _onFinish;

    /// The chained job to be completed first in order to figure out
    /// replica disposition.
    FindAllJob::Ptr _findAllJob;

    /// Replica creation jobs which are ready to be launched
    std::list<MoveReplicaJob::Ptr> _jobs;

    /// Jobs which are already active
    std::list<MoveReplicaJob::Ptr> _activeJobs;

    // The counter of jobs which will be updated. They need to be atomic
    // to avoid race condition between the onFinish() callbacks executed within
    // the Controller's thread and this thread.

    std::atomic<size_t> _numLaunched;   ///< the total number of replica creation jobs launched
    std::atomic<size_t> _numFinished;   ///< the total number of finished jobs
    std::atomic<size_t> _numSuccess;    ///< the number of successfully completed jobs

    /// The result of the operation (gets updated as requests are finishing)
    RebalanceJobResult _replicaData;
};

}}} // namespace lsst::qserv::replica

#endif // LSST_QSERV_REPLICA_REBALANCEJOB_H
