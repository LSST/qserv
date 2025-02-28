/*
 * LSST Data Management System
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
#ifndef LSST_QSERV_UTIL_TIMEUTILS_H
#define LSST_QSERV_UTIL_TIMEUTILS_H

/**
 * This header declares vairous helpers and utilities to facilitate with
 * time conversions and representations.
 */

// System headers
#include <chrono>
#include <cstdint>
#include <string>

// Qserv headers
#include "global/clock_defs.h"

// This header declarations
namespace lsst::qserv::util {

/**
 * Structure TimeUtils provides the namespace for the utilities.
 */
struct TimeUtils {
    /// @return the current time in milliseconds since Epoch
    static std::uint64_t now();

    /// @return a human-readable timestamp in a format 'YYYY-MM-DD HH:MM:SS.mmm'
    static std::string toDateTimeString(std::chrono::milliseconds const& millisecondsSinceEpoch);

    /// @return a human-readable time in a format 'YYYY-MM-DD HH:MM:SS'
    static std::string timePointToDateTimeString(TIMEPOINT const& point);

    /**
     * @param tp The timepoint to be converted.
     * @return The number of milliseconds since UNIX Epoch
     */
    static std::uint64_t tp2ms(std::chrono::system_clock::time_point const& tp);
};

}  // namespace lsst::qserv::util

#endif  // LSST_QSERV_UTIL_TIMEUTILS_H
