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
#ifndef LSST_QSERV_REPLICATIONTASK_H
#define LSST_QSERV_REPLICATIONTASK_H

// Qserv headers
#include "replica/contr/Task.h"

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class ReplicationTask runs a sequence of jobs at each iteration
 * of the Master Controller's replication loop.
 */
class ReplicationTask : public Task {
public:
    /// The pointer type for instances of the class
    typedef std::shared_ptr<ReplicationTask> Ptr;

    ReplicationTask() = delete;
    ReplicationTask(ReplicationTask const&) = delete;
    ReplicationTask& operator=(ReplicationTask const&) = delete;

    ~ReplicationTask() final = default;

    /**
     * Create a new task with specified parameters.
     *
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * @param controller A reference to the Controller for launching requests, jobs, etc.
     * @param onTerminated A callback function to be called upon abnormal termination
     *   of the task. Set it to 'nullptr' if no call back should be made.
     * @param qservSyncTimeoutSec The maximum number of seconds to be waited before giving
     *   up on the Qserv synchronization requests.
     * @param disableQservSync Disable replica synchronization at Qserv workers if 'true'.
     * @param forceQservSync Force chunk removal at worker resource collections if 'true'.
     * @param qservChunkMapUpdate Update the chunk disposition map in Qserv's QMeta database if 'true'.
     * @param replicationIntervalSec The number of seconds to wait in the end of each
     *   iteration loop before to begin the new one.
     * @param purge Purge excess replicas if 'true'.
     * @return The smart pointer to a new object
     */
    static Ptr create(Controller::Ptr const& controller,
                      Task::AbnormalTerminationCallbackType const& onTerminated,
                      unsigned int qservSyncTimeoutSec, bool disableQservSync, bool forceQservSync,
                      bool qservChunkMapUpdate, unsigned int replicationIntervalSec, bool purge);

protected:
    /// @see Task::onRun()
    virtual bool onRun() final;

private:
    /// @see ReplicationTask::create()
    ReplicationTask(Controller::Ptr const& controller, AbnormalTerminationCallbackType const& onTerminated,
                    unsigned int qservSyncTimeoutSec, bool disableQservSync, bool forceQservSync,
                    bool qservChunkMapUpdate, unsigned int replicationIntervalSec, bool purge);

    void _updateChunkMap();

    /// The maximum number of seconds to be waited before giving up
    /// on the Qserv synchronization requests.
    unsigned int const _qservSyncTimeoutSec;

    bool const _disableQservSync;  ///< Disable replica synchroization at Qserv workers if 'true'.
    bool const _forceQservSync;    ///< Force removal at worker resource collections if 'true'.
    bool const
            _qservChunkMapUpdate;  /// Update the chunk disposition map in Qserv's QMeta database if 'true'.
    bool const _purge;             ///< Purge excess replicas if 'true'.
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICATIONTASK_H
