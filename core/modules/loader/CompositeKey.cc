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
#include "loader/CompositeKey.h"

// System headers
#include <iostream>


// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.CompositeKey");
}

namespace lsst {
namespace qserv {
namespace loader {


CompositeKey const CompositeKey::minValue(0,"");


void CompositeKey::dump(std::ostream& os) const {
    os << "CKey(" << kInt << ", " << kStr << ")";
}


std::string CompositeKey::dump() const {
    std::stringstream os;
    dump(os);
    return os.str();
}


std::ostream& operator<<(std::ostream& os, CompositeKey const& cKey) {
    cKey.dump(os);
    return os;
}


}}} // namespace lsst::qserv::loader

