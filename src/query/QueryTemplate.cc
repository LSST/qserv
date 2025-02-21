// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2017 AURA/LSST.
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

/**
 * @file
 *
 * @brief Implementation of QueryTemplate, which is a object that can
 * be used to generate concrete queries from a template, given
 * certain parameters (e.g. chunk/subchunk).
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "query/QueryTemplate.h"

// System headers
#include <iostream>
#include <sstream>

// Qserv headers
#include "global/sqltoken.h"  // sqlShouldSeparate
#include "query/ColumnRef.h"
#include "query/TableRef.h"

using namespace std;

namespace lsst::qserv::query {

////////////////////////////////////////////////////////////////////////
// QueryTemplate::Entry subclasses
////////////////////////////////////////////////////////////////////////

class ColumnEntry : public QueryTemplate::Entry {
public:
    ColumnEntry(ColumnRef const& cr, QueryTemplate const& queryTemplate) {
        std::ostringstream os;
        if (not queryTemplate.getUseColumnOnly()) {
            auto tableRef = cr.getTableRef();
            if (nullptr != tableRef) {
                QueryTemplate qt(queryTemplate.getAliasMode());
                qt.setQuoteIdentifiers(queryTemplate.quoteIdentifiers());
                TableRef::render render(qt);
                render.applyToQT(*tableRef);
                os << qt;
                if (os.tellp() > 0) {  // if the tableRef wrote anything...
                    os << ".";
                }
            }
        }
        os << queryTemplate.formatIdentifier(cr.getColumn());
        val = os.str();
    }
    virtual std::string getValue() const { return val; }
    virtual bool isDynamic() const { return true; }

    std::string val;
};

////////////////////////////////////////////////////////////////////////
// QueryTemplate
////////////////////////////////////////////////////////////////////////

// Return a string representation of the object
std::string QueryTemplate::sqlFragment() const {
    std::string lastEntry;
    std::string sep(" ");
    std::ostringstream os;
    for (auto const& entry : _entries) {
        std::string const& entryStr = entry->getValue();
        if (entryStr.empty()) {
            return std::string();
        }
        if (!lastEntry.empty() &&
            lsst::qserv::sql::sqlShouldSeparate(lastEntry, *lastEntry.rbegin(), entryStr.at(0))) {
            os << sep;
        }
        os << entryStr;
        lastEntry = entryStr;
    }
    return os.str();
}

std::ostream& operator<<(std::ostream& os, QueryTemplate const& queryTemplate) {
    std::string lastEntry;
    std::string sep(" ");
    for (auto const& entry : queryTemplate._entries) {
        std::string const& entryStr = entry->getValue();
        if (entryStr.empty()) {
            return os;
        }
        if (!lastEntry.empty() &&
            lsst::qserv::sql::sqlShouldSeparate(lastEntry, *lastEntry.rbegin(), entryStr.at(0))) {
            os << sep;
        }
        os << entryStr;
        lastEntry = entryStr;
    }
    return os;
}

std::string QueryTemplate::formatIdentifier(std::string const& identifier) const {
    if (not _quoteIdentifiers) return identifier;
    return "`" + identifier + "`";
}

void QueryTemplate::append(std::string const& s) {
    std::shared_ptr<Entry> e = std::make_shared<StringEntry>(s);
    _entries.push_back(e);
}

void QueryTemplate::append(ColumnRef const& cr) {
    std::shared_ptr<Entry> e = std::make_shared<ColumnEntry>(cr, *this);
    _entries.push_back(e);
}

void QueryTemplate::append(QueryTemplate::Entry::Ptr const& e) { _entries.push_back(e); }

void QueryTemplate::appendIdentifier(std::string const& s) {
    if (not _quoteIdentifiers) {
        append(s);
        return;
    }
    append(formatIdentifier(s));
}

std::string QueryTemplate::generate(EntryMapping const& em) const {
    QueryTemplate newQt;
    for (auto const& entry : _entries) {
        newQt.append(em.mapEntry(*entry));
    }
    return newQt.sqlFragment();
}

void QueryTemplate::clear() { _entries.clear(); }

void QueryTemplate::setAliasMode(SetAliasMode aliasMode) { _aliasMode = aliasMode; }

QueryTemplate::SetAliasMode QueryTemplate::getAliasMode() const { return _aliasMode; }

QueryTemplate::GetAliasMode QueryTemplate::getValueExprAliasMode() const {
    switch (_aliasMode) {
        default:
            throw std::runtime_error("Unhandled alias mode.");

        case NO_ALIAS:
            return DONT_USE;

        case USE_ALIAS:
            return USE;

        case DEFINE_TABLE_ALIAS:
            throw std::runtime_error("can't print a ValueExpr while defining its table alias.");

        case DEFINE_VALUE_ALIAS_USE_TABLE_ALIAS:
            return DEFINE;

        case NO_VALUE_ALIAS_USE_TABLE_ALIAS:
            return DONT_USE;
    }
    throw std::runtime_error("Unexpected function exit.");
    return DONT_USE;  // should never get here but to satisfy the compiler.
}

QueryTemplate::GetAliasMode QueryTemplate::getTableAliasMode() const {
    switch (_aliasMode) {
        default:
            throw std::runtime_error("Unhandled alias mode.");

        case NO_ALIAS:
            return DONT_USE;

        case USE_ALIAS:
            return USE;

        case DEFINE_TABLE_ALIAS:
            return DEFINE;

        case DEFINE_VALUE_ALIAS_USE_TABLE_ALIAS:
            return USE;

        case NO_VALUE_ALIAS_USE_TABLE_ALIAS:
            return USE;
    }
    throw std::runtime_error("Unexpected function exit.");
    return DONT_USE;  // should never get here but to satisfy the compiler.
}

string QueryTemplate::dump() const {
    ostringstream os;
    os << "QueryTemplate quoteIdents=" << _quoteIdentifiers;
    os << " useColOnly=" << _useColumnOnly;
    os << " aliasMode=" << _aliasMode;
    os << " entries={";
    for (auto const& entry : _entries) {
        os << "(dynamic=" << entry->isDynamic();
        os << ":val=" << entry->getValue() << ")";
    }
    os << "}";
    return os.str();
}

}  // namespace lsst::qserv::query
