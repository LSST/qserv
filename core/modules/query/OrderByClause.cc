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
  * @brief Implementation of OrderByTerm and OrderByClause
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "query/OrderByClause.h"

// System headers
#include <iostream>
#include <iterator>
#include <sstream>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "query/QueryTemplate.h"
#include "query/ValueExpr.h"
#include "util/PointerCompare.h"
#include "util/IterableFormatter.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.query.OrderByClause");

using lsst::qserv::query::OrderByTerm;

char const* getOrderStr(OrderByTerm::Order o) {
    switch(o) {
    case OrderByTerm::ASC: return "ASC";
    case OrderByTerm::DESC: return "DESC";
    case OrderByTerm::DEFAULT: return "";
    default: return "UNKNOWN_ORDER";
    }
}

} // anonymous namespace

namespace lsst {
namespace qserv {
namespace query {

class OrderByTerm::render : public std::unary_function<OrderByTerm, void> {
public:
    render(QueryTemplate& qt) : _qt(qt), _count(0) {}
    void applyToQT(OrderByTerm const& term) {
        if (_count++ > 0) {
            _qt.append(", ");
        }
        term.renderTo(_qt);
        LOGS(_log, LOG_LVL_TRACE, "Query Template: " << _qt);
    }
    QueryTemplate& _qt;
    int _count;
};

////////////////////////////////////////////////////////////////////////
// OrderByTerm
////////////////////////////////////////////////////////////////////////
void
OrderByTerm::renderTo(QueryTemplate& qt) const {
    ValueExpr::render r(qt, true);
    r.applyToQT(_expr);
    if (!_collate.empty()) {
        qt.append("COLLATE");
        qt.append(_collate);
    }
    char const* orderStr = getOrderStr(_order);
    if (orderStr && orderStr[0] != '\0') {
        qt.append(orderStr);
    }
}

std::string OrderByTerm::sqlFragment() const {
    std::string str;
    str += _expr->sqlFragment();
    if (!_collate.empty()) {
        str += " COLLATE " + _collate;
    }
    char const* orderStr = getOrderStr(_order);
    if (orderStr && orderStr[0] != '\0') {
        str += std::string(" ") + orderStr;
    }
    return str;
}

std::ostream&
operator<<(std::ostream& os, OrderByTerm const& t) {
    os << "OrderByTerm(";
    os << "expr:" << t._expr;
    os << ", order:";
    switch (t._order) {
    case OrderByTerm::DEFAULT: os << "DEFAULT"; break;
    case OrderByTerm::ASC: os << "ASC"; break;
    case OrderByTerm::DESC: os << "DESC"; break;
    default: os << "!!unhandled!!"; break;
    }
    os << ", collate:" <<  t._collate;
    os << ")";
    return os;
}

bool OrderByTerm::operator==(const OrderByTerm& rhs) const {
    return util::ptrCompare<ValueExpr>(_expr, rhs._expr) &&
            _order == rhs._order &&
            _collate == rhs._collate;
}


////////////////////////////////////////////////////////////////////////
// OrderByClause
////////////////////////////////////////////////////////////////////////
std::ostream&
operator<<(std::ostream& os, OrderByClause const& clause) {
    os << "OrderByClause(" << util::ptrPrintable(clause._terms) << ")";
    return os;
}

std::ostream&
operator<<(std::ostream& os, OrderByClause const* clause) {
    (nullptr == clause) ? os << "nullptr" : os << *clause;
    return os;
}

std::string OrderByClause::sqlFragment() const {
    std::string str;
    if (_terms != nullptr) {
        if (false == _terms->empty()) {
            str += "ORDER BY ";
            for (auto termsItr = _terms->begin(); termsItr != _terms->end(); ++termsItr) {
                if (termsItr != _terms->begin()) {
                    str += ", ";
                }
                str += termsItr->sqlFragment();
            }
        }
    }
    return str;
}

void
OrderByClause::renderTo(QueryTemplate& qt) const {
    if (_terms.get() && _terms->size() > 0) {
        OrderByTerm::render r(qt);
        for(auto& term : *_terms) {
            LOGS(_log, LOG_LVL_TRACE, "Rendering term: " << term);
            r.applyToQT(term);
        }
    }
}

std::shared_ptr<OrderByClause> OrderByClause::clone() const {
    return std::make_shared<OrderByClause>(*this); // FIXME
}
std::shared_ptr<OrderByClause> OrderByClause::copySyntax() {
    return std::make_shared<OrderByClause>(*this);
}

void OrderByClause::findValueExprs(ValueExprPtrVector& list) const {
    for (auto&& orderByTerm : *_terms) {
        list.push_back(orderByTerm.getExpr());
    }
}

bool OrderByClause::operator==(const OrderByClause& rhs) const {
    return util::ptrVectorCompare<OrderByTerm>(_terms, rhs._terms);
}


}}} // namespace lsst::qserv::query
