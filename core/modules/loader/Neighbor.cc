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

// Class header
#include "loader/Neighbor.h"

// System headers

// Third-party headers

// Qserv headers

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.Neighbor");
}

namespace lsst {
namespace qserv {
namespace loader {

void Neighbor::setId(uint32_t id) {
    std::lock_guard<std::mutex> lck(_nMtx);
    if (_id != id) {
        LOGS(_log, LOG_LVL_INFO, getTypeStr() << "Neighbor changing id from(" << _id <<") to(" << id << ")");
        _established = false;
        _addressTcp.reset(new NetworkAddress("", -1));
    }
    _id = id;
}


}}} // namespace lsst::qserv::loader
