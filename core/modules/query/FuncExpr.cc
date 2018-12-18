// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2015 AURA/LSST.
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
  * @brief Implementation of FuncExpr (a parsed function call
  * expression) and FuncExpr::render
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "query/FuncExpr.h"

// System headers
#include <iostream>

// Third-party headers


#include "lsst/log/Log.h"

// Qserv headers
#include "query/ColumnRef.h"
#include "query/QueryTemplate.h"
#include "query/ValueExpr.h"
#include "query/ValueFactor.h"
#include "util/IterableFormatter.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.FuncExpr");

}

namespace lsst {
namespace qserv {
namespace query {


const std::string &
FuncExpr::getName() const {
    return _name;
}

FuncExpr::Ptr
FuncExpr::newLike(FuncExpr const& src, std::string const& newName) {
    FuncExpr::Ptr e = std::make_shared<FuncExpr>();
    e->setName(newName);
    e->params = src.params; // Shallow list copy.
    return e;
}

FuncExpr::Ptr
FuncExpr::newArg1(std::string const& newName, std::string const& arg1) {
    std::shared_ptr<ColumnRef> cr = std::make_shared<ColumnRef>("","",arg1);
    return newArg1(newName,
                   ValueExpr::newSimple(ValueFactor::newColumnRefFactor(cr)));
}

FuncExpr::Ptr
FuncExpr::newArg1(std::string const& newName, ValueExprPtr ve) {
    FuncExpr::Ptr e = std::make_shared<FuncExpr>();
    e->setName(newName);
    e->params.push_back(ve);
    return e;
}

FuncExpr::Ptr
FuncExpr::newWithArgs(std::string const& newName, const ValueExprPtrVector& ve) {
    FuncExpr::Ptr e = std::make_shared<FuncExpr>();
    e->setName(newName);
    e->params = ve;
    return e;
}

void
FuncExpr::setName(const std::string& val) {
    _name = val;
}

void
FuncExpr::findColumnRefs(ColumnRef::Vector& outputRefs) const {
    for (auto&& valueExpr : params) {
        valueExpr->findColumnRefs(outputRefs);
    }
}

std::shared_ptr<FuncExpr>
FuncExpr::clone() const {
    FuncExpr::Ptr e = std::make_shared<FuncExpr>();
    e->setName(getName());
    cloneValueExprPtrVector(e->params, params);
    return e;
}

std::ostream&
operator<<(std::ostream& os, FuncExpr const& fe) {
    os << "FuncExpr(";
    os << "name:" << fe._name;
    os << ", " << util::printable(fe.params);
    os << ")";
    return os;
}

std::ostream&
operator<<(std::ostream& os, FuncExpr const* fe) {
    (nullptr == fe) ? os << "nullptr" : os << *fe;
    return os;
}

void
FuncExpr::renderTo(QueryTemplate& qt) const {
    qt.append(getName());
    qt.append("(");
    renderList(qt, params);
    qt.append(")");
}

bool FuncExpr::operator==(const FuncExpr& rhs) const {
    return _name == rhs._name &&
           util::vectorPtrCompare<ValueExpr>(params, rhs.params);
}

}}} // namespace lsst::qserv::query
