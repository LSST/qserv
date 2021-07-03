// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
 *
 */
#ifndef LSST_QSERV_LOADER_CENTRAL_H
#define LSST_QSERV_LOADER_CENTRAL_H

// system headers
#include <thread>
#include <vector>

// third party headers
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// Qserv headers
#include "loader/ClientServer.h"
#include "loader/MasterServer.h"
#include "loader/MWorkerList.h"
#include "loader/WorkerServer.h"
#include "loader/WWorkerList.h"
#include "proto/loader.pb.h"
#include "util/ThreadPool.h"


namespace lsst {
namespace qserv {
namespace loader {

/// TODO add fileId and row to this so it can be checked in _workerKeyInsertReq
struct ChunkSubchunk {
    ChunkSubchunk(int chunk_, int subchunk_) : chunk(chunk_), subchunk(subchunk_) {}
    int const chunk;
    int const subchunk;
    friend std::ostream& operator<<(std::ostream& os, ChunkSubchunk const& csc);
};


/// This class is 'central' to the execution of the program, and must be around
/// until the bitter end. As such, it can be accessed by normal pointers.
/// This class is central to loader workers and the master.
/// It is the base class for CentralWorker, CentralMaster, and CentralClient.
/// It provides a DoList and a means to contact the master. The master
/// needs to know its own address.
class Central {
public:
    Central() = delete;
    Central(Central const&) = delete;
    Central& operator=(Central const&) = delete;

    virtual ~Central();

    std::string getMasterHostName() const { return _masterAddr.ip; }
    int getMasterPort() const { return _masterAddr.port; }
    NetworkAddress getMasterAddr() const { return _masterAddr; }

    uint64_t getNextMsgId() { return _sequence++; }

    int getErrCount() const { return _server->getErrCount(); }

    /// Send the contents of 'sendBuf' to 'host:port'. This waits for the message to be
    /// sent before returning.
    /// @throw boost::system::system_error on failure.
    void sendBufferTo(std::string const& host, int port, BufferUdp& sendBuf) {
        _server->sendBufferTo(host, port, sendBuf);
    }

    /// Only allow tracked commands on the queue. The DoList has to able to tell
    /// if a Command completed.
    void queueCmd(util::CommandTracked::Ptr const& cmd) {
        _queue->queCmd(cmd);
    }

    /// Add a DoListItem to the _doList which will run and
    /// rerun the item until it is no longer needed.
    bool addDoListItem(DoListItem::Ptr const& item) {
        return doList->addItem(item);
    }

    /// Run the item immediately before adding it to _doList.
    bool runAndAddDoListItem(DoListItem::Ptr const& item) {
        doList->runItemNow(item);
        return doList->addItem(item);
    }

    /// Run the server.
    void runServer() {
        for (; _runningIOThreads < _iOThreads; ++_runningIOThreads) {
            run();
        }
    }

    /// Provides a method for identifying different Central classes and
    /// CentralWorkers in the log file.
    virtual std::string getOurLogId() const { return "Central baseclass"; }

protected:
    Central(boost::asio::io_service& ioService_,
                std::string const& masterHostName, int masterPort,
                int threadPoolSize, int loopSleepTime,
                int iOThreads)
                : ioService(ioService_), _masterAddr(masterHostName, masterPort),
                  _threadPoolSize(threadPoolSize), _loopSleepTime(loopSleepTime),
                  _iOThreads(iOThreads) {
        _initialize();
    }

    void run(); ///< Run a single asio thread.

    boost::asio::io_service& ioService;

    DoList::Ptr doList; ///< List of items to be checked at regular intervals.

    ServerUdpBase::Ptr _server;

private:
    /// Repeatedly check the items on the _doList.
    void _checkDoList();

    void _initialize();///< Finish construction.

    NetworkAddress _masterAddr; ///< Network address of the master node.

    std::atomic<uint64_t> _sequence{1};

    util::CommandQueue::Ptr _queue{std::make_shared<util::CommandQueue>()}; // Must be defined before _pool

    int _threadPoolSize{10}; ///< Number of threads _pool.
    util::ThreadPool::Ptr _pool; ///< Thread pool.

    bool _loop{true}; ///< continue looping through _checkDolList() while this is true.
    int _loopSleepTime{100000}; ///< microseconds to sleep between each check of all list items.

    std::vector<std::thread> _ioServiceThreads; ///< List of asio io threads created by this

    std::thread _checkDoListThread; ///< Thread for running doList checks on DoListItems.

    int _iOThreads{5}; ///< Number of asio IO threads to run, set by config file.
    int _runningIOThreads{0}; ///< Number of asio IO threads started.
};

}}} // namespace lsst::qserv::loader


#endif // LSST_QSERV_LOADER_CENTRAL_H
