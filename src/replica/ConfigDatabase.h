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
#ifndef LSST_QSERV_REPLICA_CONFIGDATABASE_H
#define LSST_QSERV_REPLICA_CONFIGDATABASE_H

// System headers
#include <cstdint>
#include <list>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/Common.h"
// Forward declarations
namespace lsst::qserv::replica {
class DatabaseFamilyInfo;
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class DatabaseInfo encapsulates various parameters describing databases.
 */
class DatabaseInfo {
public:
    std::string name;    // The name of a database.
    std::string family;  // The name of the database family.

    bool isPublished = false;  // The status of the database.
    uint64_t createTime = 0;
    uint64_t publishTime = 0;

    std::vector<std::string> partitionedTables;  // The names of the partitioned tables.
    std::vector<std::string> regularTables;      // The list of fully replicated tables.

    /// Table schema (optional).
    std::map<std::string,  // table name
             std::list<SqlColDef>>
            columns;

    /// @return The names of all tables.
    std::vector<std::string> tables() const;

    /// @return The names of the "director" tables.
    std::vector<std::string> directorTables() const;

    /// Reverse dependencies from the "dependent" tables to the corresponding
    /// "directors". The "director" tables also have entries here, although they are
    /// guaranteed to have the empty values. The "dependent" tables are guaranteed
    /// to have non-empty values.
    std::map<std::string,  // The table name (partitioned tables only!).
             std::string>
            directorTable;  // The name of the Qserv "director" table if any.

    /// Each partitioned table will have an entry here. The key is required for
    /// both "director" and the "dependent" tables.
    std::map<std::string,  // The table name (partitioned tables only!).
             std::string>
            directorTableKey;  // The name of the table's key representing object identifiers.
                               // NOTES: (1) In the "director" table this is the unique
                               // PK identifying table rows, (2) In the "dependent" tables
                               // the key represents the FK associated with the corresponding
                               // PK of the "director" table.

    // Names of special columns of the partitioned tables. Each partitioned tables has
    // an entry in both maps. Non-empty values are required for the "director" tables.
    // Empty values are allowed for the "dependent" tables since they must have
    // the direct association with the corresponding "director" tables via
    // the FK -> PK relation.
    // Table names are used as keys in the dictionaries defined below.

    std::map<std::string, std::string> latitudeColName;
    std::map<std::string, std::string> longitudeColName;

    // Publishing status of the tables.
    // Table names are used as keys in the dictionaries defined below.
    std::map<std::string, bool> tableIsPublished;
    std::map<std::string, uint64_t> tableCreateTime;
    std::map<std::string, uint64_t> tablePublishTime;

    /**
     * Construct an empty unpublished database object for the given name and the family.
     * @note The create time of the database will be set to the current time.
     * @param name The name of the database.
     * @param family The name of the database family.
     * @return The initialized database descriptor.
     */
    static DatabaseInfo create(std::string const& name, std::string const family);

    /**
     * Construct from JSON.
     * @note Passing an empty JSON object or json::null object as a value of the optional
     *   parameter 'families' will disable the optional step of the family validation.
     *   This is safe to do once if the object is pulled from the transient state
     *   of the configuration which is guaranteed to be complete. In other cases, where
     *   the input provided by a client the input needs to be sanitized.
     * @param obj The JSON object to be used of a source of the worker's state.
     * @param families The collection of the database families to be used for validating
     *   the database definition.
     * @return The initialized database descriptor.
     * @throw std::invalid_argument If the input object can't be parsed, or if it has
     *   incorrect schema.
     */
    static DatabaseInfo parse(nlohmann::json const& obj,
                              std::map<std::string, DatabaseFamilyInfo> const& families);

    /// @return The JSON representation of the object.
    nlohmann::json toJson() const;

    /// @param table The name of a table.
    /// @return 'true' if the table (of either kind) exists.
    bool hasTable(std::string const& table) const;

    /// Validate parameters of a new table, then add it to the database.
    /// @throw std::invalid_argument If the input parameters are incorrect or inconsistent.
    void addTable(std::string const& table, std::list<SqlColDef> const& columns_ = std::list<SqlColDef>(),
                  bool isPartitioned = false, bool isDirector = false,
                  std::string const& directorTable_ = std::string(),
                  std::string const& directorTableKey_ = std::string(),
                  std::string const& latitudeColName_ = std::string(),
                  std::string const& longitudeColName_ = std::string());

    /// Remove the specified table from the database
    /// @throw std::invalid_argument If the empty string is passed as a value of
    ///   the parameter 'table', or the table doesn't exist.
    void removeTable(std::string const& table);

    /// @param The name of a table to be located and inspected
    /// @return 'true' if the table was found and it's 'partitioned'
    /// @throw std::invalid_argument if no such table is known
    bool isPartitioned(std::string const& table) const;

    /// @param The name of a table to be located and inspected
    /// @return 'true' if the table was found and it's the 'partitioned' and the 'director' table
    /// @throw std::invalid_argument if no such table is known
    bool isDirector(std::string const& table) const;

    /// @return The table schema in format which is suitable for CSS.
    /// @throws std::out_of_range If the table is unknown.
    std::string schema4css(std::string const& table) const;
};

std::ostream& operator<<(std::ostream& os, DatabaseInfo const& info);

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_CONFIGDATABASE_H
