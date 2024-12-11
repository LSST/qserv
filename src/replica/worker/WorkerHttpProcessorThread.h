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
#ifndef LSST_QSERV_REPLICA_WORKERHTTPPROCESSORTHREAD_H
#define LSST_QSERV_REPLICA_WORKERHTTPPROCESSORTHREAD_H

// System headers
#include <atomic>
#include <memory>
#include <string>
#include <thread>

// Forward declarations
namespace lsst::qserv::replica {
class WorkerHttpProcessor;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class WorkerHttpProcessorThread is a thread-based request processing engine
 * for replication requests within worker-side services.
 */
class WorkerHttpProcessorThread : public std::enable_shared_from_this<WorkerHttpProcessorThread> {
public:
    /**
     * Static factory method is needed to prevent issue with the lifespan
     * and memory management of instances created otherwise (as values or via
     * low-level pointers).
     *
     * @param processor A pointer to the processor which launched this thread. This pointer
     *  will be used for making call backs to the processor on the completed or rejected requests.
     * @return a pointer to the created object
     */
    static std::shared_ptr<WorkerHttpProcessorThread> create(
            std::shared_ptr<WorkerHttpProcessor> const& processor);

    WorkerHttpProcessorThread() = delete;
    WorkerHttpProcessorThread(WorkerHttpProcessorThread const&) = delete;
    WorkerHttpProcessorThread& operator=(WorkerHttpProcessorThread const&) = delete;

    ~WorkerHttpProcessorThread() = default;

    /// @return identifier of this thread object
    unsigned int id() const { return _id; }

    /// @return 'true' if the processing thread is still running
    bool isRunning() const;

    /**
     * Create and run the thread (if none is still running) fetching
     * and processing requests until method stop() is called.
     */
    void run();

    /**
     * Tell the running thread to abort processing the current
     * request (if any), put that request back into the input queue,
     * stop fetching new requests and finish. The thread can be resumed
     * later by calling method run().
     *
     * @note This is an asynchronous operation.
     */
    void stop();

    /// @return context string for logs
    std::string context() const { return "THREAD: " + std::to_string(_id) + "  "; }

private:
    /// @see WorkerHttpProcessorThread::create()
    WorkerHttpProcessorThread(std::shared_ptr<WorkerHttpProcessor> const& processor, unsigned int id);

    /**
     * Event handler called by the thread when it's about to stop
     */
    void _stopped();

    // Input parameters

    std::shared_ptr<WorkerHttpProcessor> const _processor;

    /// The identifier of this thread object
    unsigned int const _id;

    /// The processing thread is created on demand when calling method run()
    std::unique_ptr<std::thread> _thread;

    /// The flag to be raised to tell the running thread to stop.
    /// The thread will reset this flag when it finishes.
    std::atomic<bool> _stop;
};

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_WORKERHTTPPROCESSORTHREAD_H
