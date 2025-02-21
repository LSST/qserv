// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2008-2015 AURA/LSST.
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
// SqlResults class implementation file.

// Class header
#include "sql/SqlResults.h"

// System headers
#include <cstddef>
#include <sstream>

// Qserv headers
#include "mysql/SchemaFactory.h"

namespace lsst::qserv::sql {

namespace detail {

SqlResults_Iterator::SqlResults_Iterator() : _results(), _value() {}

SqlResults_Iterator::SqlResults_Iterator(std::vector<MYSQL_RES*> const& results)
        : _results(results), _value() {
    _newRow(true);
}

SqlResults_Iterator& SqlResults_Iterator::operator++() {
    _newRow(false);
    return *this;
}

SqlResults_Iterator SqlResults_Iterator::operator++(int) {
    SqlResults_Iterator tmp = *this;
    operator++();
    return tmp;
}

bool SqlResults_Iterator::operator==(SqlResults_Iterator const& other) const {
    // the only iterators that we want to compare are "end" iterators,
    return _results.empty() and other._results.empty();
}

void SqlResults_Iterator::_newRow(bool newResult) {
    while (not _results.empty()) {
        MYSQL_RES* res = _results.front();

        // get number of columns
        unsigned ncols = 0;
        if (newResult) {
            ncols = mysql_num_fields(res);
            _value.resize(ncols);
        } else {
            ncols = _value.size();
        }

        // get next row and store
        if (MYSQL_ROW row = mysql_fetch_row(res)) {
            unsigned long* lengths = mysql_fetch_lengths(res);
            for (unsigned i = 0; i != ncols; ++i) {
                _value[i].first = row[i];
                _value[i].second = lengths[i];
            }
            return;
        } else {
            // nothing left in this result, switch to next
            _results.erase(_results.begin());
            // may need to update column count
            newResult = true;
        }
    }

    // no results left, reset
    _value.clear();
    return;
}

}  // namespace detail

void SqlResults::freeResults() {
    int i, s = _results.size();
    for (i = 0; i < s; ++i) {
        mysql_free_result(_results[i]);
    }
    _results.clear();
}

void SqlResults::addResult(MYSQL_RES* r) {
    if (_discardImmediately) {
        mysql_free_result(r);
    } else {
        _results.push_back(r);
    }
}

bool SqlResults::extractFirstColumn(std::vector<std::string>& ret, SqlErrorObject& errObj) {
    int i, s = _results.size();
    for (i = 0; i < s; ++i) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(_results[i])) != nullptr) {
            ret.push_back(row[0]);
        }
        mysql_free_result(_results[i]);
    }
    _results.clear();
    return true;
}

bool SqlResults::extractFirst2Columns(std::vector<std::string>& col1, std::vector<std::string>& col2,
                                      SqlErrorObject& errObj) {
    int i, s = _results.size();
    for (i = 0; i < s; ++i) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(_results[i])) != nullptr) {
            col1.push_back(row[0]);
            col2.push_back(row[1]);
        }
        mysql_free_result(_results[i]);
    }
    _results.clear();
    return true;
}

bool SqlResults::extractFirst3Columns(std::vector<std::string>& col1, std::vector<std::string>& col2,
                                      std::vector<std::string>& col3, SqlErrorObject& errObj) {
    int i, s = _results.size();
    for (i = 0; i < s; ++i) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(_results[i])) != nullptr) {
            col1.push_back(row[0]);
            col2.push_back(row[1]);
            col3.push_back(row[2]);
        }
        mysql_free_result(_results[i]);
    }
    _results.clear();
    return true;
}

bool SqlResults::extractFirst4Columns(std::vector<std::string>& col1, std::vector<std::string>& col2,
                                      std::vector<std::string>& col3, std::vector<std::string>& col4,
                                      SqlErrorObject& errObj) {
    int i, s = _results.size();
    for (i = 0; i < s; ++i) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(_results[i])) != nullptr) {
            col1.push_back(row[0]);
            col2.push_back(row[1]);
            col3.push_back(row[2]);
            col4.push_back(row[3]);
        }
        mysql_free_result(_results[i]);
    }
    _results.clear();
    return true;
}

std::vector<std::vector<std::string>> SqlResults::extractFirstNColumns(size_t numColumns) {
    std::vector<std::vector<std::string>> rows;
    for (int resultIdx = 0, numResults = _results.size(); resultIdx < numResults; ++resultIdx) {
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(_results[resultIdx])) != nullptr) {
            std::vector<std::string> columns;
            columns.reserve(numColumns);
            for (size_t colIdx = 0; colIdx < numColumns; ++colIdx) {
                columns.push_back(row[colIdx]);
            }
            rows.push_back(std::move(columns));
        }
        mysql_free_result(_results[resultIdx]);
    }
    _results.clear();
    return rows;
}

bool SqlResults::extractFirstValue(std::string& ret, SqlErrorObject& errObj) {
    if (_results.size() != 1) {
        std::stringstream ss;
        ss << "Expecting one row, found " << _results.size() << " results" << std::endl;
        return errObj.addErrMsg(ss.str());
    }
    MYSQL_ROW row = mysql_fetch_row(_results[0]);
    if (!row) {
        return errObj.addErrMsg("Expecting one row, found no rows");
    }
    auto ptr = row[0];
    if (!ptr) {
        freeResults();
        return errObj.addErrMsg("NULL returned by the query");
    }
    ret = ptr;
    freeResults();
    return true;
}

sql::Schema SqlResults::makeSchema(SqlErrorObject& errObj) {
    sql::Schema schema;
    if (_results.size() != 1) {
        errObj.addErrMsg("Expecting single result, found " + std::to_string(_results.size()) + " results");
        return schema;
    }

    schema = mysql::SchemaFactory::newFromResult(_results[0]);
    return schema;
}

}  // namespace lsst::qserv::sql
