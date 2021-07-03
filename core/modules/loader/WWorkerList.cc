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
#include "loader/WWorkerList.h"

// System headers
#include <boost/asio.hpp>
#include <iostream>

// Third-party headers


// qserv headers
#include "loader/CentralWorker.h"
#include "loader/LoaderMsg.h"
#include "proto/ProtoImporter.h"
#include "proto/loader.pb.h"


// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.WWorkerList");
}

namespace lsst {
namespace qserv {
namespace loader {


util::CommandTracked::Ptr WWorkerList::createCommand() {
    return createCommandWorker(_central);
}


util::CommandTracked::Ptr WWorkerList::createCommandWorker(CentralWorker* centralW) {
    // On the worker, need to occasionally ask for a list of workers from the master
    // and make sure each of those workers is on the doList
    class MastWorkerListReqCmd : public util::CommandTracked {
    public:
        MastWorkerListReqCmd(CentralWorker* centralW, std::map<uint32_t, WWorkerListItem::Ptr> nameMap)
            : _centralW(centralW), _wIdMap(nameMap) {}

        void action(util::CmdData *data) override {
            /// Request a list of all workers.
            // TODO make a function for this, it's always going to be the same.
            proto::LdrNetAddress protoOurAddress;
            protoOurAddress.set_ip(_centralW->getHostName());
            protoOurAddress.set_udpport(_centralW->getUdpPort());
            protoOurAddress.set_tcpport(_centralW->getTcpPort());
            StringElement eOurAddress(protoOurAddress.SerializeAsString());

            LoaderMsg workerInfoReqMsg(LoaderMsg::MAST_WORKER_LIST_REQ, _centralW->getNextMsgId(),
                                       _centralW->getHostName(), _centralW->getUdpPort());
            BufferUdp sendBuf(1000);
            workerInfoReqMsg.appendToData(sendBuf);
            eOurAddress.appendToData(sendBuf);

            // Send the request to master.
            auto masterHost = _centralW->getMasterHostName();
            auto masterPort = _centralW->getMasterPort();
            LOGS(_log, LOG_LVL_DEBUG, "MastWorkerListReqCmd::action host=" << masterHost <<
                                      " port=" << masterPort);
            try {
                _centralW->sendBufferTo(masterHost, masterPort, sendBuf);
            } catch (boost::system::system_error const& e) {
                LOGS(_log, LOG_LVL_ERROR, "MastWorkerListReqCmd::action boost system_error=" << e.what());
            }

            /// Go through the existing list and add any that have not been add to the doList
            for (auto const& item : _wIdMap) {
                item.second->addDoListItems(_centralW);
            }
        }

    private:
        CentralWorker* _centralW;
        std::map<uint32_t, WWorkerListItem::Ptr> _wIdMap;
    };

    LOGS(_log, LOG_LVL_DEBUG, "WorkerList::createCommandWorker");
    return std::make_shared<MastWorkerListReqCmd>(centralW, _wIdMap);
}


bool WWorkerList::workerListReceive(BufferUdp::Ptr const& data) {
    std::string const funcName("WWorkerList::workerListReceive");
    LOGS(_log, LOG_LVL_INFO, funcName << " data=" << data->dumpStr());
    // Open the data protobuffer and add it to our list.
    StringElement::Ptr sData = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(*data));
    if (sData == nullptr) {
        LOGS(_log, LOG_LVL_WARN, funcName << " Failed to parse list");
        return false;
    }
    auto protoList = sData->protoParse<proto::LdrMastWorkerList>();
    if (protoList == nullptr) {
        LOGS(_log, LOG_LVL_WARN, funcName << " Failed to parse list");
        return false;
    }

    // TODO put this in separate thread, the part above can probably be put in a separate function in _central
    int sizeChange = 0;
    std::string strNames;
    {
        std::lock_guard<std::mutex> lock(_mapMtx);
        size_t initialSize = _wIdMap.size();
        // There may be more workers than will fit in a message.
        _totalNumberOfWorkers = protoList->workercount();
        int sz = protoList->worker_size();

        for (int j=0; j < sz; ++j) {
            proto::WorkerListItem const& protoItem = protoList->worker(j);
            uint32_t wId = protoItem.wid();
            // Most of the time, the worker will already be in the map.
            auto item = _wIdMap[wId];
            if (item == nullptr) {
                item = WWorkerListItem::create(wId, _central);
                _wIdMap[wId] = item;
                strNames += std::to_string(wId) + ",";
                item->addDoListItems(_central);
            }
        }
        sizeChange = _wIdMap.size() - initialSize;
        if (sizeChange > 0) {
            _flagListChange();
        }
    }
    infoReceived(); // This causes it to avoid asking for this info for a while.
    LOGS(_log, LOG_LVL_INFO, funcName << " added " << sizeChange << " names=" << strNames);
    return true;
}

// must lock _mapMtx before calling this function
void WWorkerList::_flagListChange() {
    _wListChanged = true;
}


bool WWorkerList::equal(WWorkerList& other) const {
    std::string const funcName("WWorkerList::equal");
    // Have to lock it this way as 'other' could call it's own equal function which
    // would try to lock in reverse order.
    std::lock(_mapMtx, other._mapMtx);
    std::lock_guard<std::mutex> lk1(_mapMtx,       std::adopt_lock);
    std::lock_guard<std::mutex> lk2(other._mapMtx, std::adopt_lock);

    if (_wIdMap.size() != other._wIdMap.size()) {
        LOGS(_log, LOG_LVL_INFO, funcName << " map sizes do not match this=" << _wIdMap.size() <<
                                 " other=" << other._wIdMap.size());
        return false;
    }
    auto thisIter = _wIdMap.begin();
    auto otherIter = other._wIdMap.begin();
    for (;thisIter != _wIdMap.end() && otherIter != other._wIdMap.end();
            ++thisIter, ++otherIter) {
        if (thisIter->first != otherIter->first) {
            LOGS(_log, LOG_LVL_INFO, funcName << " map first not equal");
            return false;
        }
        if (not thisIter->second->equal(*(otherIter->second))) {
            LOGS(_log, LOG_LVL_INFO, funcName << " map second not equal");
            return false;
        }
    }
    return true;
}


std::string WWorkerList::dump() const {
    std::stringstream os;
    os << "WWorkerList name:\n";
    {
        std::lock_guard<std::mutex> lck(_mapMtx);
        for (auto elem:_wIdMap) {
            os << "  " << *elem.second << "\n";
        }

        os << "WWorkerList ip:\n";
        for (auto elem:_ipMap) {
            os << "  " << *elem.second << "\n";
        }
    }
    return os.str();
}


/// There must be a name. However, ip, port, and range may be invalid.
//  TODO believe our neighbors range over the master
void WWorkerList::updateEntry(uint32_t wId,
                              std::string const& ip, int portUdp, int portTcp,
                              KeyRange& strRange) {
    std::unique_lock<std::mutex> lk(_mapMtx);
    auto iter = _wIdMap.find(wId);
    if (iter == _wIdMap.end()) {
        // This should rarely happen, make an entry for it
        auto newItem = WWorkerListItem::create(wId, _central);
        auto res = _wIdMap.insert(std::make_pair(wId, newItem));
        iter = res.first;
        LOGS(_log, LOG_LVL_INFO, "updateEntry created entry for name=" << wId <<
                                 " res=" << res.second);
    }
    WWorkerListItem::Ptr const& item = iter->second;
    if (ip != "") {
        if (item->getUdpAddress().ip == "" ) {
            NetworkAddress nAddr(ip, portUdp);
            item->setUdpAddress(nAddr);
            item->setTcpAddress(NetworkAddress(ip, portTcp));
            auto res = _ipMap.insert(std::make_pair(nAddr, item));
            LOGS(_log, LOG_LVL_INFO, "updateEntry set wId=" << wId << " Udp=" << nAddr <<
                                     " res=" << res.second);
        }
    }


    // TODO probably special action should be taken if this is our name.
    LOGS(_log, LOG_LVL_INFO, "updateEntry strRange=" << strRange);
    if (strRange.getValid()) {
        // Does the new range match the old range?
        auto oldRange = item->setRangeString(strRange);
        LOGS(_log, LOG_LVL_INFO, "updateEntry set name=" << wId << " range=" << strRange);
        if (not oldRange.equal(strRange)) {
            // Since the value changed, it needs to be removed and reinserted.
            // No invalid ranges should be in the map.
            if (oldRange.getValid()) {
                // The old value was valid, so it is likely in the map.
                auto rangeIter = _rangeMap.find(oldRange);
                if (rangeIter != _rangeMap.end()) {
                    _rangeMap.erase(rangeIter);
                }
            }
            if (strRange.getValid()) {
                _rangeMap[strRange] = item;
            }
        }
    }
}


WWorkerListItem::Ptr WWorkerList::findWorkerForKey(CompositeKey const& key) {
    std::string const funcName("WWorkerList::findWorkerForKey");
    std::unique_lock<std::mutex> lk(_mapMtx);
    // TODO Really could use a custom container for _rangeMap to speed this up.
    for (auto const& elem : _rangeMap) {
        if (elem.second->containsKey(key)) {
            LOGS(_log, LOG_LVL_INFO, funcName << " key=" << elem.first << " -> " << *(elem.second));
            return elem.second;
        }
    }
    LOGS(_log, LOG_LVL_WARN, funcName << " did not find worker for key=" << key);
    return nullptr;
}



void WWorkerListItem::addDoListItems(Central *central) {
    std::lock_guard<std::mutex> lck(_mtx);
    if (_workerUpdateNeedsMasterData == nullptr) {
        _workerUpdateNeedsMasterData.reset(new WorkerNeedsMasterData(getThis(), _central));
        central->addDoListItem(_workerUpdateNeedsMasterData);
    }
}


bool WWorkerListItem::equal(WWorkerListItem& other) const {
    std::string const funcName("WWorkerListItem::equal");

    if (_wId != other._wId) {
        LOGS(_log, LOG_LVL_INFO, funcName << " item name not equal t=" << _wId << " o=" << other._wId);
        return false;
    }
    auto thisUdp = getUdpAddress();
    auto otherUdp = other.getUdpAddress();
    if (thisUdp != otherUdp) {
        LOGS(_log, LOG_LVL_INFO, funcName << " item addr != name=" << _wId <<
                " t=" << thisUdp << " o=" << otherUdp);
        return false;
    }

    std::lock(_mtx, other._mtx);
    std::lock_guard<std::mutex> lck1(_mtx,       std::adopt_lock);
    std::lock_guard<std::mutex> lck2(other._mtx, std::adopt_lock);
    if (not _range.equal(other._range)) {
        LOGS(_log, LOG_LVL_INFO, funcName << " item range != name=" << _wId <<
                                 " t=" << _range << " o=" << other._range);
    }
    return true;
}


util::CommandTracked::Ptr WWorkerListItem::WorkerNeedsMasterData::createCommand() {
    auto item = wWorkerListItem.lock();
    if (item == nullptr) {
        // TODO: should mark set the removal flag for this doListItem
        return nullptr;
    }
    return item->createCommandWorkerInfoReq(central);
}

util::CommandTracked::Ptr WWorkerListItem::createCommandWorkerInfoReq(CentralWorker* centralW) {
     // Create a command to put on the pool to
     //  - ask the master about a server with our _name

    class WorkerReqCmd : public util::CommandTracked {
    public:
        WorkerReqCmd(CentralWorker* centralW, uint32_t name) : _centralW(centralW), _wId(name) {}

        void action(util::CmdData *data) override {
            /// Request all information the master has for one worker.
            LOGS(_log, LOG_LVL_INFO, "WWorkerListItem::createCommand::WorkerReqCmd::action " <<
                                     "ourName=" << _centralW->getOurLogId() << " req name=" << _wId);

            // TODO make a function for this, it's always going to be the same.
            proto::LdrNetAddress protoOurAddress;
            protoOurAddress.set_ip(_centralW->getHostName());
            protoOurAddress.set_udpport(_centralW->getUdpPort());
            protoOurAddress.set_tcpport(_centralW->getTcpPort());
            StringElement eOurAddress(protoOurAddress.SerializeAsString());

            proto::WorkerListItem protoItem;
            protoItem.set_wid(_wId);
            StringElement eItem(protoItem.SerializeAsString());

            LoaderMsg workerInfoReqMsg(LoaderMsg::MAST_WORKER_INFO_REQ, _centralW->getNextMsgId(),
                                       _centralW->getHostName(), _centralW->getUdpPort());
            BufferUdp sendBuf(1000);
            workerInfoReqMsg.appendToData(sendBuf);
            eOurAddress.appendToData(sendBuf);
            eItem.appendToData(sendBuf);

            // Send the request to master.
            auto masterHost = _centralW->getMasterHostName();
            auto masterPort = _centralW->getMasterPort();
            try {
                _centralW->sendBufferTo(masterHost, masterPort, sendBuf);
            } catch (boost::system::system_error const& e) {
                LOGS(_log, LOG_LVL_ERROR, "WorkerReqCmd::action boost system_error=" << e.what() <<
                        " wId=" << _wId);
            }
        }

    private:
        CentralWorker* _centralW;
        uint32_t _wId; ///< worker id
    };

    LOGS(_log, LOG_LVL_INFO, "WWorkerListItem::createCommandWorker this=" <<
                             centralW->getOurLogId() << " name=" << _wId);
    return std::make_shared<WorkerReqCmd>(centralW, _wId);
}


bool WWorkerListItem::containsKey(CompositeKey const& key) const {
    std::lock_guard<std::mutex> lck(_mtx);
    return _range.isInRange(key);
}


}}} // namespace lsst::qserv::loader



