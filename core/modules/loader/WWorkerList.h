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
#ifndef LSST_QSERV_LOADER_WWORKERLIST_H
#define LSST_QSERV_LOADER_WWORKERLIST_H

// system headers
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

// Qserv headers
#include "loader/BufferUdp.h"
#include "loader/DoList.h"
#include "loader/WorkerListItemBase.h"


namespace lsst {
namespace qserv {
namespace loader {

class CentralWorker;
class LoaderMsg;


/// Standard information for a single worker, IP address, key range, timeouts.
class WWorkerListItem : public WorkerListItemBase {
public:
    using Ptr = std::shared_ptr<WWorkerListItem>;
    using WPtr = std::weak_ptr<WWorkerListItem>;

    static WWorkerListItem::Ptr create(uint32_t wId, CentralWorker *central) {
        return WWorkerListItem::Ptr(new WWorkerListItem(wId, central));
    }

    WWorkerListItem() = delete;
    WWorkerListItem(WWorkerListItem const&) = delete;
    WWorkerListItem& operator=(WWorkerListItem const&) = delete;

    virtual ~WWorkerListItem() = default;

    /// @return a properly typed shared pointer to this object.
    Ptr getThis() {
        Ptr ptr = std::static_pointer_cast<WWorkerListItem>(shared_from_this());
        return ptr;
    }

    void addDoListItems(Central *central) override;

    util::CommandTracked::Ptr createCommandWorkerInfoReq(CentralWorker* centralW);

    /// @return true if this item is equal to other.
    bool equal(WWorkerListItem &other) const;

    /// @return true if 'key' can be found in this item's map.
    bool containsKey(CompositeKey const& key) const;

private:
    WWorkerListItem(uint32_t wId, CentralWorker* central) : WorkerListItemBase(wId), _central(central) {}

    CentralWorker* _central;

    struct WorkerNeedsMasterData : public DoListItem {
        WorkerNeedsMasterData(WWorkerListItem::Ptr const& wWorkerListItem_, CentralWorker* central_) :
            wWorkerListItem(wWorkerListItem_), central(central_) {}
        WWorkerListItem::WPtr wWorkerListItem;
        CentralWorker* central;
        util::CommandTracked::Ptr createCommand() override;
    };
    DoListItem::Ptr _workerUpdateNeedsMasterData;
};



class WWorkerList : public DoListItem {
public:
    using Ptr = std::shared_ptr<WWorkerList>;

    WWorkerList(CentralWorker* central) : _central(central) {}
    WWorkerList() = delete;
    WWorkerList(WWorkerList const&) = delete;
    WWorkerList& operator=(WWorkerList const&) = delete;

    virtual ~WWorkerList() = default;

    /// Receive a list of workers from the master.
    bool workerListReceive(BufferUdp::Ptr const& data);

    bool equal(WWorkerList& other) const;

    util::CommandTracked::Ptr createCommand() override;
    util::CommandTracked::Ptr createCommandWorker(CentralWorker* centralW);

    ////////////////////////////////////////////
    /// Nearly the same on Worker and Master
    size_t getIdMapSize() {
        std::lock_guard<std::mutex> lck(_mapMtx);
        return _wIdMap.size();
    }
    WWorkerListItem::Ptr getWorkerWithId(uint32_t id) {
        std::lock_guard<std::mutex> lck(_mapMtx);
        auto iter = _wIdMap.find(id);
        if (iter == _wIdMap.end()) { return nullptr; }
        return iter->second;
    }

    void updateEntry(uint32_t wId,
                     std::string const& ipUdp, int portUdp, int portTcp,
                     KeyRange& strRange);
    WWorkerListItem::Ptr findWorkerForKey(CompositeKey const& key);

    std::string dump() const;

protected:
    void _flagListChange();

    CentralWorker* _central;
    std::map<uint32_t, WWorkerListItem::Ptr> _wIdMap; ///< worker id map
    std::map<NetworkAddress, WWorkerListItem::Ptr> _ipMap;
    std::map<KeyRange, WWorkerListItem::Ptr> _rangeMap;
    bool _wListChanged{false}; ///< true if the list has changed
    uint32_t _totalNumberOfWorkers{0}; ///< total number of workers according to the master.
    mutable std::mutex _mapMtx; ///< protects _wIdMap, _ipMap, _rangeMap, _wListChanged
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_WWORKERLIST_H
