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
#ifndef LSST_QSERV_LOADER_WORKERLISTBASE_H
#define LSST_QSERV_LOADER_WORKERLISTBASE_H

// system headers
#include <atomic>
#include <map>
#include <memory>
#include <mutex>


// Qserv headers
#include "loader/KeyRange.h"
#include "loader/NetworkAddress.h"
#include "loader/Updateable.h"


namespace lsst {
namespace qserv {
namespace loader {

class Central;

class WorkerListItemBase : public std::enable_shared_from_this<WorkerListItemBase> {
public:
    using BasePtr = std::shared_ptr<WorkerListItemBase>;
    using BaseWPtr = std::weak_ptr<WorkerListItemBase>;

    WorkerListItemBase() = delete;
    WorkerListItemBase(WorkerListItemBase const&) = delete;
    WorkerListItemBase& operator=(WorkerListItemBase const&) = delete;

    virtual ~WorkerListItemBase() = default;

    /// @return return the previous range value.
    KeyRange setRangeString(KeyRange const& strRange);

    /// @return the current range.
    KeyRange getRangeString() const {
        std::lock_guard<std::mutex> lck(_mtx);
        return _range;
    }

    NetworkAddress getUdpAddress() const { return _udpAddress.getAddress(); }

    NetworkAddress getTcpAddress() const { return _tcpAddress.getAddress(); }

    /// Set the UDP address to 'addr'. This can only be done once,
    /// so 'addr' needs to be correct.
    /// @return true if the address was set to 'addr'
    bool setUdpAddress(NetworkAddress const& addr) { return _udpAddress.setAddress(addr); }

    /// Set the TCP address to 'addr'. This can only be done once,
    /// so 'addr' needs to be correct.
    /// @return true if the address was set to 'addr'
    bool setTcpAddress(NetworkAddress const& addr) { return _tcpAddress.setAddress(addr); }

    uint32_t getId() const { return _wId; }

    virtual void addDoListItems(Central *central) = 0;

    virtual std::ostream& dump(std::ostream& os) const;

    std::string dump() const {
        std::stringstream os;
        dump(os);
        return os.str();
    }

    friend std::ostream& operator<<(std::ostream& os, WorkerListItemBase const& item);
protected:
    WorkerListItemBase(uint32_t wId) : _wId(wId) {}
    WorkerListItemBase(uint32_t wId,
                       NetworkAddress const& udpAddress,
                       NetworkAddress const& tcpAddress)
             : _wId(wId) {
        setUdpAddress(udpAddress);
        setTcpAddress(tcpAddress);
    }

    uint32_t const _wId; ///< worker id, immutable.
    KeyRange _range;      ///< min and max range for this worker.
    mutable std::mutex _mtx; ///< protects _range. Child classes may use it to protect additional members.

private:
    /// _udpAddress and _tcpAddress only have their values set to a valid value once and then
    /// they remain constant.
    NetworkAddressLatch _udpAddress; ///< empty string indicates address invalid.
    NetworkAddressLatch _tcpAddress; ///< empty string indicates address invalid.
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_WORKERLISTBASE_H
