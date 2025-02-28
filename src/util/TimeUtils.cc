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

// Class header
#include "util/TimeUtils.h"

// System headers
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace std;

namespace lsst::qserv::util {

uint64_t TimeUtils::now() {
    return chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now().time_since_epoch())
            .count();
}

string TimeUtils::toDateTimeString(chrono::milliseconds const& millisecondsSinceEpoch) {
    chrono::time_point<chrono::system_clock> const point(millisecondsSinceEpoch);
    auto const timer = chrono::system_clock::to_time_t(point);
    auto broken_time = *localtime(&timer);

    ostringstream ss;
    ss << put_time(&broken_time, "%Y-%m-%d %H:%M:%S");
    ss << '.' << setfill('0') << setw(3) << millisecondsSinceEpoch.count() % 1000;
    return ss.str();
}

uint64_t TimeUtils::tp2ms(chrono::system_clock::time_point const& tp) {
    return chrono::duration_cast<chrono::milliseconds>(tp.time_since_epoch()).count();
}

string TimeUtils::timePointToDateTimeString(TIMEPOINT const& point) {
    auto const timer = chrono::system_clock::to_time_t(point);
    auto broken_time = *localtime(&timer);

    ostringstream ss;
    ss << put_time(&broken_time, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

}  // namespace lsst::qserv::util
