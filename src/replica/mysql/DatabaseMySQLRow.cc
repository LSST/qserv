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
#include "replica/mysql/DatabaseMySQLRow.h"

// System headers
#include <sstream>

// Third party headers
#include "boost/lexical_cast.hpp"

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "util/String.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DatabaseMySQL");

using Row = lsst::qserv::replica::database::mysql::Row;
using InvalidTypeError = lsst::qserv::replica::database::mysql::InvalidTypeError;

template <typename K>
bool getAsString(Row const& row, K key, string& value) {
    Row::Cell const& cell = row.getDataCell(key);
    if (cell.first) {
        value = string(cell.first);
        return true;
    }
    return false;
}

template <typename K, class T>
bool getAsNumber(Row const& row, K key, T& value) {
    try {
        Row::Cell const& cell = row.getDataCell(key);
        if (cell.first) {
            value = boost::lexical_cast<T>(cell.first, cell.second);
            return true;
        }
        return false;
    } catch (boost::bad_lexical_cast const& ex) {
        ostringstream os;
        os << key;
        throw InvalidTypeError("DatabaseMySQL::" + string(__func__) +
                               "<K,T>  type conversion failed for key: " + os.str());
    }
}

}  // namespace

namespace lsst::qserv::replica::database::mysql {

///////////////////////////////////////
//                Row                //
///////////////////////////////////////

Row::Row() : _name2indexPtr(nullptr) {}

size_t Row::numColumns() const {
    if (not isValid()) {
        throw logic_error("Row::" + string(__func__) + "  the object is not valid");
    }
    return _index2cell.size();
}

bool Row::isNull(size_t columnIdx) const { return not getDataCell(columnIdx).first; }
bool Row::isNull(string const& columnName) const { return not getDataCell(columnName).first; }

bool Row::get(size_t columnIdx, string& value) const { return ::getAsString(*this, columnIdx, value); }
bool Row::get(string const& columnName, string& value) const {
    return ::getAsString(*this, columnName, value);
}

bool Row::get(size_t columnIdx, uint64_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, uint32_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, uint16_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, uint8_t& value) const { return ::getAsNumber(*this, columnIdx, value); }

bool Row::get(string const& columnName, uint64_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, uint32_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, uint16_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, uint8_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}

bool Row::get(size_t columnIdx, int64_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, int32_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, int16_t& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, int8_t& value) const { return ::getAsNumber(*this, columnIdx, value); }

bool Row::get(string const& columnName, int64_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, int32_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, int16_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, int8_t& value) const {
    return ::getAsNumber(*this, columnName, value);
}

bool Row::get(size_t columnIdx, float& value) const { return ::getAsNumber(*this, columnIdx, value); }
bool Row::get(size_t columnIdx, double& value) const { return ::getAsNumber(*this, columnIdx, value); }

bool Row::get(string const& columnName, float& value) const {
    return ::getAsNumber(*this, columnName, value);
}
bool Row::get(string const& columnName, double& value) const {
    return ::getAsNumber(*this, columnName, value);
}

bool Row::get(size_t columnIdx, bool& value) const {
    uint8_t number;
    if (::getAsNumber(*this, columnIdx, number)) {
        value = number != '0';
        return true;
    }
    return false;
}

bool Row::get(string const& columnName, bool& value) const {
    uint8_t number;
    if (::getAsNumber(*this, columnName, number)) {
        value = number != '0';
        return true;
    }
    return false;
}

Row::Cell const& Row::getDataCell(size_t columnIdx) const {
    string const context = "Row::" + string(__func__) + "  ";

    if (not isValid()) {
        throw logic_error(context + "the object is not valid");
    }
    if (columnIdx >= _index2cell.size()) {
        throw invalid_argument(context + "the column index '" + to_string(columnIdx) +
                               "' is not in the result set");
    }
    return _index2cell.at(columnIdx);
}

Row::Cell const& Row::getDataCell(string const& columnName) const {
    string const context = "Row::" + string(__func__) + "  ";
    if (not isValid()) {
        throw logic_error(context + "the object is not valid");
    }
    auto itr = _name2indexPtr->find(columnName);
    if (_name2indexPtr->end() == itr) {
        throw invalid_argument(context + "the column '" + columnName + "' is not in the result set");
    }
    return _index2cell.at(itr->second);
}

void Row::exportRow(ProtocolResponseSqlRow* ptr) const {
    string const context = "Row::" + string(__func__) + "  ";
    if (not isValid()) {
        throw logic_error(context + "the object is not valid");
    }
    if (nullptr == ptr) {
        throw invalid_argument(context + "null pointer passed as a parameter");
    }
    for (auto&& cell : _index2cell) {
        auto&& str = cell.first;
        if (nullptr == str) {
            ptr->add_cells(string());
            ptr->add_nulls(true);
        } else {
            ptr->add_cells(cell.first, cell.second);
            ptr->add_nulls(false);
        }
    }
}

json Row::toJson() const {
    string const context = "Row::" + string(__func__) + "  ";
    if (not isValid()) {
        throw logic_error(context + "the object is not valid");
    }
    json result = json::object();
    result["cells"] = json::array();
    result["nulls"] = json::array();
    json& cellsJson = result["cells"];
    json& nullsJson = result["nulls"];
    for (Cell const& cell : _index2cell) {
        char const* ptr = cell.first;
        size_t const length = cell.second;
        if (nullptr == ptr) {
            cellsJson.push_back(string());
            nullsJson.push_back(1);
        } else {
            cellsJson.push_back(util::String::toHex(ptr, length));
            nullsJson.push_back(0);
        }
    }
    return result;
}

}  // namespace lsst::qserv::replica::database::mysql
