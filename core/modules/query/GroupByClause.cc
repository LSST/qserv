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
  * @brief Implementation of GroupByClause and GroupByTerm
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "query/GroupByClause.h"

// System headers
#include <algorithm>
#include <iostream>
#include <iterator>

// Third-party headers

// Qserv headers
#include "query/QueryTemplate.h"
#include "query/ValueExpr.h"
#include "util/IterableFormatter.h"

namespace lsst {
namespace qserv {
namespace query {


////////////////////////////////////////////////////////////////////////
// GroupByTerm
////////////////////////////////////////////////////////////////////////
GroupByTerm GroupByTerm::cloneValue() const {
    GroupByTerm t;
    if (_expr) { t._expr = _expr->clone(); }
    t._collate = _collate;
    return t;
}

GroupByTerm& GroupByTerm::operator=(GroupByTerm const& gb) {
    if (this != &gb) {
        if (gb._expr) { _expr = gb._expr->clone(); }
        _collate = gb._collate;
    }
    return *this;
}

std::ostream& operator<<(std::ostream& os, GroupByTerm const& t) {
    os << "GroupByTerm(expr:" << t._expr;
    os << ", collate:" << t._collate;
    os << ")";
    return os;
}

bool GroupByTerm::operator==(const GroupByTerm& rhs) const {
    return util::ptrCompare<ValueExpr>(_expr, rhs._expr) &&
            _collate == rhs._collate;
}

////////////////////////////////////////////////////////////////////////
// GroupByClause
////////////////////////////////////////////////////////////////////////
std::ostream& operator<<(std::ostream& os, GroupByClause const& c) {
    os << "GroupByClause(" << util::ptrPrintable(c._terms) << ")";
    return os;
}

std::ostream& operator<<(std::ostream& os, GroupByClause const* c) {
    (nullptr == c) ? os << "nullptr" : os << *c;
    return os;
}

std::string GroupByClause::getGenerated() {
    QueryTemplate qt;
    renderTo(qt);
    return qt.sqlFragment();
}


void GroupByClause::renderTo(QueryTemplate& qt) const {
    if (_terms.get() && _terms->size() > 0) {
        ValueExpr::render vr(qt, true);
        int count = 0;
        for (auto& term : *_terms) {
            if (count++ > 0) {
                qt.append(",");
            }
            vr.applyToQT(term.getExpr());
        }
    }
}

namespace {
GroupByTerm callClone(GroupByTerm const& t) {
    return t.cloneValue();
}
}

std::shared_ptr<GroupByClause> GroupByClause::clone() const {
    GroupByClause::Ptr p = std::make_shared<GroupByClause>();
    std::transform(_terms->begin(), _terms->end(),
                   std::back_inserter(*p->_terms), callClone);
    return p;
}

std::shared_ptr<GroupByClause> GroupByClause::copySyntax() {
    return std::make_shared<GroupByClause>(*this);
}

void GroupByClause::findValueExprs(ValueExprPtrVector& list) const {
    for (auto&& groupByTerm : *_terms) {
        list.push_back(groupByTerm.getExpr());
    }
}

bool GroupByClause::operator==(const GroupByClause& rhs) const {
    return util::ptrDequeCompare<GroupByTerm>(_terms, rhs._terms);
}


}}} // namespace lsst::qserv::query
