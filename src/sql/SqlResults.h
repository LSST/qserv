// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012 LSST Corporation.
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
// SqlResults is a class that buffers up results from a particular query. It is
// currently mysql-specific, but this may (likely) change in the future.
// Unfortunately, including SqlResults.h will pull in mysql.h since MYSQL_RES is
// used in the class declaration and MYSQL_RES is a typedef rather than a class
// or simple struct.

#ifndef LSST_QSERV_SQL_SQLRESULTS_H
#define LSST_QSERV_SQL_SQLRESULTS_H

// System headers
#include <iterator>
#include <string>
#include <vector>

// Third-party headers
#include "boost/utility.hpp"
#include <mysql/mysql.h>

// Local headers
#include "sql/SqlErrorObject.h"
#include "sql/Schema.h"

namespace lsst::qserv::sql {

namespace detail {

/**
 *  Iterator for going over the rows in the result set. Value type for iterator
 *  is the sequence of strings (pointers) and their lengths. Pointer may be NULL
 *  if the column value is NONE.
 */
class SqlResults_Iterator
        : public std::iterator<std::input_iterator_tag, std::vector<std::pair<char const*, unsigned long>>> {
public:
    SqlResults_Iterator();
    SqlResults_Iterator(std::vector<MYSQL_RES*> const& results);

    pointer operator->() { return &_value; }
    reference operator*() { return _value; }

    SqlResults_Iterator& operator++();
    SqlResults_Iterator operator++(int);

    bool operator==(SqlResults_Iterator const& other) const;
    bool operator!=(SqlResults_Iterator const& other) const { return not operator==(other); }

private:
    void _newRow(bool newResult);

    std::vector<MYSQL_RES*> _results;
    value_type _value;
};
}  // namespace detail

class SqlResults : boost::noncopyable {
public:
    typedef detail::SqlResults_Iterator iterator;
    typedef detail::SqlResults_Iterator const_iterator;
    typedef iterator::value_type value_type;

    SqlResults(bool discardImmediately = false) : _discardImmediately(discardImmediately), _affectedRows(0) {}
    ~SqlResults() { freeResults(); }

    void addResult(MYSQL_RES* r);
    void setAffectedRows(unsigned long long count) { _affectedRows = count; }
    // Get number of affected rows for UPDATE/DELETE/INSERT,
    // do not use it for SELECT
    unsigned long long getAffectedRows() const { return _affectedRows; }
    bool extractFirstValue(std::string&, SqlErrorObject&);
    bool extractFirstColumn(std::vector<std::string>&, SqlErrorObject&);
    bool extractFirst2Columns(std::vector<std::string>&,  // FIXME: generalize
                              std::vector<std::string>&, SqlErrorObject&);
    bool extractFirst3Columns(std::vector<std::string>&,  // FIXME: generalize
                              std::vector<std::string>&, std::vector<std::string>&, SqlErrorObject&);
    bool extractFirst4Columns(std::vector<std::string>&, std::vector<std::string>&, std::vector<std::string>&,
                              std::vector<std::string>&, SqlErrorObject&);

    /**
     * Extract a result set into the 2D array.
     * @param numColumns The number of columns in the array.
     * @return a 2D array, where the first index of the array represents rows
     *   and the second index represents columns.
     */
    std::vector<std::vector<std::string>> extractFirstNColumns(size_t numColumns);

    void freeResults();

    /// Return row iterator
    iterator begin() { return iterator(_results); }
    iterator end() { return iterator(); }

    /// Return result schema, this makes sense
    /// only if there is a single result.
    sql::Schema makeSchema(SqlErrorObject& errObj);

private:
    std::vector<MYSQL_RES*> _results;
    bool _discardImmediately;
    unsigned long long _affectedRows;
};

}  // namespace lsst::qserv::sql

#endif  // LSST_QSERV_SQL_SQLRESULTS_H
