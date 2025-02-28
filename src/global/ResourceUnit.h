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

#ifndef LSST_QSERV_RESOURCEUNIT_H
#define LSST_QSERV_RESOURCEUNIT_H

// System headers
#include <map>
#include <string>

// Qserv headers
#include "global/constants.h"  // For DUMMY_CHUNK

namespace lsst::qserv {

/// This class is used to store the database and chunk id of a resource.
class ResourceUnit {
public:
    class Checker;
    enum UnitType { GARBAGE, DBCHUNK, UNKNOWN, QUERY };

    ResourceUnit() = default;
    ResourceUnit(ResourceUnit const&) = default;
    ResourceUnit& operator=(ResourceUnit const&) = default;
    ~ResourceUnit() = default;

    /// @return the constructed path.
    std::string path() const;

    // Retrieve elements of the path.

    UnitType unitType() const { return _unitType; }
    std::string const& db() const { return _db; }
    int chunk() const { return _chunk; }

    /// @return the path prefix element for a given request type.
    static std::string prefix(UnitType const& r);

    /// @return the path of the database/chunk resource
    static std::string makePath(int chunk, std::string const& db);

    // Setup a path of a certain type.
    void setAsDbChunk(std::string const& db, int chunk = DUMMY_CHUNK);

private:
    UnitType _unitType = UnitType::GARBAGE;  //< Type of unit
    std::string _db;                         //< for DBCHUNK type
    int _chunk = -1;                         //< for DBCHUNK type

    static char const _pathSep = '/';

    friend std::ostream& operator<<(std::ostream& os, ResourceUnit const& ru);
};

}  // namespace lsst::qserv

#endif  // LSST_QSERV_RESOURCEUNIT_H
