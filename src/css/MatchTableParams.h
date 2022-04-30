// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014 LSST Corporation.
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

#ifndef LSST_QSERV_CSS_MATCHTABLEPARAMS_H
#define LSST_QSERV_CSS_MATCHTABLEPARAMS_H

#include <string>

namespace lsst::qserv::css {

/// A container for match-table metadata.
struct MatchTableParams {
    MatchTableParams() : angSep(0.0) {}
    MatchTableParams(std::string const& dirTable1_, std::string const& dirColName1_,
                     std::string const& dirTable2_, std::string const& dirColName2_,
                     std::string const& flagColName_, double angSep_)
            : dirTable1(dirTable1_),
              dirColName1(dirColName1_),
              dirTable2(dirTable2_),
              dirColName2(dirColName2_),
              flagColName(flagColName_),
              angSep(angSep_) {}

    std::string dirTable1;    ///< First director-table involved in match.
    std::string dirColName1;  ///< The column used to join with dirTable1.
    std::string dirTable2;    ///< Second director-table involved in match.
    std::string dirColName2;  ///< The column used to join with dirTable2.
    std::string flagColName;  ///< Match-flags column name.
    double angSep;            ///< Max angular separation between objects in director tables.

    /// Returns true if table is a match table
    bool isMatchTable() const { return not dirTable1.empty(); }
};

}  // namespace lsst::qserv::css

#endif  // LSST_QSERV_CSS_MATCHTABLEPARAMS_H
