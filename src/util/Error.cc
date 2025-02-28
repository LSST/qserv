// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
#include "util/Error.h"

// System headers
#include <sstream>

// LSST headers
#include "lsst/log/Log.h"

namespace {  // File-scope helpers

LOG_LOGGER _log = LOG_GET("lsst.qserv.util.Error");

}  // namespace

namespace lsst::qserv::util {

Error::Error(int code, std::string const& msg, int status, bool logLvlErr)
        : _code(code), _msg(msg), _status(status) {
    if (_code != ErrorCode::NONE || _msg != "" || _status != ErrorCode::NONE) {
        // Flushing output as it is likely that this exception will not be caught.
        int logLvl = (logLvlErr) ? LOG_LVL_ERROR : LOG_LVL_TRACE;
        LOGS(_log, logLvl, "Error " << *this << std::endl);
    }
}

/** Overload output operator for this class
 *
 * @param out
 * @param multiError
 * @return an output stream
 */
std::ostream& operator<<(std::ostream& out, Error const& error) {
    out << "[" << error._code << "] " << error._msg;
    return out;
}

}  // namespace lsst::qserv::util
