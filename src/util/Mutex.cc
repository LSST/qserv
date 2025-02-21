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
 */

// Class header
#include "util/Mutex.h"

// System headers
#include <cassert>

// Qserv headers
#include "lsst/log/Log.h"
#include "util/IterableFormatter.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.util.Mutex");
}

namespace lsst::qserv::util {

mutex Mutex::_lockedIdMtx;
set<unsigned int> Mutex::_lockedId;

void Lock::_lock() {
    if (!_context.empty()) {
        LOGS(_log, LOG_LVL_TRACE,
             _context << "  LOCK[" << _mutex.id() << "]:1 "
                      << "  LOCKED: " << util::printable(Mutex::lockedId(), "", "", " "));
    }
    assert(!_mutex.lockedByThread());
    _mutex.lock();
    if (!_context.empty()) {
        LOGS(_log, LOG_LVL_TRACE,
             _context << "  LOCK[" << _mutex.id() << "]:2 "
                      << "  LOCKED: " << util::printable(Mutex::lockedId(), "", "", " "));
    }
}

void Lock::_unlock() {
    if (!_context.empty()) {
        LOGS(_log, LOG_LVL_TRACE,
             _context << "  LOCK[" << _mutex.id() << "]:3 "
                      << "  LOCKED: " << util::printable(Mutex::lockedId(), "", "", " "));
    }
    _mutex.unlock();
}

}  // namespace lsst::qserv::util
