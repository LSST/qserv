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
#ifndef LSST_QSERV_REPLICA_TASK_H
#define LSST_QSERV_REPLICA_TASK_H

// System headers
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/contr/Controller.h"
#include "replica/contr/EventLogger.h"
#include "replica/jobs/Job.h"
#include "replica/services/ServiceProvider.h"
#include "util/BlockPost.h"
#include "util/Issue.h"
#include "replica/util/Mutex.h"

// LSST headers
#include "lsst/log/Log.h"

// Forward declarations
namespace lsst::qserv::replica {
struct ControllerEvent;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class TaskError represents exceptions throw by the method
 * of class Task on various error conditions.
 */
class TaskError : public util::Issue {
public:
    TaskError(util::Issue::Context const& ctx, std::string const& message);
};

/**
 * Class TaskStopped represents exceptions throw by subclasses
 * (or method invoked by subclasses) when running subclass-specific
 * activities as a response to activity cancellation requests. Note, that
 * this kind of exception is not considered an error.
 */
class TaskStopped : public std::runtime_error {
public:
    TaskStopped() : std::runtime_error("task stopped") {}
};

/**
 * Class Task is the base class for the Controller-side activities run within
 * dedicated threads. The class provides a public interface for starting and stopping
 * the activities, notifying clients on abnormal termination of the activities,
 * as well as an infrastructure supporting an implementation of these activities.
 */
class Task : public EventLogger, public std::enable_shared_from_this<Task> {
public:
    /// The pointer type for instances of the class
    typedef std::shared_ptr<Task> Ptr;

    /// The function type for notifications on the abnormal termination of the tasks
    typedef std::function<void(Ptr)> AbnormalTerminationCallbackType;

    /// The function type for functions used in evaluating user-defined early-termination
    /// conditions for aborting task completion tracking
    typedef std::function<bool(Ptr)> WaitEvaluatorType;

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    virtual ~Task() = default;

    /// @return a reference to a provider of services
    ServiceProvider::Ptr const& serviceProvider() const { return _controller->serviceProvider(); }

    // @return 'true' if the task is running
    bool isRunning() const { return _isRunning.load(); }

    /**
     * Start a subclass-supplied sequence of actions (virtual method 'run')
     * within a new thread if it's not running. Note that the lifetime of
     * a task object is guaranteed to be extended for the duration of
     * the thread. This allows the thread to communicate with the object if needed.
     *
     * @return 'true' if the task was already running at a time when this
     *   method was called.
     */
    bool start();

    /**
     * Stop the task if it's still running.
     * @return 'true' if the task was already stopped at a time when this
     *   method was called.
     */
    bool stop();

    /**
     * Start the task (if it's not running yet) and then keep tracking
     * its status before it stops or before the optional early-termination
     * evaluator returns 'true'.
     *
     * @param abortWait (optional) this functional will be repeatedly (once a second)
     *   called while tracking the task's status.
     */
    bool startAndWait(WaitEvaluatorType const& abortWait = nullptr);

protected:
    /**
     * The constructor is available to subclasses only
     *
     * @param controller A reference to the Controller for launching requests, jobs, etc.
     * @param name The name of a task (used for logging info into the log stream,
     *   and for logging task events into the persistent log).
     * @param onTerminated A callback function to be called upon abnormal termination
     *   of the task.
     * @param waitIntervalSec The number of seconds to wait before calling subclass-specific
     *   method onRun.
     */
    Task(Controller::Ptr const& controller, std::string const& name,
         AbnormalTerminationCallbackType const& onTerminated, unsigned int waitIntervalSec);

    /// @return a shared pointer of the desired subclass (no dynamic type checking)
    template <class T>
    std::shared_ptr<T> shared_from_base() {
        return std::static_pointer_cast<T>(shared_from_this());
    }

    /**
     * This optional method implements subclass-specific sequence of actions
     * to be executed when the tasks starts running.
     *
     * @note Any but TaskStopped exceptions thrown by this method will
     *   be interpreted as abnormal termination of the task. Eventually this will
     *   also result in calling the 'onTerminated' callback if the one was provided
     *   to the constructor of the class.
     * @throws TaskStopped when the task cancellation request was detected.
     */
    virtual void onStart() {}

    /**
     * This optional method implements subclass-specific sequence of actions
     * to be run by the task.
     *
     * @note Any but TaskStopped exceptions thrown by this method will
     *   be interpreted as abnormal termination of the task. Eventually this will
     *   also result in calling the 'onTerminated' callback if the one was provided
     *   to the constructor of the class.
     * @return 'true' to schedule next invocation of the method after waiting
     *   for a interval configured in this class's constructor. Otherwise
     *   stop as if exception TaskStopped was thrown.
     * @throws TaskStopped when the task cancellation request was detected.
     */
    virtual bool onRun() { return false; }

    /**
     * This optional method implements subclass-specific sequence of actions
     * to be executed when the tasks stops running.
     */
    virtual void onStop() {}

    /// @return A flag indicating if the task needs to be stopped.
    bool stopRequested() const { return _stopRequested.load(); }

    /// @return The context string to be used when logging messages into a log stream.
    std::string context() const;

    /**
     * Log a message into the Logger's LOG_LVL_INFO stream.
     * @param msg A message to be logged
     */
    void info(std::string const& msg) { LOGS(_log, LOG_LVL_INFO, context() << msg); }

    /**
     * Log a message into the Logger's LOG_LVL_DEBUG stream.
     * @param msg A message to be logged.
     */
    void debug(std::string const& msg) { LOGS(_log, LOG_LVL_DEBUG, context() << msg); }

    /**
     * Log a message into the Logger's LOG_LVL_WARN stream.
     * @param msg A message to be logged.
     */
    void warn(std::string const& msg) { LOGS(_log, LOG_LVL_WARN, context() << msg); }

    /**
     * Log a message into the Logger's LOG_LVL_ERROR stream.
     * @param msg A message to be logged.
     */
    void error(std::string const& msg) { LOGS(_log, LOG_LVL_ERROR, context() << msg); }

    /**
     * Launch and track a job of the specified type per each known database
     * family. Note that parameters of the job are passed as variadic arguments
     * to the method.
     * @param priority The priority level of the job.
     * @param Fargs Job-specific variadic parameters.
     * @throws TaskStopped when the task cancellation request was detected
     */
    template <class T, typename... Targs>
    void launch(int priority, Targs... Fargs) {
        info(T::typeName());

        // Launch the jobs
        std::vector<typename T::Ptr> jobs;
        _numFinishedJobs = 0;
        std::string const parentJobId;  // no parent for these jobs
        for (auto&& family : serviceProvider()->config()->databaseFamilies()) {
            auto job = T::create(
                    family, Fargs..., controller(), parentJobId,
                    [self = shared_from_this()](typename T::Ptr const& job) {
                        self->_numFinishedJobs++;
                        // FIXME: analyze job status and report it here
                    },
                    priority);
            job->start();
            jobs.push_back(job);
            logJobStartedEvent(T::typeName(), job, job->databaseFamily());
        }

        // Track the completion of all jobs
        track<T>(T::typeName(), jobs, _numFinishedJobs);
        for (auto&& job : jobs) {
            logJobFinishedEvent(T::typeName(), job, job->databaseFamily());
        }
    }

    /**
     * Launch Qserv synchronization jobs.
     * @param qservSyncTimeoutSec The number of seconds to wait before a completion of
     *   the synchronization operation.
     * @param forceQservSync (optional) The flag to force Qserv synchronization if 'true'.
     * @throws TaskStopped when the task cancellation request was detected.
     * @see QservSyncJob
     */
    void sync(unsigned int qservSyncTimeoutSec, bool forceQservSync = false);

    /**
     * Track the completion of all jobs. Also monitor the task cancellation
     * condition while tracking the jobs. When such condition will be seen
     * all jobs will be canceled. The tracking will be done with an interval
     * of 1 second.
     *
     * @param typeName The name of a job.
     * @param jobs The collection of jobs to be tracked.
     * @param numFinishedJobs The counter of completed jobs.
     */
    template <class T>
    void track(std::string const& typeName, std::vector<typename T::Ptr> const& jobs,
               std::atomic<size_t> const& numFinishedJobs) {
        info(typeName + ": tracking started");
        util::BlockPost blockPost(1000, 1001);  // ~1 second wait time between iterations
        while (numFinishedJobs != jobs.size()) {
            if (stopRequested()) {
                for (auto&& job : jobs) {
                    job->cancel();
                }
                info(typeName + ": tracking aborted");
                throw TaskStopped();
            }
            blockPost.wait();
        }
        info(typeName + ": tracking finished");
    }

private:
    /// This method is launched by the task when it starts
    void _startImpl();

    /// The callback (if provided) to be called upon an abnormal termination
    /// of the user-supplied algorithm run in a context of the task.
    AbnormalTerminationCallbackType _onTerminated;

    unsigned int const _waitIntervalSec;   ///< The wait time before calling subclass's method onRun.
    std::atomic<bool> _isRunning;          ///< The flag indicating if it's already running.
    std::atomic<bool> _stopRequested;      ///< The flag to be raised when the task needs to be stopped.
    std::atomic<size_t> _numFinishedJobs;  ///< The thread-safe counter of the finished jobs.
    LOG_LOGGER _log;                       ///< Message logger
    replica::Mutex _mtx;                   ///< For guarding the object's state
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_TASK_H
