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
#include "loader/CentralWorker.h"

// system headers
#include <boost/asio.hpp>
#include <iostream>

// qserv headers
#include "loader/BufferUdp.h"
#include "loader/CentralWorkerDoListItem.h"
#include "loader/LoaderMsg.h"
#include "loader/WorkerConfig.h"
#include "proto/loader.pb.h"
#include "proto/ProtoImporter.h"


// LSST headers
#include "lsst/log/Log.h"


namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.CentralWorker");
}

namespace lsst {
namespace qserv {
namespace loader {


CentralWorker::CentralWorker(boost::asio::io_service& ioService_, boost::asio::io_context& io_context_,
                             std::string const& hostName_, WorkerConfig const& cfg)
    : Central(ioService_, cfg.getMasterHost(), cfg.getMasterPortUdp(),
              cfg.getThreadPoolSize(), cfg.getLoopSleepTime(), cfg.getIOThreads()),
      _hostName(hostName_),
      _udpPort(cfg.getWPortUdp()),
      _tcpPort(cfg.getWPortTcp()),
      _ioContext(io_context_),
      _recentAddLimit(cfg.getRecentAddLimit()),
      _thresholdNeighborShift(cfg.getThresholdNeighborShift()),
      _maxKeysToShift(cfg.getMaxKeysToShift()) {
}


void CentralWorker::start() {
    _server = std::make_shared<WorkerServer>(ioService, _hostName, _udpPort, this);
    _tcpServer = std::make_shared<ServerTcpBase>(_ioContext, _tcpPort, this);
    _tcpServer->runThread();
    _startMonitoring();
}


CentralWorker::~CentralWorker() {
    _wWorkerList.reset();
    _tcpServer.reset();
}


std::string CentralWorker::getOurLogId() const {
    std::stringstream os;
    os << "(w name=" << _ourId << " addr=" << _hostName <<
            ":udp=" << _udpPort << " tcp=" << _tcpPort << ")";
    return os.str();
}

void CentralWorker::_startMonitoring() {
    // Add _workerList to _doList so it starts checking new entries.
    _centralWorkerDoListItem = std::make_shared<CentralWorkerDoListItem>(this);
    doList->addItem(_wWorkerList);
    doList->addItem(_centralWorkerDoListItem);
}


void CentralWorker::_monitor() {
    LOGS(_log, LOG_LVL_INFO, "CentralWorker::_monitor");

    /// If our id is invalid, try registering with the master.
    if (_isOurIdInvalid()) {
        _registerWithMaster();
        // Give the master a half second to answer.
        usleep(500000);
        return;
    }

    // If data gets shifted, check everything again as ranges will have
    // changed and there may be a lot more data to shift.
    bool dataShifted = false;
    do {
        // TODO Check if we've heard from left neighbor (possibly kill connection if nothing heard???)

        // Check the right neighbor connection, kill and restart if needed.
        // Check if data needs to be shifted with the right node
        // This mutex is locked for a long time  TODO break this up?
        std::lock_guard<std::mutex> rMtxLG(_rightMtx);
        LOGS(_log, LOG_LVL_INFO, "_monitor " << _ourId <<
                                 " checking right neighbor " << _neighborRight.getId());
        if (_neighborRight.getId() != 0) {
            try {
                if (not _neighborRight.getEstablished()) {
                    LOGS(_log, LOG_LVL_INFO, "_monitor " << _ourId << " trying to connect");
                    auto nAddr = _neighborRight.getAddressTcp();
                    if (nAddr.ip == "") {
                        // look up the network address for the rightNeighbor
                        WWorkerListItem::Ptr nWorker =
                            _wWorkerList->getWorkerWithId(_neighborRight.getId());
                        if (nWorker != nullptr) {
                            auto addrTcp = nWorker->getTcpAddress();
                            auto addrUdp = nWorker->getUdpAddress();
                            if (addrTcp.ip.empty() || addrUdp.ip.empty()) {
                                throw LoaderMsgErr(ERR_LOC, "Missing valid address for neighbor=" +
                                                   std::to_string(_neighborRight.getId()));
                            }
                            LOGS(_log, LOG_LVL_INFO, "_monitor neighbor right " <<
                                 _neighborRight.getId() << " T=" << addrTcp << " U=" << addrUdp);
                            _neighborRight.setAddressTcp(addrTcp);
                            _neighborRight.setAddressUdp(addrUdp);
                        }
                    }

                    LOGS(_log, LOG_LVL_INFO, "_monitor trying to establish TCP connection with " <<
                            _neighborRight.getId() << " " << _neighborRight.getAddressTcp());
                    _rightConnect(rMtxLG); // calls _determineRange() while establishing connection
                } else {
                    LOGS(_log, LOG_LVL_INFO, "_monitor " << _ourId << " getting range info");
                    if (_determineRange()) {
                        _rangeChanged = true;
                    }
                }
                dataShifted = _shiftIfNeeded(rMtxLG);
            } catch (LoaderMsgErr const& ex) {
                LOGS(_log, LOG_LVL_ERROR, "_monitor() catching exception " << ex.what());
                _rightDisconnect(rMtxLG);
            } catch (boost::system::system_error const& ex) {
                LOGS(_log, LOG_LVL_ERROR, "_monitor() catching boost exception " << ex.what());
                _rightDisconnect(rMtxLG);
            }
        } else {
            // If there is a connection, close it.
            _rightDisconnect(rMtxLG);
        }
        if (_rangeChanged) {
            // Send new range to master so all clients and workers can be updated.
            _rangeChanged = false;
            LOGS(_log, LOG_LVL_INFO, "_monitor updating range with master");
            NetworkAddress masterAddr(getMasterHostName(), getMasterPort());
            _sendWorkerKeysInfo(masterAddr, getNextMsgId());
        }
    } while (dataShifted);
}


bool CentralWorker::_setOurId(uint32_t id) {
    std::lock_guard<std::mutex> lck(_ourIdMtx);
    if (_ourIdInvalid) {
        _ourId = id;
        _ourIdInvalid = false;
        return true;
    } else {
        /// TODO add error message, check if _ourId matches id
        if (id == 0) {
            _masterDisable();
        } else if (id != _ourId) {
            LOGS(_log, LOG_LVL_ERROR, "worker=" << _ourId <<
                    " id being changed by master!!! new id=" << id);
        }
        return false;
    }
}


void CentralWorker::_masterDisable() {
    LOGS(_log, LOG_LVL_INFO, "worker=" << _ourId <<
            " changed to 0, master shutting this down.");
    _ourIdInvalid = true;
    // Disconnect from right neighbor.
    {
        std::lock_guard<std::mutex> rMtxLG(_rightMtx);
        _rightDisconnect(rMtxLG);
        _neighborRight.setId(0);
    }
    // Disconnect from left neighbor. TODO actively kill the left connection.
    _neighborLeft.setId(0);
    // TODO invalidate range and _keyValueMap
}


bool CentralWorker::_determineRange() {
    std::string const funcName("CentralWorker::_determineRange");
    bool rangeChanged = false;
    BufferUdp data(2000);
    {
        data.reset();
        UInt32Element imLeftKind(LoaderMsg::IM_YOUR_L_NEIGHBOR);
        imLeftKind.appendToData(data);
        // Send information about how many keys on this node and their range.
        StringElement strElem;
        std::unique_ptr<proto::WorkerKeysInfo> protoWKI = _workerKeysInfoBuilder();
        protoWKI->SerializeToString(&(strElem.element));
        UInt32Element bytesInMsg(strElem.transmitSize());
        // Must send the number of bytes in the message so TCP server knows how many bytes to read.
        bytesInMsg.appendToData(data);
        strElem.appendToData(data);
        ServerTcpBase::writeData(*_rightSocket, data);
    }
    // Get back their basic info
    {
        data.reset();
        auto msgElem = data.readFromSocket(*_rightSocket, funcName + " - range bytes");
        auto bytesInMsg = std::dynamic_pointer_cast<UInt32Element>(msgElem);
        msgElem = data.readFromSocket(*_rightSocket, funcName + " - range info");
        auto strWKI = std::dynamic_pointer_cast<StringElement>(msgElem);
        auto protoItem = strWKI->protoParse<proto::WorkerKeysInfo>();
        if (protoItem == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, funcName << " protoItem parse issue!!!!!");
            throw LoaderMsgErr(ERR_LOC, "protoItem parse issue!!!!!");
        }
        NeighborsInfo nInfoR;
        auto workerId = protoItem->wid();
        nInfoR.keyCount = protoItem->mapsize();
        _neighborRight.setKeyCount(nInfoR.keyCount); // TODO add a timestamp to this data.
        nInfoR.recentAdds = protoItem->recentadds();
        proto::WorkerRange protoRange = protoItem->range();
        LOGS(_log, LOG_LVL_INFO, funcName << " rightNeighbor workerId=" << workerId <<
                                 " keyCount=" << nInfoR.keyCount << " recentAdds=" << nInfoR.recentAdds);
        bool valid = protoRange.valid();
        KeyRange rightRange;
        if (valid) {
            CompositeKey min(protoRange.minint(), protoRange.minstr());
            CompositeKey max(protoRange.maxint(), protoRange.maxstr());
            bool unlimited = protoRange.maxunlimited();
            rightRange.setMinMax(min, max, unlimited);
            LOGS(_log, LOG_LVL_INFO, funcName << " rightRange=" << rightRange);
            _neighborRight.setRange(rightRange);
            // Adjust our max range given the the right minimum information.
            // Our maximum value is up to but not including the right minimum.
            {
                std::lock_guard<std::mutex> lckMap(_idMapMtx);
                auto origMax = _keyRange.getMax();
                auto origUnlim = _keyRange.getUnlimited();
                // Can't be unlimited anymore as there is a right neighbor.
                _keyRange.setMax(min, false);
                if (origUnlim != _keyRange.getUnlimited() ||
                    (!origUnlim && origMax != _keyRange.getMax())) {
                    rangeChanged = true;
                }
            }
        }
        proto::Neighbor protoLeftNeigh = protoItem->left();
        nInfoR.neighborLeft->update(protoLeftNeigh.wid());  // Not really useful in this case.
        proto::Neighbor protoRightNeigh = protoItem->right();
        nInfoR.neighborRight->update(protoRightNeigh.wid()); // This should be our id
        if (nInfoR.neighborLeft->get() != getOurId()) {
            LOGS(_log, LOG_LVL_ERROR, "Our (" << getOurId() <<
                ") right neighbor does not have our name as its left neighbor" );
        }
    }
    return rangeChanged;
}


// must hold _rightMtx before calling
bool CentralWorker::_shiftIfNeeded(std::lock_guard<std::mutex> const& rightMtxLG) {
    // There should be reasonably recent information from our neighbors. Use that
    // and our status to ask the right neighbor to give us entries or we send entries
    // to the right neighbor.
    // If right connection is not established, return
    if (not _neighborRight.getEstablished()) {
        LOGS(_log, LOG_LVL_INFO, "_shiftIfNeeded no right neighbor, no shift.");
        return false;
    }
    if (_shiftAsClientInProgress) {
        LOGS(_log, LOG_LVL_INFO, "_shiftIfNeeded shift already in progress.");
        return false;
    }

    // Get local copies of range and map info.
    KeyRange range;
    size_t mapSize;
    {
        std::lock_guard<std::mutex> lck(_idMapMtx);
        range = _keyRange;
        mapSize = _keyValueMap.size();
    }

    // If this worker has more keys than the rightNeighbor, consider shifting keys to the right neighbor.
    // If this worker has _thresholdAverage more keys than average or _thresholdNeighborShift more keys than the right neighbor
    // send enough keys to the right to balance (min 1 key, max _maxShiftKeys, never shift more than 1/3 of our keys)
    int rightKeyCount = 0;
    KeyRange rightRange;
    _neighborRight.getKeyData(rightKeyCount, rightRange);
    if (range > rightRange) {
        LOGS(_log, LOG_LVL_ERROR, "Right neighbor range is less than ours!!!! our=" << range << " right=" << rightRange);
        return false;
    }
    int keysToShift = 0;
    CentralWorker::Direction direction = NONE0;
    int sourceSize = 0;
    LOGS(_log, LOG_LVL_INFO, "_shiftIfNeeded _monitor thisSz=" << mapSize << " rightSz=" << rightKeyCount);
    if (mapSize > rightKeyCount*_thresholdNeighborShift) { // TODO add average across workers check
        keysToShift = (mapSize - rightKeyCount)/2; // Try for nearly equal number of keys on each.
        direction = TORIGHT1;
        sourceSize = mapSize;
    } else if (mapSize*_thresholdNeighborShift < rightKeyCount) { // TODO add average across workers check
        keysToShift = (rightKeyCount - mapSize)/2; // Try for nearly equal number of keys on each.
        direction = FROMRIGHT2;
        sourceSize = rightKeyCount;
    } else {
        LOGS(_log, LOG_LVL_INFO, "No reason to shift.");
        return false;
    }
    if (keysToShift > _maxKeysToShift) keysToShift = _maxKeysToShift;
    if (keysToShift > sourceSize/3) keysToShift = sourceSize/3;
    if (keysToShift < 1) {
        LOGS(_log, LOG_LVL_INFO, "Worker doesn't have enough keys to shift.");
        return false;
    }
    _shiftAsClientInProgress = true;
    LOGS(_log, LOG_LVL_INFO, "shift dir(TO1 FROM2)=" << direction << " keys=" << keysToShift <<
                             " szThis=" << mapSize << " szRight=" << rightKeyCount);
    _shift(direction, keysToShift);
    return true;
}


void CentralWorker::_shift(Direction direction, int keysToShift) {
    LOGS(_log, LOG_LVL_DEBUG, "CentralWorker::_shift");
    if (direction == FROMRIGHT2) {
        BufferUdp data(1000000);
        // Construct a message asking for keys to shift (it will shift its lowest keys, which will be our highest keys)
        proto::KeyShiftRequest protoKeyShiftRequest;
        protoKeyShiftRequest.set_keystoshift(keysToShift);
        {
            StringElement keyShiftReq;
            protoKeyShiftRequest.SerializeToString(&(keyShiftReq.element));
            // Send the message kind, followed by the transmit size, and then the protobuffer.
            UInt32Element kindShiftFromRight(LoaderMsg::SHIFT_FROM_RIGHT);
            UInt32Element bytesInMsg(keyShiftReq.transmitSize());
            BufferUdp data(kindShiftFromRight.transmitSize() + bytesInMsg.transmitSize() + keyShiftReq.transmitSize());
            kindShiftFromRight.appendToData(data);
            bytesInMsg.appendToData(data);
            keyShiftReq.appendToData(data);
            LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift FROMRIGHT " << keysToShift);
            ServerTcpBase::writeData(*_rightSocket, data);
        }
        // Wait for the KeyList response
        {
            data.reset();
            auto msgElem = data.readFromSocket(*_rightSocket,
                                               "CentralWorker::_shift waiting for FROMRIGHT KeyList");
            auto keyListElem = std::dynamic_pointer_cast<StringElement>(msgElem);
            if (keyListElem == nullptr) {
                throw LoaderMsgErr(ERR_LOC, "_shift FROMRIGHT failure to get KeyList");
            }
            auto protoKeyList = keyListElem->protoParse<proto::KeyList>();
            if (protoKeyList == nullptr) {
                throw LoaderMsgErr(ERR_LOC, "_shift FROMRIGHT failure to parse KeyList size=" +
                                   std::to_string(keyListElem->element.size()));
            }

            // TODO This is very similar to code in TcpBaseConnection::_handleShiftToRight and they should be merged.
            int sz = protoKeyList->keypair_size();
            std::vector<CentralWorker::CompKeyPair> keyList;
            for (int j=0; j < sz; ++j) {
                proto::KeyInfo const& protoKI = protoKeyList->keypair(j);
                ChunkSubchunk chSub(protoKI.chunk(), protoKI.subchunk());
                keyList.push_back(std::make_pair(CompositeKey(protoKI.keyint(), protoKI.keystr()), chSub));
            }
            insertKeys(keyList, false);
        }
        // Send received message
        data.reset();
        UInt32Element elem(LoaderMsg::SHIFT_FROM_RIGHT_RECEIVED);
        elem.appendToData(data);
        ServerTcpBase::writeData(*_rightSocket, data);
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift  direction=" << direction << " keys=" << keysToShift);

    } else if (direction == TORIGHT1) {
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift TORIGHT " << keysToShift);
        // TODO this is very similar to CentralWorker::buildKeyList() and should be merged with that.
        // Construct a message with that many keys and send it (sending the highest keys)
        proto::KeyList protoKeyList;
        protoKeyList.set_keycount(keysToShift);
        CompositeKey minKey = CompositeKey::minValue; // smallest key is sent to right neighbor
        {
            std::lock_guard<std::mutex> lck(_idMapMtx);
            if (not _transferListToRight.empty()) {
                throw LoaderMsgErr(ERR_LOC, "_shift _transferList not empty");
            }
            for (int j=0; j < keysToShift && _keyValueMap.size() > 1; ++j) {
                auto iter = _keyValueMap.end();
                --iter; // rbegin() returns a reverse iterator which doesn't work with erase().
                _transferListToRight.push_back(std::make_pair(iter->first, iter->second));
                proto::KeyInfo* protoKI = protoKeyList.add_keypair();
                minKey = iter->first;
                protoKI->set_keyint(minKey.kInt);
                protoKI->set_keystr(minKey.kStr);
                protoKI->set_chunk(iter->second.chunk);
                protoKI->set_subchunk(iter->second.subchunk);
                _keyValueMap.erase(iter);
            }
            // Adjust our range;
            _keyRange.setMax(minKey);
        }
        StringElement keyList;
        protoKeyList.SerializeToString(&(keyList.element));
        // Send the message kind, followed by the transmit size, and then the protobuffer.
        UInt32Element kindShiftRight(LoaderMsg::SHIFT_TO_RIGHT);
        UInt32Element bytesInMsg(keyList.transmitSize());
        BufferUdp data(kindShiftRight.transmitSize() + bytesInMsg.transmitSize() + keyList.transmitSize());
        kindShiftRight.appendToData(data);
        bytesInMsg.appendToData(data);
        keyList.appendToData(data);
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift TORIGHT sending keys");
        ServerTcpBase::writeData(*_rightSocket, data);

        // read back LoaderMsg::SHIFT_TO_RIGHT_KEYS_RECEIVED
        data.reset();
        auto msgElem = data.readFromSocket(*_rightSocket,
                                           "CentralWorker::_shift SHIFT_TO_RIGHT_KEYS_RECEIVED");
        UInt32Element::Ptr received = std::dynamic_pointer_cast<UInt32Element>(msgElem);
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift TORIGHT keys were received");
        if (received == nullptr || received->element !=  LoaderMsg::SHIFT_TO_RIGHT_RECEIVED) {
            throw LoaderMsgErr(ERR_LOC, "_shift receive failure");
        }
        _finishShiftToRight();
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift end direction=" << direction << " keys=" << keysToShift);
    }
    LOGS(_log, LOG_LVL_INFO, "CentralWorker::_shift DumpKeys " << dumpKeysStr(2));
    _shiftAsClientInProgress = false;
}


void CentralWorker::_finishShiftToRight() {
    std::lock_guard<std::mutex> lck(_idMapMtx);
    _transferListToRight.clear();
}


void CentralWorker::finishShiftFromRight() {
    std::lock_guard<std::mutex> lck(_idMapMtx);
    _transferListWithLeft.clear();
}


StringElement::UPtr CentralWorker::buildKeyList(int keysToShift) {
    std::string funcName = "CentralWorker::buildKeyList";
    proto::KeyList protoKeyList;
    CompositeKey minKey = CompositeKey::minValue; // smallest key sent
    CompositeKey maxKey = CompositeKey::minValue; // largest key sent
    {
        LOGS(_log, LOG_LVL_INFO, funcName);
        std::lock_guard<std::mutex> lck(_idMapMtx);
        if (not _transferListWithLeft.empty()) {
            throw LoaderMsgErr(ERR_LOC, "_shift _transferListFromRight not empty");
        }
        int maxKeysToShift = _keyValueMap.size()/3;
        if (keysToShift > maxKeysToShift) keysToShift = maxKeysToShift;
        protoKeyList.set_keycount(keysToShift);
        bool firstPass = true;
        for (int j=0; j < keysToShift && _keyValueMap.size() > 1; ++j) {
            auto iter = _keyValueMap.begin();
            if (firstPass) {
                minKey = iter->first;
            }
            _transferListWithLeft.push_back(std::make_pair(iter->first, iter->second));
            proto::KeyInfo* protoKI = protoKeyList.add_keypair();
            maxKey = iter->first;
            protoKI->set_keyint(maxKey.kInt);
            protoKI->set_keystr(maxKey.kStr);
            protoKI->set_chunk(iter->second.chunk);
            protoKI->set_subchunk(iter->second.subchunk);
            _keyValueMap.erase(iter);
        }
        // Adjust our range;
        auto iter = _keyValueMap.begin();
        auto minKey = _keyRange.getMin();
        if (minKey != CompositeKey::minValue) {
            if (iter->first != minKey) {
                _keyRange.setMin(iter->first);
                _rangeChanged = true;
            }
        }
    }
    StringElement::UPtr keyList(new StringElement());
    protoKeyList.SerializeToString(&(keyList->element));
    return keyList;
}


/// Must hold _rightMtx before calling
void CentralWorker::_rightConnect(std::lock_guard<std::mutex> const& rightMtxLG) {
    std::string const funcName("CentralWorker::_rightConnect");
    if(_rightConnectStatus == VOID0) {
        _rightConnectStatus = STARTING1;
        // Connect to the right neighbor server
        AsioTcp::resolver resolver(_ioContext);
        auto addr = _neighborRight.getAddressTcp();
        AsioTcp::resolver::results_type endpoints = resolver.resolve(addr.ip, std::to_string(addr.port));
        _rightSocket.reset(new AsioTcp::socket(_ioContext));
        boost::system::error_code ec;
        boost::asio::connect(*_rightSocket, endpoints, ec);
        if (ec) {
            _rightSocket.reset();
            LOGS(_log, LOG_LVL_WARN, "failed to connect to " << _neighborRight.getId() << " " <<
                    addr << " ec=" << ec.value() << ":" << ec.message());
            return;
        }

        // Get name from server
        BufferUdp data(2000);
        {
            auto msgElem = data.readFromSocket(*_rightSocket, "CentralWorker::_rightConnect");
            // First element should be UInt32Element with the other worker's name
            UInt32Element::Ptr nghName = std::dynamic_pointer_cast<UInt32Element>(msgElem);
            if (nghName == nullptr) {
                throw LoaderMsgErr(ERR_LOC, std::string("first element wasn't correct type ") +
                        msgElem->getStringVal());
            }

            // Check if correct name
            if (nghName->element != _neighborRight.getId()) {
                throw LoaderMsgErr(ERR_LOC, std::string("wrong name expected ") +
                                   std::to_string(_neighborRight.getId()) +
                                   " got " + std::to_string(nghName->element));
            }
        }

        // Send our basic key info so ranges can be determined.
        _determineRange();

        _rightConnectStatus = ESTABLISHED2;
        _neighborRight.setEstablished(true);
    }
}


void CentralWorker::setNeighborInfoLeft(uint32_t wId, int keyCount, KeyRange const& range) {
    if (wId != _neighborLeft.getId()) {
        LOGS(_log, LOG_LVL_ERROR, "disconnecting left since setNeighborInfoLeft wId(" << wId <<
                                  ") != neighborLeft.name(" << _neighborLeft.getId() << ")");
        _neighborLeft.setEstablished(false);
        return;
    }
    _neighborLeft.setKeyCount(keyCount);
    _neighborLeft.setRange(range);
    _neighborLeft.setEstablished(true);
}


/// Must hold _rightMtx before calling
void CentralWorker::_rightDisconnect(std::lock_guard<std::mutex> const& lg) {
    LOGS(_log, LOG_LVL_DEBUG, "CentralWorker::_rightDisconnect");
    if (_rightSocket != nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::_rightDisconnect disconnecting");
        _rightSocket->shutdown(boost::asio::ip::tcp::socket::shutdown_both);
        _rightSocket->close();
        _neighborRight.setEstablished(false);
    }
    _rightConnectStatus = VOID0;
    _cancelShiftsWithRightNeighbor();
}


void CentralWorker::_cancelShiftsWithRightNeighbor() {
    // Client side of connection, was sending largest keys right.
    // If keys were being shifted from right, this node's map is still intact.
    LOGS(_log, LOG_LVL_DEBUG, "_cancelShiftsWithRightNeighbor");
    std::lock_guard<std::mutex> lck(_idMapMtx);
    if (_shiftAsClientInProgress.exchange(false)) {
        LOGS(_log, LOG_LVL_WARN, "Canceling shiftToRight neighbor");
        // Restore the transfer list to the id map
        for (auto&& elem:_transferListToRight) {
            auto res = _keyValueMap.insert(std::make_pair(elem.first, elem.second));
            if (not res.second) {
                LOGS(_log, LOG_LVL_WARN, "_cancelShiftsRightNeighbor Possible duplicate " <<
                                         elem.first << ":" << elem.second);
            }
        }
        _transferListToRight.clear();
        // Leave the reduced range until fixed by our right neighbor.
    }
}


void CentralWorker::cancelShiftsWithLeftNeighbor() {
    // Server side of connection, was sending smallest keys left.
    // If keys were being transfered from the left node, this node's map is still intact.
    LOGS(_log, LOG_LVL_WARN, "cancelShiftsWithLeftNeighbor");
    std::lock_guard<std::mutex> lck(_idMapMtx);
    if (not _transferListWithLeft.empty()) {
        // Restore the transfer list to the id map
        for (auto&& elem:_transferListWithLeft) {
            auto res = _keyValueMap.insert(std::make_pair(elem.first, elem.second));
            if (not res.second) {
                LOGS(_log, LOG_LVL_WARN, "_cancelShiftsRightNeighbor Possible duplicate " <<
                                         elem.first << ":" << elem.second);
            }
        }
        _transferListWithLeft.clear();

        // Fix the bottom of the range.
        if (_keyRange.getMin() != CompositeKey::minValue) {
            _keyRange.setMin(_keyValueMap.begin()->first);
        }
    }
}


bool CentralWorker::workerInfoReceive(BufferUdp::Ptr const&  data) {
    // Open the data protobuffer and add it to our list.
    StringElement::Ptr sData = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(*data));
    if (sData == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::workerInfoRecieve Failed to parse list");
        return false;
    }
    std::unique_ptr<proto::WorkerListItem> protoList = sData->protoParse<proto::WorkerListItem>();
    if (protoList == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::workerInfoRecieve Failed to parse list");
        return false;
    }

    // TODO: move this call to another thread
    _workerInfoReceive(protoList);
    return true;
}


void CentralWorker::_workerInfoReceive(std::unique_ptr<proto::WorkerListItem>& protoL) {
    std::unique_ptr<proto::WorkerListItem> protoList(std::move(protoL));

    // Check the information, if it is our network address, set or check our id.
    // Then compare it with the map, adding new/changed information.
    uint32_t wId = protoList->wid();
    std::string ipUdp("");
    int portUdp = 0;
    int portTcp = 0;
    if (protoList->has_address()) {
        proto::LdrNetAddress protoAddr = protoList->address();
        ipUdp = protoAddr.ip();
        portUdp = protoAddr.udpport();
        portTcp = protoAddr.tcpport();
    }
    KeyRange strRange;
    if (protoList->has_range()) {
        proto::WorkerRange protoRange = protoList->range();
        bool valid        = protoRange.valid();
        if (valid) {
            CompositeKey min(protoRange.minint(), protoRange.minstr());
            CompositeKey max(protoRange.maxint(), protoRange.maxstr());
            bool unlimited = protoRange.maxunlimited();
            strRange.setMinMax(min, max, unlimited);
        }
    }

    // If the address matches ours, check the name.
    if (getHostName() == ipUdp && getUdpPort() == portUdp) {
        if (_isOurIdInvalid()) {
            LOGS(_log, LOG_LVL_INFO, "Setting our name " << wId);
            _setOurId(wId);
        } else if (getOurId() != wId) {
            LOGS(_log, LOG_LVL_ERROR, "Our wId doesn't match address from master! wId=" <<
                                      getOurId() << " from master=" << wId);
        }

        // It is this worker. If there is a valid range in the message and our range is not valid,
        // take the range given as our own.
        if (strRange.getValid()) {
            std::lock_guard<std::mutex> lckM(_idMapMtx);
            if (not _keyRange.getValid()) {
                LOGS(_log, LOG_LVL_INFO, "Setting our range " << strRange);
                _keyRange.setMinMax(strRange.getMin(), strRange.getMax(), strRange.getUnlimited());
            }
        }
    }

    // Make/update entry in map.
    _wWorkerList->updateEntry(wId, ipUdp, portUdp, portTcp, strRange);
}


KeyRange CentralWorker::updateRangeWithLeftData(KeyRange const& leftNeighborRange) {
    // Update our range with data from our left neighbor. Our min is their max.
    // If our range is invalid
    //    our min is their max incremented (stringRange increment function)
    //    if their max is unlimited, our max becomes unlimited
    //    else max = increment(min)
    //    send range to master
    //    return our new range
    KeyRange newLeftNeighborRange(leftNeighborRange);
    {
        std::unique_lock<std::mutex> lck(_idMapMtx);
        if (not _keyRange.getValid()) {
            // Our range has not been set, so base it on the range of the left neighbor.
            auto min = KeyRange::increment(leftNeighborRange.getMax());
            auto max = min;
            _keyRange.setMinMax(min, max, leftNeighborRange.getUnlimited());
            newLeftNeighborRange.setMax(max, false);
        } else {
            // Our range is valid already, it should be > than the left neighbor range.
            if (_keyRange < leftNeighborRange) {
                LOGS(_log, LOG_LVL_ERROR, "LeftNeighborRange(" << leftNeighborRange <<
                        ") is greater than our range(" << _keyRange << ")");
                // TODO corrective action?
            }
            // The left neighbor's max should be the minimum value in our keymap, unless the
            // map is empty.
            if (_keyValueMap.empty()) {
                // Don't do anything to left neighbor range.
            } else {
                auto min = _keyValueMap.begin()->first;
                _keyRange.setMin(min);
                newLeftNeighborRange.setMax(min, false);
            }
        }
    }

    return newLeftNeighborRange;
}


bool CentralWorker::workerKeyInsertReq(LoaderMsg const& inMsg, BufferUdp::Ptr const&  data) {
    StringElement::Ptr sData = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(*data));
    if (sData == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::workerKeyInsertReq Failed to read list element");
        return false;
    }
    auto protoData = sData->protoParse<proto::KeyInfoInsert>();
    if (protoData == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::workerKeyInsertReq Failed to parse list");
        return false;
    }

    // TODO move this to another thread
    _workerKeyInsertReq(inMsg, protoData);
    return true;
}


void CentralWorker::_workerKeyInsertReq(LoaderMsg const& inMsg, std::unique_ptr<proto::KeyInfoInsert>& protoBuf) {
    std::unique_ptr<proto::KeyInfoInsert> protoData(std::move(protoBuf));

    // Get the source of the request
    proto::LdrNetAddress protoAddr = protoData->requester();
    NetworkAddress nAddr(protoAddr.ip(), protoAddr.udpport());

    proto::KeyInfo protoKeyInfo = protoData->keyinfo();
    CompositeKey key(protoKeyInfo.keyint(), protoKeyInfo.keystr());
    ChunkSubchunk chunkInfo(protoKeyInfo.chunk(), protoKeyInfo.subchunk());

    /// see if the key should be inserted into our map
    std::unique_lock<std::mutex> lck(_idMapMtx);
    auto min = _keyRange.getMin();
    auto leftAddress = _neighborLeft.getAddressUdp();
    auto rightAddress = _neighborRight.getAddressUdp();
    if (_keyRange.isInRange(key)) {
        // insert into our map
        auto res = _keyValueMap.insert(std::make_pair(key, chunkInfo));
        lck.unlock();
        if (not res.second) {
            // Element already found, check file id and row number. Bad if not the same.
            // TODO HIGH send back duplicate key mismatch message to the original requester and return
        }
        LOGS(_log, LOG_LVL_INFO, "Key inserted=" << key << "(" << chunkInfo << ")");
        // TODO Send this item to the keyLogger (which would then send KEY_INSERT_COMPLETE back to the requester),
        // for now this function will send the message back for proof of concept.
        LoaderMsg msg(LoaderMsg::KEY_INSERT_COMPLETE, inMsg.msgId->element, getHostName(), getUdpPort());
        BufferUdp msgData;
        msg.appendToData(msgData);
        // protoKeyInfo should still be the same
        proto::KeyInfo protoReply;
        protoReply.set_keyint(key.kInt);
        protoReply.set_keystr(key.kStr);
        protoReply.set_chunk(chunkInfo.chunk);
        protoReply.set_subchunk(chunkInfo.subchunk);
        StringElement strElem;
        protoReply.SerializeToString(&(strElem.element));
        strElem.appendToData(msgData);
        LOGS(_log, LOG_LVL_INFO, "sending complete " << key << " to " << nAddr << " from " << _ourId);
        try {
            sendBufferTo(nAddr.ip, nAddr.port, msgData);
        } catch (boost::system::system_error const& e) {
            LOGS(_log, LOG_LVL_ERROR, "CentralWorker::_workerKeyInsertReq boost system_error=" << e.what() <<
                    " msg=" << inMsg);
        }
    } else {
        lck.unlock();
        // Find the target range in the list and send the request there
        auto targetWorker = _wWorkerList->findWorkerForKey(key);
        if (targetWorker != nullptr && targetWorker->getId() != _ourId) {
            _forwardKeyInsertRequest(targetWorker->getUdpAddress(), inMsg, protoData);
        } else {
            // Send request to left or right neighbor
            if (key < min && leftAddress.ip != "") {
                _forwardKeyInsertRequest(leftAddress, inMsg, protoData);
            } else if (key > min && rightAddress.ip != "") {
                _forwardKeyInsertRequest(rightAddress, inMsg, protoData);
            }
        }
    }
}


void CentralWorker::_forwardKeyInsertRequest(NetworkAddress const& targetAddr, LoaderMsg const& inMsg,
                                             std::unique_ptr<proto::KeyInfoInsert>& protoData) {
    // Aside from hops, the proto buffer should be the same.
    proto::KeyInfo protoKeyInfo = protoData->keyinfo();
    CompositeKey key(protoKeyInfo.keyint(), protoKeyInfo.keystr());
    // The proto buffer should be the same, just need a new message.
    int hops = protoData->hops() + 1;
    if (hops > 4) { // TODO replace magic number with variable set via config file.
        LOGS(_log, LOG_LVL_INFO, "Too many hops, dropping insert request hops=" << hops << " key=" << key);
        return;
    }
    LOGS(_log, LOG_LVL_INFO, "Forwarding key insert hops=" << hops << " key=" << key);
    LoaderMsg msg(LoaderMsg::KEY_INSERT_REQ, inMsg.msgId->element, getHostName(), getUdpPort());
    BufferUdp msgData;
    msg.appendToData(msgData);

    StringElement strElem;
    protoData->SerializeToString(&(strElem.element));
    strElem.appendToData(msgData);
    try {
        sendBufferTo(targetAddr.ip, targetAddr.port, msgData);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralWorker::_forwardKeyInsertRequest boost system_error=" << e.what() <<
                " tAddr=" << targetAddr << " inMsg=" << inMsg);
    }
}


bool CentralWorker::workerKeyInfoReq(LoaderMsg const& inMsg, BufferUdp::Ptr const&  data) {
    LOGS(_log, LOG_LVL_DEBUG, "CentralWorker::workerKeyInfoReq");
    StringElement::Ptr sData = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(*data));
    if (sData == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::workerKeyInfoReq Failed to read list element");
        return false;
    }
    auto protoData = sData->protoParse<proto::KeyInfoInsert>();
    if (protoData == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "CentralWorker::workerKeyInfoReq Failed to parse list");
        return false;
    }

    // TODO move this to another thread
    _workerKeyInfoReq(inMsg, protoData);
    return true;
}


void CentralWorker::_workerKeyInfoReq(LoaderMsg const& inMsg, std::unique_ptr<proto::KeyInfoInsert>& protoBuf) {
    std::unique_ptr<proto::KeyInfoInsert> protoData(std::move(protoBuf));

    // Get the source of the request
    proto::LdrNetAddress protoAddr = protoData->requester();
    NetworkAddress nAddr(protoAddr.ip(), protoAddr.udpport());

    proto::KeyInfo protoKeyInfo = protoData->keyinfo();
    CompositeKey key(protoKeyInfo.keyint(), protoKeyInfo.keystr());

    /// see if the key is in our map
    std::unique_lock<std::mutex> lck(_idMapMtx);
    if (_keyRange.isInRange(key)) {
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_workerKeyInfoReq " << _ourId << " looking for key=" << key);
        // check out map
        auto iter = _keyValueMap.find(key);
        lck.unlock();

        // Key found or not, message will be returned.
        LoaderMsg msg(LoaderMsg::KEY_LOOKUP, inMsg.msgId->element, getHostName(), getUdpPort());
        BufferUdp msgData;
        msg.appendToData(msgData);
        proto::KeyInfo protoReply;
        protoReply.set_keyint(key.kInt);
        protoReply.set_keystr(key.kStr);
        if (iter == _keyValueMap.end()) {
            // key not found message.
            protoReply.set_chunk(0);
            protoReply.set_subchunk(0);
            protoReply.set_success(false);
            LOGS(_log, LOG_LVL_INFO, "Key info not found key=" << key);
        } else {
            // key found message.
            auto elem = iter->second;
            protoReply.set_chunk(elem.chunk);
            protoReply.set_subchunk(elem.subchunk);
            protoReply.set_success(true);
            LOGS(_log, LOG_LVL_INFO, "Key info lookup key=" << key <<
                 " (" << protoReply.chunk() << ", " << protoReply.subchunk() << ")");
        }
        StringElement strElem;
        protoReply.SerializeToString(&(strElem.element));
        strElem.appendToData(msgData);
        LOGS(_log, LOG_LVL_INFO, "sending key lookup " << key << " to " << nAddr << " from " << _ourId);
        try {
            sendBufferTo(nAddr.ip, nAddr.port, msgData);
        } catch (boost::system::system_error const& e) {
            LOGS(_log, LOG_LVL_ERROR, "CentralWorker::_workerKeyInfoReq boost system_error=" << e.what() <<
                    " inMsg=" << inMsg);
        }
    } else {
        // Find the target range in the list and send the request there
        auto targetWorker = _wWorkerList->findWorkerForKey(key);
        if (targetWorker == nullptr) {
            LOGS(_log, LOG_LVL_INFO, "CentralWorker::_workerKeyInfoReq " << _ourId <<
                                     " could not forward key=" << key);
            // TODO HIGH forward request to neighbor in case it was in recent shift.
            return;
        }
        LOGS(_log, LOG_LVL_INFO, "CentralWorker::_workerKeyInfoReq " << _ourId <<
                                 " forwarding key=" << key << " to " << *targetWorker);
        _forwardKeyInfoRequest(targetWorker, inMsg, protoData);
    }
}


bool CentralWorker::workerWorkerSetRightNeighbor(LoaderMsg const& inMsg, BufferUdp::Ptr const& data) {
    auto msgElem = MsgElement::retrieve(*data);
    UInt32Element::Ptr neighborName = std::dynamic_pointer_cast<UInt32Element>(msgElem);
    if (neighborName == nullptr) {
        return false;
    }

    LOGS(_log, LOG_LVL_INFO, "workerWorkerSetRightNeighbor ourName=" << _ourId << " rightN=" << neighborName->element);
    // Just setting the name, so it can stay here. See CentralWorker::_monitor(), which establishes/maintains connections.
    _neighborRight.setId(neighborName->element);
    return true;
}


bool CentralWorker::workerWorkerSetLeftNeighbor(LoaderMsg const& inMsg, BufferUdp::Ptr const& data) {
    auto msgElem = MsgElement::retrieve(*data);
    UInt32Element::Ptr neighborName = std::dynamic_pointer_cast<UInt32Element>(msgElem);
    if (neighborName == nullptr) {
        return false;
    }

    LOGS(_log, LOG_LVL_INFO, "workerWorkerSetLeftNeighbor ourName=" << _ourId << " leftN=" << neighborName->element);
    // TODO move to separate thread
    _neighborLeft.setId(neighborName->element);
    // Just setting the name. See CentralWorker::_monitor(), which establishes/maintains connections.
    return true;
}


bool CentralWorker::workerWorkerKeysInfoReq(LoaderMsg const& inMsg, BufferUdp::Ptr const& data) {
    // Send a message containing information about the range and number of keys handled by this worker back
    // to the sender. Nothing in data

    // TODO move this to another thread
    _workerWorkerKeysInfoReq(inMsg);
    return true;

}


void CentralWorker::_workerWorkerKeysInfoReq(LoaderMsg const& inMsg) {
    // Use the address from inMsg as this kind of request is pointless to forward.
    NetworkAddress nAddr(inMsg.senderHost->element, inMsg.senderPort->element);
    uint64_t msgId = inMsg.msgId->element;
    _sendWorkerKeysInfo(nAddr, msgId);
}


void CentralWorker::_sendWorkerKeysInfo(NetworkAddress const& nAddr, uint64_t msgId) {
    // Build message containing Range, size of map, number of items added.
    LoaderMsg msg(LoaderMsg::WORKER_KEYS_INFO, msgId, getHostName(), getUdpPort());
    BufferUdp msgData;
    msg.appendToData(msgData);
    std::unique_ptr<proto::WorkerKeysInfo> protoWKI = _workerKeysInfoBuilder();
    StringElement strElem;
    protoWKI->SerializeToString(&(strElem.element));
    strElem.appendToData(msgData);
    LOGS(_log, LOG_LVL_INFO, "sending WorkerKeysInfo name=" << _ourId <<
         " mapsize=" << protoWKI->mapsize() << " recentAdds=" << protoWKI->recentadds() <<
         " to " << nAddr);
    try {
        sendBufferTo(nAddr.ip, nAddr.port, msgData);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralWorker::_sendWorkerKeysInfo boost system_error=" << e.what() <<
                " nAddr=" << nAddr << "msgId=" << msgId);
    }
}


std::unique_ptr<proto::WorkerKeysInfo> CentralWorker::_workerKeysInfoBuilder() {
    std::unique_ptr<proto::WorkerKeysInfo> protoWKI(new proto::WorkerKeysInfo());
    // Build message containing Range, size of map, number of items added.
    // TODO this code is similar to code elsewhere, try to merge it.
    KeyRange range;
    size_t mapSize;
    size_t recentAdds;
    {
        std::lock_guard<std::mutex> lck(_idMapMtx);
        range = _keyRange;
        mapSize = _keyValueMap.size();
        _removeOldEntries();
        recentAdds = _recentAdds.size();
    }
    LOGS(_log, LOG_LVL_INFO, "CentralWorker WorkerKeysInfo a name=" << _ourId <<
                             " keyCount=" << mapSize << " recentAdds=" << recentAdds);
    protoWKI->set_wid(_ourId);
    protoWKI->set_mapsize(mapSize);
    protoWKI->set_recentadds(recentAdds);
    proto::WorkerRange *protoRange = protoWKI->mutable_range();
    range.loadProtoRange(*protoRange);
    proto::Neighbor *protoLeft = protoWKI->mutable_left();
    protoLeft->set_wid(_neighborLeft.getId());
    proto::Neighbor *protoRight = protoWKI->mutable_right();
    protoRight->set_wid(_neighborRight.getId());
    LOGS(_log, LOG_LVL_INFO, "CentralWorker WorkerKeysInfo b name=" << _ourId <<
                             " keyCount=" << mapSize << " recentAdds=" << recentAdds);
    return protoWKI;
}


// TODO This looks a lot like the other _forward*** functions, try to combine them.
void CentralWorker::_forwardKeyInfoRequest(WWorkerListItem::Ptr const& target, LoaderMsg const& inMsg,
                                             std::unique_ptr<proto::KeyInfoInsert> const& protoData) {
    // The proto buffer should be the same, just need a new message.
    LoaderMsg msg(LoaderMsg::KEY_LOOKUP_REQ, inMsg.msgId->element, getHostName(), getUdpPort());
    BufferUdp msgData;
    msg.appendToData(msgData);

    StringElement strElem;
    protoData->SerializeToString(&(strElem.element));
    strElem.appendToData(msgData);

    auto nAddr = target->getUdpAddress();
    try {
        sendBufferTo(nAddr.ip, nAddr.port, msgData);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralWorker::_forwardKeyInfoRequest boost system_error=" << e.what() <<
                " target=" << target << " inMsg=" << inMsg);
    }
}


void CentralWorker::_registerWithMaster() {
    LoaderMsg msg(LoaderMsg::MAST_WORKER_ADD_REQ, getNextMsgId(), getHostName(), getUdpPort());
    BufferUdp msgData;
    msg.appendToData(msgData);
    // create the proto buffer
    lsst::qserv::proto::LdrNetAddress protoBuf;
    protoBuf.set_ip(getHostName());
    protoBuf.set_udpport(getUdpPort());
    protoBuf.set_tcpport(getTcpPort());

    StringElement strElem;
    protoBuf.SerializeToString(&(strElem.element));
    strElem.appendToData(msgData);

    try {
        sendBufferTo(getMasterHostName(), getMasterPort(), msgData);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralWorker::_registerWithMaster boost system_error=" << e.what());
    }
}


void CentralWorker::testSendBadMessage() {
    uint16_t kind = 60200;
    LoaderMsg msg(kind, getNextMsgId(), getHostName(), getUdpPort());
    LOGS(_log, LOG_LVL_INFO, "testSendBadMessage msg=" << msg);
    BufferUdp msgData(128);
    msg.appendToData(msgData);
    try {
        sendBufferTo(getMasterHostName(), getMasterPort(), msgData);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "CentralWorker::testSendBadMessage boost system_error=" << e.what());
        throw e; // This would not be the expected error, re-throw so it is noticed.
    }
}


void CentralWorker::_removeOldEntries() {
    // _idMapMtx must be held when this is called.
    auto now = std::chrono::system_clock::now();
    auto then = now - _recentAddLimit;
    while (_recentAdds.size() > 0 && _recentAdds.front() < then) {
        _recentAdds.pop_front();
    }
}


void CentralWorker::insertKeys(std::vector<CompKeyPair> const& keyList, bool mustSetMin) {
    std::unique_lock<std::mutex> lck(_idMapMtx);
    auto maxKey = _keyRange.getMax();
    bool maxKeyChanged = false;
    for (auto&& elem:keyList) {
        auto const& key = elem.first;
        auto res = _keyValueMap.insert(std::make_pair(key, elem.second));
        if (key > maxKey) {
            maxKey = key;
            maxKeyChanged = true;
        }
        if (not res.second) {
            LOGS(_log, LOG_LVL_WARN, "insertKeys Possible duplicate " <<
                                     elem.first << ":" << elem.second);
        }
    }

    // On all nodes except the left most, the minimum should be reset.
    if (mustSetMin && _keyValueMap.size() > 0) {
        auto minKeyPair = _keyValueMap.begin();
        _keyRange.setMin(minKeyPair->first);
    }

    if (maxKeyChanged) {
        // if unlimited is false, range will be slightly off until corrected by the right neighbor.
        bool unlimited = _keyRange.getUnlimited();
        _keyRange.setMax(maxKey, unlimited);
    }
}


std::string CentralWorker::dumpKeysStr(unsigned int count) {
    std::stringstream os;
    std::lock_guard<std::mutex> lck(_idMapMtx);
    os << "name=" << getOurId() << " count=" << _keyValueMap.size() << " range("
       << _keyRange << ") pairs: ";

    if (count < 1 || _keyValueMap.size() < count*2) {
        for (auto&& elem:_keyValueMap) {
            os << elem.first << "{" << elem.second << "} ";
        }
    } else {
        auto iter = _keyValueMap.begin();
        for (size_t j=0; j < count && iter != _keyValueMap.end(); ++iter, ++j) {
            os << iter->first << "{" << iter->second << "} ";
        }
        os << " ... ";
        auto rIter = _keyValueMap.rbegin();
        for (size_t j=0; j < count && rIter != _keyValueMap.rend(); ++rIter, ++j) {
            os << rIter->first << "{" << rIter->second << "} ";
        }

    }
    return os.str();
}
}}} // namespace lsst::qserv::loader
