// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2016 LSST Corporation.
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

#ifndef LSST_QSERV_PROTOJSON_SCANTABLEINFO_H
#define LSST_QSERV_PROTOJSON_SCANTABLEINFO_H

// System headers
#include <memory>
#include <string>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

namespace lsst::qserv::protojson {

/// Structure to store shared scan information for a single table.
///
struct ScanTableInfo {
    using ListOf = std::vector<ScanTableInfo>;

    ScanTableInfo() = default;
    ScanTableInfo(std::string const& db_, std::string const& table_) : db(db_), table(table_) {}
    ScanTableInfo(std::string const& db_, std::string const& table_, bool lockInMemory_, int scanRating_)
            : db{db_}, table{table_}, lockInMemory{lockInMemory_}, scanRating{scanRating_} {}

    ScanTableInfo(ScanTableInfo const&) = default;

    int compare(ScanTableInfo const& rhs) const;

    std::string db;
    std::string table;
    bool lockInMemory{false};
    int scanRating{0};
};

/// This class stores information about database table ratings for
/// a user query.
class ScanInfo {
public:
    using Ptr = std::shared_ptr<ScanInfo>;

    /// Threshold priority values. Scan priorities are not limited to these values.
    enum Rating { FASTEST = 0, FAST = 10, MEDIUM = 20, SLOW = 30, SLOWEST = 100 };

    ScanInfo() = default;
    ScanInfo(ScanInfo const&) = default;

    static Ptr create() { return Ptr(new ScanInfo()); }

    static Ptr createFromJson(nlohmann::json const& ujJson);

    /// Return a json version of the contents of this class.
    nlohmann::json serializeJson() const;

    void sortTablesSlowestFirst();
    int compareTables(ScanInfo const& rhs);

    ScanTableInfo::ListOf infoTables;
    int scanRating{Rating::FASTEST};
};

std::ostream& operator<<(std::ostream& os, ScanTableInfo const& tbl);
std::ostream& operator<<(std::ostream& os, ScanInfo const& info);

}  // namespace lsst::qserv::protojson

#endif  // LSST_QSERV_PROTOJSON_SCANTABLEINFO_H
