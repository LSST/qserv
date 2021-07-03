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
#ifndef LSST_QSERV_LOADER_CENTRALMASTER_H
#define LSST_QSERV_LOADER_CENTRALMASTER_H

// system headers
#include <thread>
#include <vector>

// third party headers
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// Qserv headers
#include "loader/Central.h"
#include "loader/MasterConfig.h"


namespace lsst {
namespace qserv {
namespace loader {

/// Central Master is the central element of the master. It maintains a DoList
/// and a list of all workers including their addresses, key ranges, and number of keys
/// on each worker. The authoritative ranges come from the workers. The ranges in the
/// master's list may be out of date but the system should handle it gracefully.
///
/// Workers register with the master when they start and are inactive until the master
/// gives them a valid range or a neighbor. The first worker activated has a range
/// covering all possible keys.
class CentralMaster : public Central {
public:
    /// Base class basic constructor, copy constructor, and operator= set to delete.
    CentralMaster(boost::asio::io_service& ioService_, std::string const& masterHostName_,
                  MasterConfig const& cfg)
        : Central(ioService_, masterHostName_, cfg.getMasterPort(),
                  cfg.getThreadPoolSize(), cfg.getLoopSleepTime(), cfg.getIOThreads()),
          _maxKeysPerWorker(cfg.getMaxKeysPerWorker()) {}

    /// Open the UDP port. This can throw boost::system::system_error.
    void start();

    ~CentralMaster() override { _mWorkerList.reset(); }

    void setMaxKeysPerWorker(int val) { _maxKeysPerWorker = val; }
    int getMaxKeysPerWorker() const { return _maxKeysPerWorker; }

    void addWorker(std::string const& ip, int udpPort, int tcpPort); ///< Add a new worker to the system.
    void updateWorkerInfo(uint32_t workerId, NeighborsInfo const& nInfo, KeyRange const& strRange);

    MWorkerListItem::Ptr getWorkerWithId(uint32_t id);

    MWorkerList::Ptr getWorkerList() const { return _mWorkerList; }

    void reqWorkerKeysInfo(uint64_t msgId, std::string const& targetIp, short targetPort,
                           std::string const& ourHostName, short ourPort);

    std::string getOurLogId() const override { return "master"; }

    void setWorkerNeighbor(MWorkerListItem::WPtr const& target, int message, uint32_t neighborId);

private:
    /// Upon receiving new worker information, check if an inactive worker should be made active.
    void _assignNeighborIfNeeded(uint32_t workerId, MWorkerListItem::Ptr const& wItem);

    std::mutex _assignMtx; ///< Protects critical region where worker's can be set to active.

    std::atomic<int> _maxKeysPerWorker{1000}; // TODO load from config file.

    MWorkerList::Ptr _mWorkerList{std::make_shared<MWorkerList>(this)}; ///< List of workers.

    std::atomic<bool> _firstWorkerRegistered{false}; ///< True when one worker has been activated.

    /// The id of the worker being added. '0' indicates no worker being added.
    /// Its value can only be set to non-zero values within _assignNeighborIfNeeded(...).
    std::atomic<uint32_t> _addingWorkerId{0}; // TODO maybe move _addingWorkerId to MWorkerList.
};

}}} // namespace lsst::qserv::loader


#endif // LSST_QSERV_LOADER_CENTRALMASTER_H
