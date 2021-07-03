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
#ifndef LSST_QSERV_LOADER_CENTRAL_WORKER_H
#define LSST_QSERV_LOADER_CENTRAL_WORKER_H

// system headers
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <thread>
#include <vector>

// Qserv headers
#include "loader/Central.h"
#include "loader/DoList.h"
#include "loader/Neighbor.h"
#include "loader/ServerTcpBase.h"
#include "loader/WorkerConfig.h"


namespace lsst {
namespace qserv {

namespace proto {
class WorkerKeysInfo;
}

namespace loader {


class CentralWorkerDoListItem;


class CentralWorker : public Central {
public:
    typedef std::pair<CompositeKey, ChunkSubchunk> CompKeyPair;

    enum SocketStatus {
        VOID0 = 0,
        STARTING1,
        ESTABLISHED2
    };

    enum Direction {
        NONE0 = 0,
        TORIGHT1,
        FROMRIGHT2
    };

    CentralWorker(boost::asio::io_service& ioService, boost::asio::io_context& io_context_,
                      std::string const& hostName_, WorkerConfig const& cfg);

    /// Open the UDP and TCP ports and start monitoring. This can throw boost::system::system_error.
    void start();

    ~CentralWorker() override;

    WWorkerList::Ptr getWorkerList() const { return _wWorkerList; }

    std::string const& getHostName() const { return _hostName; }
    int getUdpPort() const { return _udpPort; }
    int getTcpPort() const { return _tcpPort; }

    uint32_t getOurId() const {
        std::lock_guard<std::mutex> lck(_ourIdMtx);
        return _ourId;
    }

    /// Insert the keys in keyList into _keyValueMap, adjusting ranges
    /// as needed.
    /// @parameter mustSetMin should be set true if this is not the left
    ///                       most worker. It causes the minimum value to
    ///                       be set to the smallest key in _keyValueMap.
    void insertKeys(std::vector<CompKeyPair> const& keyList, bool mustSetMin);

    /// @Return a list of the smallest keys from _keyValueMap. The keys are removed from
    ///         from the map. Put keys are also put in _transferList in case the shift fails
    ///         and they need to be put back into _keyValueMap.
    /// TODO add argument for smallest or largest and code to build list from smallest or largest keys.
    StringElement::UPtr buildKeyList(int keysToShift);

    /////////////////////////////////////////////////////////////////////////////////
    /// Methods to handle messages received from other servers.
    /// 'inMsg' contains information about the originator of a request
    ///         and the type of message.
    /// 'data'  contains the message data.

    /// Update our range with data from our left neighbor.
    /// Our minimum key is their maximum key(exclusive).
    /// @returns what it thinks the range of the left neighbor should be.
    KeyRange updateRangeWithLeftData(KeyRange const& strRange);

    /// Receive our name from the master. Returns true if successful.
    bool workerInfoReceive(BufferUdp::Ptr const&  data);

    /// Receive a request to insert a key value pair.
    /// If the key value pair could not be inserted, it tries to forward the request appropriately.
    /// @Returns true if the request could be parsed.
    bool workerKeyInsertReq(LoaderMsg const& inMsg, BufferUdp::Ptr const&  data);

    /// Receive a request to lookup a key value.
    /// If the key is not within this worker's range, it tries to forward the request appropriately.
    /// @Returns true if the request could be parsed.
    bool workerKeyInfoReq(LoaderMsg const& inMsg, BufferUdp::Ptr const& data);

    /// Receive a request for information about this worker's keys, how many key-value pairs are
    /// stored and the range of keys the worker is responsible for.
    /// @Returns true if the message could be parsed.
    bool workerWorkerKeysInfoReq(LoaderMsg const& inMsg, BufferUdp::Ptr const& data);

    /// Receive a message from the master providing the wId of our right neighbor.
    bool workerWorkerSetRightNeighbor(LoaderMsg const& inMsg, BufferUdp::Ptr const& data);

    /// Receive a message from the master providing the wId of our left neighbor.
    bool workerWorkerSetLeftNeighbor(LoaderMsg const& inMsg, BufferUdp::Ptr const& data);

    std::string getOurLogId() const override;

    std::unique_ptr<proto::WorkerKeysInfo> _workerKeysInfoBuilder(); // TODO make private
    void setNeighborInfoLeft(uint32_t wId, int keyCount, KeyRange const& range);  // TODO make private


    /// @Return a string describing the first and last 'count' keys. count=0 dumps all keys.
    std::string dumpKeysStr(unsigned int count);

    /// Called when our right neighbor indicates it is done with a shift FROMRIGHT
    void finishShiftFromRight();

    /// Called when there has been a problem with shifting with the left neighbor and changes
    /// to _keyValueMap need to be undone.
    void cancelShiftsWithLeftNeighbor();

    /// Send a bad message for testing purposes.
    void testSendBadMessage();

    friend CentralWorkerDoListItem;

private:
    /// Add the _monitor DoListItem to the DoList
    void _startMonitoring();

    /// Contact the master so it can provide this worker with an id. The master
    /// will activate this worker when it is needed at a later time.
    void _registerWithMaster();

    /// @return true if our worker id is not valid.
    bool _isOurIdInvalid() const {
        std::lock_guard<std::mutex> lck(_ourIdMtx);
        return _ourIdInvalid;
    }

    /// If ourId is invalid, set our id to id.
    bool _setOurId(uint32_t id);

    /// Disable this worker. Only to be used if the master has deemed
    /// this worker as too unreliable and replaced it.
    void _masterDisable();

    /// This function is run to monitor this worker's status. It is used to
    /// register with the master, connect and control shifting with the
    /// the right neighbor.
    void _monitor();

    /// Use the information from our right neighbor to set our key range.
    bool _determineRange();

    /// If this worker has significantly more or fewer keys than its right neighbor,
    /// shift keys between them to make a more even distribution.
    /// @Return true if data was shifted with the right neighbor.
    bool _shiftIfNeeded(std::lock_guard<std::mutex> const& rightMtxLG);

    /// Attempt to shift keys to or from the right neighbor.
    /// @parameter keysToShift is number of keys to shift.
    /// @parameter direction is TO or FROM the right neighbor.
    void _shift(Direction direction, int keysToShift);

    void _workerInfoReceive(std::unique_ptr<proto::WorkerListItem>& protoBuf); ///< see workerInfoReceive()

    /// See workerKeyInsertReq(...)
    void _workerKeyInsertReq(LoaderMsg const& inMsg, std::unique_ptr<proto::KeyInfoInsert>& protoBuf);
    /// Forward a workerKeyInsertReq to an appropriate worker.
    void _forwardKeyInsertRequest(NetworkAddress const& targetAddr, LoaderMsg const& inMsg,
                                  std::unique_ptr<proto::KeyInfoInsert>& protoData);

    /// See workerKeyInfoReq
    void _workerKeyInfoReq(LoaderMsg const& inMsg, std::unique_ptr<proto::KeyInfoInsert>& protoBuf);
    /// Forward a workerKeyInfoReq to an appropriate worker.
    void _forwardKeyInfoRequest(WWorkerListItem::Ptr const& target, LoaderMsg const& inMsg,
                                std::unique_ptr<proto::KeyInfoInsert> const& protoData);

    /// See workerWorkerKeysInfoReq(...)
    void _workerWorkerKeysInfoReq(LoaderMsg const& inMsg);
    /// Send information about our keys (range, number of pairs) to 'nAddr'.
    void _sendWorkerKeysInfo(NetworkAddress const& nAddr, uint64_t msgId);

    void _removeOldEntries(); ///< remove old entries from _recentAdds

    /// Connect to the right neighbor. Must hold _rightMtx in the lock.
    void _rightConnect(std::lock_guard<std::mutex> const& rightMtxLG);
    ///< Disconnect from the right neighbor. Must hold _rightMtx in the lock.
    void _rightDisconnect(std::lock_guard<std::mutex> const& rightMtxLG);

    void _cancelShiftsWithRightNeighbor(); ///< Cancel shifts to/from the right neighbor.
    void _finishShiftToRight(); ///< The shift to the right neighbor is complete, cleanup.

    const std::string        _hostName;
    const int                _udpPort;
    const int                _tcpPort;
    boost::asio::io_context& _ioContext;

    WWorkerList::Ptr _wWorkerList{std::make_shared<WWorkerList>(this)}; ///< Maps of workers.

    bool _ourIdInvalid{true}; ///< true until our id has been set by the master.
    std::atomic<uint32_t> _ourId{0}; ///< id given by the master, 0 is invalid id.
    mutable std::mutex _ourIdMtx; ///< protects _ourIdInvalid, _ourId


    KeyRange _keyRange; ///< range for this worker
    std::atomic<bool> _rangeChanged{false};
    std::map<CompositeKey, ChunkSubchunk> _keyValueMap;
    std::deque<std::chrono::system_clock::time_point> _recentAdds; ///< track how many keys added recently.
    std::chrono::milliseconds _recentAddLimit; ///< After this period of time, additions are no longer recent.
    std::mutex _idMapMtx; ///< protects _strRange, _keyValueMap,
                          ///< _recentAdds, _transferListToRight, _transferListFromRight

    Neighbor _neighborLeft{Neighbor::LEFT};
    Neighbor _neighborRight{Neighbor::RIGHT};

    ServerTcpBase::Ptr _tcpServer; // For our right neighbor to connect to us.

    std::mutex _rightMtx;
    SocketStatus _rightConnectStatus{VOID0};
    std::shared_ptr<AsioTcp::socket>  _rightSocket;

    std::atomic<bool> _shiftAsClientInProgress{false}; ///< True when shifting to or from right neighbor.

    /// Shift if a node has % more than it's neighbor. the percentage threshold is expressed
    /// as a decimal, so 1.1 would be 10% more than neighbor or 110%.
    double _thresholdNeighborShift;

    /// Maximum number of keys to shift in one iteration. 10000 may be reasonable.
    /// An iteration would be transfer, insert, and verify range. During the
    /// insert phase, the mutex is locked preventing key inserts and lookups.
    /// Using smaller values locks the mutex for more periods of time but each
    /// period is shorter and lookups can occur during the gaps.
    /// Too big a value, and the maps will be paralyzed for a long time during inserts.
    /// Too small and shift operations will take significantly longer.
    int _maxKeysToShift;
    std::vector<CompKeyPair> _transferListToRight; ///< List of items being transfered to right
    /// List of items being transfered to our left neighbor. (answering neighbor's FromRight request)
    std::vector<CompKeyPair> _transferListWithLeft;

    /// The DoListItem that makes sure _monitor() is run.
    std::shared_ptr<CentralWorkerDoListItem> _centralWorkerDoListItem;
};





}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_CENTRAL_WORKER_H

