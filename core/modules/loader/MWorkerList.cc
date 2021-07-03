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
#include "loader/MWorkerList.h"

// System headers
#include <boost/asio.hpp>
#include <iostream>

// Third-party headers


// qserv headers
#include "loader/CentralMaster.h"
#include "loader/LoaderMsg.h"
#include "proto/ProtoImporter.h"
#include "proto/loader.pb.h"


// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.MWorkerList");
}

namespace lsst {
namespace qserv {
namespace loader {

/// Create commands to set a worker's neighbor.
/// It should keep trying this until it works. When the worker sets the neighbor to
/// the target value, this object should initiate a chain reaction that destroys itself.
/// It is very important that the message and neighborPtr both point to
/// the same (left or right) neighbor.
class SetNeighborOneShot : public DoListItem, public UpdateNotify<uint32_t> {
public:
    using Ptr = std::shared_ptr<SetNeighborOneShot>;

    static Ptr create(CentralMaster* central_,
               MWorkerListItem::Ptr const& target_,
               int msg_,
               uint32_t neighborId_,
               NeighborsInfo::NeighborPtr const& neighborPtr_) {
        Ptr oneShot(new SetNeighborOneShot(central_, target_, msg_, neighborId_,  neighborPtr_));
        auto oneShotPtr = std::static_pointer_cast<SetNeighborOneShot>(oneShot->getDoListItemPtr());
        auto updatePtr = std::static_pointer_cast<UpdateNotify<uint32_t>>(oneShotPtr);
        neighborPtr_->registerNotify(updatePtr); // Must do this so it will call our updateNotify().
        LOGS(_log, LOG_LVL_INFO, "SetNeighborOneShot neighborId=" <<
                                 neighborId_ << " " << neighborPtr_->get());
        return oneShot;
    }

    util::CommandTracked::Ptr createCommand() override;

    // This is called every time the worker sends the master a value for its (left/right) neighbor.
    // See neighborPtr_->registerNotify()
    void updateNotify(uint32_t& oldVal, uint32_t& newVal) override {
        if (newVal == neighborId) {
            infoReceived(); // This should result in this oneShot DoListItem being removed->destroyed.
        }
    }

    CentralMaster* const central;
    MWorkerListItem::WPtr target;
    int const message;
    uint32_t const neighborId;
    NeighborsInfo::NeighborWPtr neighborPtr;
private:
    SetNeighborOneShot(CentralMaster* central_,
                       MWorkerListItem::Ptr const& target_,
                       int msg_,
                       uint32_t neighborId_,
                       NeighborsInfo::NeighborPtr const& neighborPtr_) :
                    central(central_), target(target_), message(msg_), neighborId(neighborId_),
                    neighborPtr(neighborPtr_) {
        setOneShot(true);
    }
};



util::CommandTracked::Ptr SetNeighborOneShot::createCommand() {
    struct SetNeighborCmd : public util::CommandTracked {
        SetNeighborCmd(SetNeighborOneShot::Ptr const& ptr) : oneShotData(ptr) {}
        void action(util::CmdData*) override {
            auto oSData = std::dynamic_pointer_cast<SetNeighborOneShot>(oneShotData.lock());
            if (oSData != nullptr) {
                oSData->central->setWorkerNeighbor(oSData->target, oSData->message, oSData->neighborId);
            }
        }
        std::weak_ptr<SetNeighborOneShot> oneShotData;
    };
    auto ptr = std::dynamic_pointer_cast<SetNeighborOneShot>(getDoListItemPtr());
    return std::make_shared<SetNeighborCmd>(ptr);
}


util::CommandTracked::Ptr MWorkerList::createCommand() {
    return createCommandMaster(_central);
}


util::CommandTracked::Ptr MWorkerList::createCommandMaster(CentralMaster* centralM) {
    // The master probably doesn't need to make any checks on the list.
    return nullptr;
}


// Returns pointer to new item when new worker added, otherwise nullptr.
MWorkerListItem::Ptr MWorkerList::addWorker(std::string const& ip, int udpPort, int tcpPort) {
    NetworkAddress udpAddress(ip, udpPort);
    NetworkAddress tcpAddress(ip, tcpPort);


    // If it is already in the map, do not change its id.
    std::lock_guard<std::mutex> lock(_mapMtx);
    auto iter = _ipMap.find(udpAddress);
    if (iter != _ipMap.end()) {
        LOGS(_log, LOG_LVL_WARN, "addWorker, Could not add worker as worker already exists. " <<
                ip << ":" << udpPort);
        return nullptr;
    }
    // Get an id and make new worker item
    auto workerListItem = MWorkerListItem::create(_sequenceId++, udpAddress, tcpAddress, _central);
    _ipMap.insert(std::make_pair(udpAddress, workerListItem));
    _wIdMap.insert(std::make_pair(workerListItem->getId(), workerListItem));
    LOGS(_log, LOG_LVL_INFO, "Added worker " << *workerListItem);
    _flagListChange();

    return workerListItem;
}

bool MWorkerList::sendListTo(uint64_t msgId, std::string const& ip, short port,
                            std::string const& ourHostName, short ourPort) {
    NetworkAddress address(ip, port);
    StringElement workerList;
    {
        std::lock_guard<std::mutex> lockStatList(_statListMtx);
        {
            std::lock_guard<std::mutex> lockMap(_mapMtx);
            if (_wListChanged || _stateListData == nullptr) {
                _wListChanged = false;
                /// At this time, all workers should easily fit in a single message.
                /// TODO send multiple messages (if needed) with each having the address and
                ///      range of 100 workers.
                ///      This version is useful for testing. _stateListData becomes a vector.
                proto::LdrMastWorkerList protoList;
                protoList.set_workercount(_wIdMap.size());
                for (auto const& item : _wIdMap ) {
                    proto::WorkerListItem* protoItem = protoList.add_worker();
                    MWorkerListItem::Ptr const& wListItem = item.second;
                    protoItem->set_wid(wListItem->getId());
                }
                protoList.SerializeToString(&(workerList.element));
                LoaderMsg workerListMsg(LoaderMsg::MAST_WORKER_LIST, msgId, ourHostName, ourPort);
                _stateListData = std::make_shared<BufferUdp>();
                workerListMsg.appendToData(*_stateListData);
                workerList.appendToData(*_stateListData);
            }
        }
        try {
            _central->sendBufferTo(ip, port, *_stateListData);
        } catch (boost::system::system_error const& e) {
            LOGS(_log, LOG_LVL_ERROR, "MWorkerList::sendListTo boost system_error=" << e.what() <<
                    " msgId=" << msgId << " ip=" << ip << " port=" << port <<
                    " ourName=" << ourHostName << " ourPort=" << ourPort);
        }
    }

    // See if this worker is know.
    MWorkerListItem::Ptr workerItem;
    {
        // See if this is a worker in our map
        std::lock_guard<std::mutex> lock(_mapMtx);
        auto iter = _ipMap.find(address);
        if (iter != _ipMap.end()) {
            workerItem = iter->second;
        }
    }
    if (workerItem != nullptr) {
        workerItem->sendListToWorkerInfoReceived();
    }
    return true;
}




std::pair<std::vector<MWorkerListItem::Ptr>, std::vector<MWorkerListItem::Ptr>>
MWorkerList::getActiveInactiveWorkerLists() {
    std::vector<MWorkerListItem::Ptr> active;
    std::vector<MWorkerListItem::Ptr> inactive;
    std::lock_guard<std::mutex> lck(_mapMtx);
    for(auto const& elem : _wIdMap) {
        auto item = elem.second;
        if (item->isActive()) {
            active.push_back(item);
        } else {
            inactive.push_back(item);
        }
    }
    auto pair = std::make_pair(active, inactive);
    return pair;
}


// must lock _mapMtx before calling this function
void MWorkerList::_flagListChange() {
    _wListChanged = true;
    // On the Master, flag each worker in the list that it needs to send an updated list to it's worker.
    for (auto const& elem : _wIdMap) {
        auto const& item = elem.second;
        item->flagNeedToSendList();
    }
}


std::string MWorkerList::dump() const {
    std::stringstream os;
    os << "MWorkerList:\n";
    {
        std::lock_guard<std::mutex> lck(_mapMtx);
        for (auto elem:_wIdMap) {
            os << "  " << *elem.second << "\n";
        }
        os << "MWorkerList ip:\n";
        for (auto elem:_ipMap) {
            os << "  " << *elem.second << "\n";
        }
    }
    return os.str();
}


void MWorkerListItem::addDoListItems(Central *central) {
    LOGS(_log, LOG_LVL_DEBUG, "MWorkerListItem::addDoListItems");
    std::lock_guard<std::mutex> lck(_doListItemsMtx);
    if (_sendListToWorker == nullptr) {
        _sendListToWorker = std::make_shared<SendListToWorker>(getThis(), _central);
        _central->addDoListItem(_sendListToWorker);
    }
    if (_reqWorkerKeyInfo == nullptr) {
        _reqWorkerKeyInfo = std::make_shared<ReqWorkerKeyInfo>(getThis(), _central);
        _central->addDoListItem(_reqWorkerKeyInfo);
    }
}


void MWorkerListItem::flagNeedToSendList() {
    auto slw = _sendListToWorker;
    if (slw != nullptr) { slw->setNeedInfo(); }
}


void MWorkerListItem::sendListToWorkerInfoReceived() {
    auto slw = _sendListToWorker;
    if (slw != nullptr) {
        // _sendListToWorker is a tough one to tell if the worker got the info, so
        // it is assumed that this worked when the list is sent. The worker
        // will either ask for it or it will be sent again later.
        // TODO find a reasonable way to tell that the worker got the list.
        slw->infoReceived();
    }
}


void MWorkerListItem::setAllInclusiveRange() {
    LOGS(_log, LOG_LVL_INFO, "MWorkerListItem::setAllInclusiveRange for wId=" << _wId);
    std::lock_guard<std::mutex> lck(_mtx);
    _range.setAllInclusiveRange();
    _active = true;  /// First worker.
}


void MWorkerListItem::setNeighborsInfo(NeighborsInfo const& nInfo) {
    std::lock_guard<std::mutex> lck(_mtx);
    _neighborsInfo.keyCount = nInfo.keyCount;
    _neighborsInfo.recentAdds = nInfo.recentAdds;

    auto old = _neighborsInfo.neighborLeft->get();
    if (old != 0 && old != nInfo.neighborLeft->get()) {
        LOGS(_log, LOG_LVL_WARN, "Worker=" << _wId <<
             "neighborLeft changing from valid old=" << old <<
             " to new=" << nInfo.neighborLeft->get());
    }
    if (old != nInfo.neighborLeft->get()) {
        LOGS(_log, LOG_LVL_INFO, "Worker=" << _wId <<
             "neighborLeft=" << nInfo.neighborLeft->get());
    }
    _neighborsInfo.neighborLeft->update(nInfo.neighborLeft->get());

    old = _neighborsInfo.neighborRight->get();
    if (old != 0) {
        LOGS(_log, LOG_LVL_WARN, "Worker=" << _wId <<
                "neighborRight changing from valid old=" << old <<
                " to new=" << nInfo.neighborRight->get());
    }
    if (old != nInfo.neighborRight->get()) {

        LOGS(_log, LOG_LVL_INFO, "Worker=" << _wId <<
                "neighborRight=" << nInfo.neighborRight->get());
    }
    _neighborsInfo.neighborRight->update(nInfo.neighborRight->get());
}


int MWorkerListItem::getKeyCount() const {
    return _neighborsInfo.keyCount;
}


std::ostream& MWorkerListItem::dump(std::ostream& os) const {
    WorkerListItemBase::dump(os); // call base class version
    os << " active=" << _active;
    return os;
}


/// Set this worker's RIGHT neighbor to the worker described in 'item'.
void MWorkerListItem::setRightNeighbor(MWorkerListItem::Ptr const& item) {
    // Create a one shot to send a message to the worker.
    // It knows it has worked when the worker sends a message back saying it
    // has the correct right neighbor.
    LOGS(_log, LOG_LVL_DEBUG," MWorkerListItem::setRightNeighbor");

    auto oneShot = SetNeighborOneShot::create(_central,
                                              getThis(),
                                              LoaderMsg::WORKER_RIGHT_NEIGHBOR,
                                              item->getId(),
                                              _neighborsInfo.neighborRight);
    _central->addDoListItem(oneShot);
}


// TODO very similar to MWorkerListItem::setRightNeighbor, consider merging.
void MWorkerListItem::setLeftNeighbor(MWorkerListItem::Ptr const& item) {
    // Create a one shot to send a message to the worker.
    // It knows it has worked when the worker sends a message back saying it
    // has the correct left neighbor.
    LOGS(_log, LOG_LVL_DEBUG,"MWorkerListItem::setLeftNeighbor");

    auto oneShot = SetNeighborOneShot::create(_central,
                                              getThis(),
                                              LoaderMsg::WORKER_LEFT_NEIGHBOR,
                                              item->getId(),
                                              _neighborsInfo.neighborLeft);

    _central->addDoListItem(oneShot);
}


util::CommandTracked::Ptr MWorkerListItem::SendListToWorker::createCommand() {
    auto item = mWorkerListItem.lock();
    if (item == nullptr) {
        // TODO: should mark set the removal flag for this doListItem
        return nullptr;
    }

    struct SendListToWorkerCmd : public util::CommandTracked {
        SendListToWorkerCmd(CentralMaster *centM_, MWorkerListItem::Ptr const& tItem_) : centM(centM_), tItem(tItem_) {}
        void action(util::CmdData*) override {
            LOGS(_log, LOG_LVL_DEBUG, "SendListToWorkerCmd::action");
            auto udp = tItem->getUdpAddress();
            centM->getWorkerList()->sendListTo(centM->getNextMsgId(),
                   udp.ip, udp.port,
                   centM->getMasterHostName(), centM->getMasterPort());
        }
        CentralMaster *centM;
        MWorkerListItem::Ptr tItem;
    };
    LOGS(_log, LOG_LVL_DEBUG, "SendListToWorker::createCommand");
    return std::make_shared<SendListToWorkerCmd>(central, item);
}



util::CommandTracked::Ptr MWorkerListItem::ReqWorkerKeyInfo::createCommand() {
    auto item = mWorkerListItem.lock();
    if (item == nullptr) {
        // TODO: should mark set the removal flag for this doListItem
        return nullptr;
    }

    struct ReqWorkerKeysInfoCmd : public util::CommandTracked {
        ReqWorkerKeysInfoCmd(CentralMaster *centM_, MWorkerListItem::Ptr const& tItem_) : centM(centM_), tItem(tItem_) {}
        void action(util::CmdData*) override {
            LOGS(_log, LOG_LVL_DEBUG, "ReqWorkerKeyInfoCmd::action");
            auto udp = tItem->getUdpAddress();
            centM->reqWorkerKeysInfo(centM->getNextMsgId(),
                    udp.ip, udp.port,
                    centM->getMasterHostName(), centM->getMasterPort());
        }
        CentralMaster *centM;
        MWorkerListItem::Ptr tItem;
    };
    LOGS(_log, LOG_LVL_DEBUG, "SendListToWorker::createCommand");
    return std::make_shared<ReqWorkerKeysInfoCmd>(central, item);
}


}}} // namespace lsst::qserv::loader






