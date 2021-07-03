// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 AURA/LSST.
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
#include "loader/CentralMaster.h"

// system headers
#include <iostream>

// Third-party headers
#include <boost/asio.hpp>

// LSST headers
#include "lsst/log/Log.h"


namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.CentralMaster");
}

namespace lsst {
namespace qserv {
namespace loader {


void CentralMaster::start() {
    _server = std::make_shared<MasterServer>(ioService, getMasterHostName(), getMasterPort(), this);
}


void CentralMaster::addWorker(std::string const& ip, int udpPort, int tcpPort) {
    auto item = _mWorkerList->addWorker(ip, udpPort, tcpPort);

    if (item != nullptr) {
        // If that was the first worker added, it gets unlimited range.
        if (_firstWorkerRegistered.exchange(true) == false) {
            LOGS(_log, LOG_LVL_INFO, "setAllInclusiveRange for name=" << item->getId());
            item->setAllInclusiveRange();
        }

        item->addDoListItems(this);
        LOGS(_log, LOG_LVL_INFO, "Master::addWorker " << *item);
    }
}


void CentralMaster::updateWorkerInfo(uint32_t workerId, NeighborsInfo const& nInfo, KeyRange const& strRange) {
    if (workerId == 0) {
        return;
    }
    auto item = getWorkerWithId(workerId);
    if (item == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralMaster::updateNeighbors nullptr for workerId=" << workerId);
        return;
    }
    // TODO setting nInfo and strRange can be done in one call to reduce mutex locking.
    item->setNeighborsInfo(nInfo);
    item->setRangeString(strRange);
    _assignNeighborIfNeeded(workerId, item);
}


void CentralMaster::setWorkerNeighbor(MWorkerListItem::WPtr const& target, int message, uint32_t neighborId) {
    // Get the target worker's network address
    auto targetWorker = target.lock();
    if (targetWorker == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralMaster::setWorkerNeighbor nullptr for " << neighborId);
        return;
    }

    LOGS(_log, LOG_LVL_DEBUG, "CentralMaster::setWorkerNeighbor " << neighborId << " " << *targetWorker);
    // Build and send the message
    LoaderMsg msg(message, getNextMsgId(), getMasterHostName(), getMasterPort());
    BufferUdp msgData;
    msg.appendToData(msgData);
    UInt32Element neighborIdElem(neighborId);
    neighborIdElem.appendToData(msgData);
    auto addr = targetWorker->getUdpAddress();
    try {
        sendBufferTo(addr.ip, addr.port, msgData);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralMaster::setWorkerNeighbor boost system_error=" << e.what() <<
                                  " targ=" << *targetWorker << " msg=" << message <<
                                  " neighborId=" << neighborId);
    }
}


void CentralMaster::_assignNeighborIfNeeded(uint32_t workerId, MWorkerListItem::Ptr const& wItem) {
    // Go through the list and see if all the workers are full.
    // If they are, assign a worker to the end (rightmost) worker
    // and increase the maximum by an order of magnitude, max 10 million.
    // TODO Make a better algorithm, insert workers at busiest worker.
    std::string funcName("_assignNeighborIfNeeded");
    LOGS(_log, LOG_LVL_DEBUG, funcName);
    if (_addingWorkerId != 0 && _addingWorkerId != workerId) {
        // Already in process of adding a worker, and the worker
        // with new information wasn't the one added. Nothing to do.
        // TODO Check if it failed. (May need to go in a timer thread instead)
        return;
    }
    // Only one thread should ever be working on this logic at a time.
    std::lock_guard<std::mutex> lck(_assignMtx);
    if (_addingWorkerId != 0) {
        if (_addingWorkerId == workerId) {
            auto rng = wItem->getRangeString();
            if (rng.getValid()) {
                wItem->setActive(true);
                LOGS(_log, LOG_LVL_INFO, "Successfully activated wId=" << workerId <<
                        " range=" << rng);
                _addingWorkerId = 0;
            }
        }
    }

    auto pair = _mWorkerList->getActiveInactiveWorkerLists();
    std::vector<MWorkerListItem::Ptr>& activeList = pair.first;
    std::vector<MWorkerListItem::Ptr>& inactiveList = pair.second;
    if (inactiveList.empty() || _addingWorkerId != 0) { return; }
    double sum = 0.0;
    int max = 0;
    uint32_t maxWId = 0;
    uint32_t rightMostName = 0; // Name of the rightmost worker, unlimited upper range.
    MWorkerListItem::Ptr rightMostItem;
    for(auto& item : activeList) {
        int keyCount = item->getKeyCount();
        sum += keyCount;
        if (keyCount > max) {
            max = keyCount;
            maxWId = item->getId();
        }
        auto range = item->getRangeString();
        if (range.getValid() && range.getUnlimited()) {
            if (rightMostName != 0) {
                std::string eStr("Multiple rightMost workers name=");
                eStr += rightMostName +  " name=" + item->getId();
                LOGS(_log, LOG_LVL_ERROR, "_assignNeighborIfNeeded " << eStr);
                throw LoaderMsgErr(ERR_LOC, eStr);
            }
            rightMostName = item->getId();
            rightMostItem = item;
        }
    }
    if (rightMostItem == nullptr) {
        LOGS(_log, LOG_LVL_WARN, funcName << " no rightmost worker found when expected.");
        return;
    }
    double avg = sum/(double)(activeList.size());
    LOGS(_log, LOG_LVL_INFO, "max=" << max << " maxWId=" << maxWId << " avg=" << avg);
    if (avg > getMaxKeysPerWorker()) {
        // Assign a neighbor to the rightmost worker, if there are any unused nodes.
        // TODO Probably better to assign a new neighbor next to the node with the most recent activity.
        //      but that's much more complicated.
        LOGS(_log, LOG_LVL_INFO, "ADDING WORKER avg=" << avg);
        auto inactiveItem = inactiveList.front();
        if (inactiveItem == nullptr) {
            throw LoaderMsgErr(ERR_LOC,"_assignNeighborIfNeeded unexpected inactiveList nullptr");
        }
        _addingWorkerId = inactiveItem->getId();
        // Sequence of events goes something like
        // 1) left item gets message from master that it is getting a right neighbor, writes it down.
        // 2) Right item get message from master that it is getting a left neighbor, writes it down.
        // 3) CentralWorker::_monitor() on the left node(rightmostItem) connects to the right
        //    node(inactiveItem), ranges are setup and shifts are started.
        // 4) When message received from the new worker saying that it has a valid range,
        //    set _addingWorkerId to 0. This check happens earlier in this function.
        //
        // Steps 1 and 2
        rightMostItem->setRightNeighbor(inactiveItem);
        inactiveItem->setLeftNeighbor(rightMostItem);
    }
}

MWorkerListItem::Ptr CentralMaster::getWorkerWithId(uint32_t id) {
    return _mWorkerList->getWorkerWithId(id);
}


void CentralMaster::reqWorkerKeysInfo(uint64_t msgId, std::string const& targetIp, short targetPort,
                                      std::string const& ourHostName, short ourPort) {
    LoaderMsg reqMsg(LoaderMsg::WORKER_KEYS_INFO_REQ, msgId, ourHostName, ourPort);
    BufferUdp data;
    reqMsg.appendToData(data);
    try {
        sendBufferTo(targetIp, targetPort, data);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralMaster::reqWorkerKeysInfo boost system_error=" << e.what() <<
                " msgId=" << msgId << " tIp=" << targetIp << " tPort=" << targetPort <<
                " ourHost=" << ourHostName << " ourPort=" << ourPort);
    }
}

}}} // namespace lsst::qserv::loader
