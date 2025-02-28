// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2015 LSST Corporation.
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

#include "global/ResourceUnit.h"

// System headers
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace lsst::qserv {

std::string ResourceUnit::path() const {
    std::stringstream ss;
    ss << _pathSep << prefix(_unitType);
    switch (_unitType) {
        case GARBAGE:
            return "/GARBAGE";
        case DBCHUNK:
            ss << _pathSep << _db;
            if (_chunk != -1) {
                ss << _pathSep << _chunk;
            }
            break;
        case UNKNOWN:
            ss << _pathSep << "UNKNOWN_RESOURCE_UNIT";
            break;
        default:
            ::abort();
            break;
    }
    return ss.str();
}

std::string ResourceUnit::prefix(UnitType const& r) {
    switch (r) {
        case DBCHUNK:
            return "chk";
        case UNKNOWN:
            return "UNKNOWN";
        case QUERY:
            return "query";
        case GARBAGE:
        default:
            return "GARBAGE";
    }
}

std::string ResourceUnit::makePath(int chunk, std::string const& db) {
    return _pathSep + prefix(UnitType::DBCHUNK) + _pathSep + db + _pathSep + std::to_string(chunk);
}

void ResourceUnit::setAsDbChunk(std::string const& db, int chunk) {
    _unitType = DBCHUNK;
    _db = db;
    _chunk = chunk;
}

std::ostream& operator<<(std::ostream& os, ResourceUnit const& ru) {
    return os << "Resource(" << ru.path() << ")";
}

}  // namespace lsst::qserv
