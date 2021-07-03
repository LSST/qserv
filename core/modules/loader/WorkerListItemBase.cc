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
#include "loader/WorkerListItemBase.h"

// System headers
#include <boost/asio.hpp>
#include <iostream>

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.WorkerListBase");
}

namespace lsst {
namespace qserv {
namespace loader {


KeyRange WorkerListItemBase::setRangeString(KeyRange const& strRange) {
    std::lock_guard<std::mutex> lck(_mtx);
    auto oldRange = _range;
    _range = strRange;
     LOGS(_log, LOG_LVL_INFO, "setRangeStr name=" << _wId << " range=" << _range <<
                             " oldRange=" << oldRange);
    return oldRange;
}


std::ostream& WorkerListItemBase::dump(std::ostream& os) const {
    os << "wId=" << _wId;
    os << " UDP=" << getUdpAddress();
    os << " TCP=" << getTcpAddress();
    std::lock_guard<std::mutex> lck(_mtx);
    os << " range("<< _range << ")";
    return os;
}


std::ostream& operator<<(std::ostream& os, WorkerListItemBase const& item) {
    return item.dump(os);
}


}}} // namespace lsst::qserv::loader


