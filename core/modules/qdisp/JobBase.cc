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
#include "qdisp/JobBase.h"

// System headers
#include <iostream>

// Qserv headers


// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.JobBase");
}

namespace lsst {
namespace qserv {
namespace qdisp {


std::ostream& JobBase::dumpOS(std::ostream &os) const {
    os << "JobBase no data members";
    return os;
}

std::string JobBase::dump() const {
    std::ostringstream os;
    dumpOS(os);
    return os.str();
}


std::ostream& operator<<(std::ostream &os, JobBase const& jb) {
    return jb.dumpOS(os);
}


}}} // namespace lsst::qserv::qdisp


