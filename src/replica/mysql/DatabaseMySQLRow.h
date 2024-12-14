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
#ifndef LSST_QSERV_REPLICA_DATABASEMYSQLROW_H
#define LSST_QSERV_REPLICA_DATABASEMYSQLROW_H

/**
 * This header defines class Row which is one of the public classes of
 * the C++ wrapper over the MySQL C library.
 *
 * @see class Connection
 *
 * This class is not normally included directly by user's code.
 */

// System headers
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/mysql/DatabaseMySQLExceptions.h"

// Forward declarations
namespace lsst::qserv::replica {
class ProtocolResponseSqlRow;
class SqlResultSet;
namespace database::mysql {
class Connection;
}  // namespace database::mysql
}  // namespace lsst::qserv::replica

// This header declarations
namespace lsst::qserv::replica::database::mysql {

/**
 * Class Row represents the current row obtained from the last result set.
 * It provides an interface for obtaining values of fields and translating
 * them from the internal MySQL representation into the proposed C++ type
 * system.
 *
 * All type-specific 'get' methods defined in this class will return 'true' and
 * set the value returned for the specified column if the value was not 'NULL'.
 * They will return 'false' otherwise. All methods have two parameters:
 *
 *   columnName - the name of a column
 *   value      - the value (of a type which depends on the method signature)
 *                to be initialized upon the successful completion of a method
 *
 * Methods may also throw the following exceptions:
 *
 *   std::logic_error      - when attempting to use methods of an invalid object
 *   std::invalid_argument - for unknown column names
 *   InvalidTypeError      - when the conversion of row data into a value of
 *                           the requested type is not possible.
 *
 * @note
 *   The validity of each object of this class is limited by the life
 *   span of the database Connection object and a result set of the last
 *   query. Use this object only for short periods of time while iterating over
 *   a result set after each successful invocation of the iterator method
 *   Connection::next().
 *
 * @see Connection::next()
 *
 */
class Row {
public:
    // These classes are allowed to initialize the valid content of rows.
    friend class Connection;
    friend class lsst::qserv::replica::SqlResultSet;

    /**
     * The class encapsulate a raw data pointer and the number of bytes
     * in each column.
     */
    typedef std::pair<char const*, size_t> Cell;

    /**
     * The default constructor will initialize invalid instances of the class.
     *
     * @note
     *   Any attempts to call most (but 'isValid', copy constructor,
     *   assignment operator and destructor) methods of objects constructed
     *   with this state will throw exception std::logic_error. So, make sure
     *   the object is properly set by passing it for initialization to method
     *   Connection::next() when iterating over a result set.
     *
     * @see Connection::next()
     * @see Row::isValid()
     */
    Row();

    Row(Row const& rhs) = default;
    Row& operator=(Row const& rhs) = default;

    ~Row() = default;

    /// @return 'true' of the object has meaningful content
    bool isValid() const { return _name2indexPtr != nullptr; }

    /// @return width of the row
    size_t numColumns() const;

    // These methods will return 'true' if the specified field is NULL

    bool isNull(size_t columnIdx) const;
    bool isNull(std::string const& columnName) const;

    // Type-specific data extractors/converters for values of fields.
    // There are two ways to access the values: either by a relative
    // index of a column in a result set, or by the name of the column.
    // The second method has some extra (though, minor) overhead.

    template <typename T>
    T getAs(size_t columnIdx) const {
        T val;
        if (get(columnIdx, val)) return val;
        throw database::mysql::Error("NULL is not allowed for column index: " + std::to_string(columnIdx));
    }

    template <typename T>
    T getAs(std::string const& columnName) const {
        T val;
        if (get(columnName, val)) return val;
        throw database::mysql::Error("NULL is not allowed for column name: '" + columnName + "'");
    }

    template <typename T>
    T getAs(size_t columnIdx, T const& defaultValue) const {
        T val;
        if (get(columnIdx, val)) return val;
        return defaultValue;
    }

    template <typename T>
    T getAs(std::string const& columnName, T const& defaultValue) const {
        T val;
        if (get(columnName, val)) return val;
        return defaultValue;
    }

    // Strings

    bool get(size_t columnIdx, std::string& value) const;
    bool get(std::string const& columnName, std::string& value) const;

    // Unsigned integer types

    bool get(size_t columnIdx, uint64_t& value) const;
    bool get(size_t columnIdx, uint32_t& value) const;
    bool get(size_t columnIdx, uint16_t& value) const;
    bool get(size_t columnIdx, uint8_t& value) const;

    bool get(std::string const& columnName, uint64_t& value) const;
    bool get(std::string const& columnName, uint32_t& value) const;
    bool get(std::string const& columnName, uint16_t& value) const;
    bool get(std::string const& columnName, uint8_t& value) const;

    // Signed integer types

    bool get(size_t columnIdx, int64_t& value) const;
    bool get(size_t columnIdx, int32_t& value) const;
    bool get(size_t columnIdx, int16_t& value) const;
    bool get(size_t columnIdx, int8_t& value) const;

    bool get(std::string const& columnName, int64_t& value) const;
    bool get(std::string const& columnName, int32_t& value) const;
    bool get(std::string const& columnName, int16_t& value) const;
    bool get(std::string const& columnName, int8_t& value) const;

    // Floating point types

    bool get(size_t columnIdx, float& value) const;
    bool get(size_t columnIdx, double& value) const;

    bool get(std::string const& columnName, float& value) const;
    bool get(std::string const& columnName, double& value) const;

    // Other types

    bool get(size_t columnIdx, bool& value) const;
    bool get(std::string const& columnName, bool& value) const;

    /**
     * @param columnIdx the index of a column
     * @return reference to the data cell for the column
     */
    Cell const& getDataCell(size_t columnIdx) const;

    /**
     * @param columnName the name of a column
     * @return reference to the data cell for the column
     */
    Cell const& getDataCell(std::string const& columnName) const;

    /**
     * Fill a Protobuf object representing a row.
     * @param ptr a valid pointer to the Protobuf object to be populated.
     * @param std::invalid_argument if the input pointer is 0
     */
    void exportRow(ProtocolResponseSqlRow* ptr) const;

    /**
     * Convert the current row into a JSON object.
     * @return a JSON object representing the current row
     */
    nlohmann::json toJson() const;

private:
    /**
     * Mapping column names to the indexes
     *
     * @note
     *   If the pointer is set to 'nullptr' then the object is not
     *   in the valid state. The valid state is set by class Connection
     *   when iterating over a result set.
     */
    std::map<std::string, size_t> const* _name2indexPtr;

    /// Mapping column indexes to the raw data cells
    std::vector<Cell> _index2cell;
};

}  // namespace lsst::qserv::replica::database::mysql

#endif  // LSST_QSERV_REPLICA_DATABASEMYSQLROW_H
