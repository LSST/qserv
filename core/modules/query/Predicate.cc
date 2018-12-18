// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2017 AURA/LSST.
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
  * @brief Predicate, CompPredicate, InPredicate, BetweenPredicate, LikePredicate, and NullPredicate implementations.
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "query/Predicate.h"

// System
#include <algorithm>
#include <stdexcept>

// Qserv headers
#include "query/QueryTemplate.h"
#include "query/SqlSQL2Tokens.h" // (generated) SqlSQL2Tokens
#include "query/ValueExpr.h"
#include "util/IterableFormatter.h"
#include "util/PointerCompare.h"


namespace lsst {
namespace qserv {
namespace query {

std::ostream& operator<<(std::ostream& os, Predicate const& bt) {
    bt.dbgPrint(os);
    return os;
}


void CompPredicate::findColumnRefs(ColumnRef::Vector& vector) const {
    if (left) { left->findColumnRefs(vector); }
    if (right) { right->findColumnRefs(vector); }
}

void InPredicate::findColumnRefs(ColumnRef::Vector& vector) const {
    if (value) { value->findColumnRefs(vector); }
    for(auto&& valueExpr : cands) {
        valueExpr->findColumnRefs(vector);
    }
}

void BetweenPredicate::findColumnRefs(ColumnRef::Vector& vector) const {
    if (value) { value->findColumnRefs(vector); }
    if (minValue) { minValue->findColumnRefs(vector); }
    if (maxValue) { maxValue->findColumnRefs(vector); }
}

void LikePredicate::findColumnRefs(ColumnRef::Vector& vector) const {
    if (value) { value->findColumnRefs(vector); }
    if (charValue) { charValue->findColumnRefs(vector); }
}

void NullPredicate::findColumnRefs(ColumnRef::Vector& vector) const {
    if (value) { value->findColumnRefs(vector); }
}

std::ostream& CompPredicate::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}
std::ostream& InPredicate::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}
std::ostream& BetweenPredicate::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}
std::ostream& LikePredicate::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}
std::ostream& NullPredicate::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}

void CompPredicate::renderTo(QueryTemplate& qt) const {

    ValueExpr::render r(qt, false);
    r.applyToQT(left);
    switch(op) {
    case SqlSQL2Tokens::EQUALS_OP: qt.append("="); break;
    case SqlSQL2Tokens::NULL_SAFE_EQUALS_OP: qt.append("<=>"); break;
    case SqlSQL2Tokens::NOT_EQUALS_OP: qt.append("<>"); break;
    case SqlSQL2Tokens::LESS_THAN_OP: qt.append("<"); break;
    case SqlSQL2Tokens::GREATER_THAN_OP: qt.append(">"); break;
    case SqlSQL2Tokens::LESS_THAN_OR_EQUALS_OP: qt.append("<="); break;
    case SqlSQL2Tokens::GREATER_THAN_OR_EQUALS_OP: qt.append(">="); break;
    case SqlSQL2Tokens::NOT_EQUALS_OP_ALT: qt.append("!="); break;
    }
    r.applyToQT(right);
}

void InPredicate::renderTo(QueryTemplate& qt) const {
    ValueExpr::render r(qt, false);
    r.applyToQT(value);
    if (hasNot) qt.append("NOT");
    qt.append("IN");
    ValueExpr::render rComma(qt, true);
    qt.append("(");
    for (auto& cand : cands) {
        rComma.applyToQT(cand);
    }
    qt.append(")");
}

void BetweenPredicate::renderTo(QueryTemplate& qt) const {
    ValueExpr::render r(qt, false);
    r.applyToQT(value);
    if (hasNot) qt.append("NOT");
    qt.append("BETWEEN");
    r.applyToQT(minValue);
    qt.append("AND");
    r.applyToQT(maxValue);
}

void LikePredicate::renderTo(QueryTemplate& qt) const {
    ValueExpr::render r(qt, false);
    r.applyToQT(value);
    if (hasNot) { qt.append("NOT"); }
    qt.append("LIKE");
    r.applyToQT(charValue);
}

void NullPredicate::renderTo(QueryTemplate& qt) const {
    ValueExpr::render r(qt, false);
    r.applyToQT(value);
    qt.append("IS");
    if (hasNot) { qt.append("NOT"); }
    qt.append("NULL");
}

void CompPredicate::findValueExprs(ValueExprPtrVector& vector) const {
    vector.push_back(left);
    vector.push_back(right);
}

void InPredicate::findValueExprs(ValueExprPtrVector& vector) const {
    vector.push_back(value);
    vector.insert(vector.end(), cands.begin(), cands.end());
}

void BetweenPredicate::findValueExprs(ValueExprPtrVector& vector) const {
    vector.push_back(value);
    vector.push_back(minValue);
    vector.push_back(maxValue);
}

void LikePredicate::findValueExprs(ValueExprPtrVector& vector) const {
    vector.push_back(value);
    vector.push_back(charValue);
}

void NullPredicate::findValueExprs(ValueExprPtrVector& vector) const {
    vector.push_back(value);
}

int CompPredicate::lookupOp(char const* op) {
    switch(op[0]) {
    case '<':
        if (op[1] == '\0') { return SqlSQL2Tokens::LESS_THAN_OP; }
        else if (op[1] == '>') { return SqlSQL2Tokens::NOT_EQUALS_OP; }
        else if (op[1] == '=') { return SqlSQL2Tokens::LESS_THAN_OR_EQUALS_OP; }
        else { throw std::invalid_argument("Invalid op string <?"); }
    case '>':
        if (op[1] == '\0') { return SqlSQL2Tokens::GREATER_THAN_OP; }
        else if (op[1] == '=') { return SqlSQL2Tokens::GREATER_THAN_OR_EQUALS_OP; }
        else { throw std::invalid_argument("Invalid op string >?"); }
    case '=':
        return SqlSQL2Tokens::EQUALS_OP;
    default:
        throw std::invalid_argument("Invalid op string ?");
    }
}

void CompPredicate::dbgPrint(std::ostream& os) const {
    os << "CompPredicate(left:" << left;
    os << ", op:" << op;
    os << ", right:" << right;
    os << ")";
}

bool CompPredicate::operator==(const BoolFactorTerm& rhs) const {
    auto rhsCompPredicate = dynamic_cast<CompPredicate const *>(&rhs);
    if (nullptr == rhsCompPredicate) {
        return false;
    }
    return util::ptrCompare<ValueExpr>(left, rhsCompPredicate->left) &&
           op == rhsCompPredicate->op &&
           util::ptrCompare<ValueExpr>(right, rhsCompPredicate->right);
}

BoolFactorTerm::Ptr CompPredicate::clone() const {
    CompPredicate* p = new CompPredicate;
    if (left) p->left = left->clone();
    p->op = op;
    if (right) p->right = right->clone();
    return BoolFactorTerm::Ptr(p);
}

BoolFactorTerm::Ptr GenericPredicate::clone() const {
    //return BfTerm::Ptr(new GenericPredicate());
    return BoolFactorTerm::Ptr();
}

namespace {
    struct valueExprCopy {
        inline ValueExprPtr operator()(ValueExprPtr const& p) {
            return p ? p->clone() : ValueExprPtr();
        }
    };
}

BoolFactorTerm::Ptr InPredicate::clone() const {
    InPredicate::Ptr p  = std::make_shared<InPredicate>();
    if (value) p->value = value->clone();
    std::transform(cands.begin(), cands.end(),
                   std::back_inserter(p->cands),
                   valueExprCopy());
    p->hasNot = hasNot;
    return BoolFactorTerm::Ptr(p);
}

void InPredicate::dbgPrint(std::ostream& os) const {
    os << "InPredicate(value:" << value;
    os << ", cands:" << util::printable(cands);
    if (hasNot) {
        os << ", has NOT";
    }
    os << ")";
}

bool InPredicate::operator==(const BoolFactorTerm& rhs) const {
    auto rhsInPredicate = dynamic_cast<InPredicate const *>(&rhs);
    if (nullptr == rhsInPredicate) {
        return false;
    }
    return util::ptrCompare<ValueExpr>(value, rhsInPredicate->value) &&
           util::vectorPtrCompare<ValueExpr>(cands, rhsInPredicate->cands) &&
           hasNot == rhsInPredicate->hasNot;
}

BoolFactorTerm::Ptr BetweenPredicate::clone() const {
    BetweenPredicate::Ptr p = std::make_shared<BetweenPredicate>();
    if (value) p->value = value->clone();
    p->hasNot = hasNot;
    if (minValue) p->minValue = minValue->clone();
    if (maxValue) p->maxValue = maxValue->clone();
    return BoolFactorTerm::Ptr(p);
}

void BetweenPredicate::dbgPrint(std::ostream& os) const {
    os << "BetweenPredicate(value:" << value;
    if (hasNot) {
        os << " NOT,";
    }
    os << ", minValue:" << minValue;
    os << ", maxValue:" << maxValue;
    os << ")";
}

bool BetweenPredicate::operator==(const BoolFactorTerm& rhs) const {
    auto rhsBetweenPredicate = dynamic_cast<BetweenPredicate const *>(&rhs);
    if (nullptr == rhsBetweenPredicate) {
        return false;
    }
    return util::ptrCompare<ValueExpr>(value, rhsBetweenPredicate->value) &&
           hasNot == rhsBetweenPredicate->hasNot &&
           util::ptrCompare<ValueExpr>(minValue, rhsBetweenPredicate->minValue) &&
           util::ptrCompare<ValueExpr>(maxValue, rhsBetweenPredicate->maxValue);
}

BoolFactorTerm::Ptr LikePredicate::clone() const {
    LikePredicate::Ptr p = std::make_shared<LikePredicate>();
    if (value) p->value = value->clone();
    if (charValue) p->charValue = charValue->clone();
    p->hasNot = hasNot;
    return BoolFactorTerm::Ptr(p);
}

void LikePredicate::dbgPrint(std::ostream& os) const {
    os << "LikePredicate(value:" << value;
    if (hasNot) {
        os << ", NOT";
    }
    os << ", charValue:" << charValue;
    os << ")";
}

bool LikePredicate::operator==(const BoolFactorTerm& rhs) const {
    auto rhsLikePredicate = dynamic_cast<LikePredicate const *>(&rhs);
    if (nullptr == rhsLikePredicate) {
        return false;
    }
    return util::ptrCompare<ValueExpr>(value, rhsLikePredicate->value) &&
           util::ptrCompare<ValueExpr>(charValue, rhsLikePredicate->charValue) &&
           hasNot == rhsLikePredicate->hasNot;
}

BoolFactorTerm::Ptr NullPredicate::clone() const {
    NullPredicate::Ptr p = std::make_shared<NullPredicate>();
    if (value) p->value = value->clone();
    p->hasNot = hasNot;
    return BoolFactorTerm::Ptr(p);
}

void NullPredicate::dbgPrint(std::ostream& os) const {
    os << "NullPredicate(value:" << value;
    if (hasNot) {
        os << ", NOT";
    }
    os << ")";
}

bool NullPredicate::operator==(const BoolFactorTerm& rhs) const {
    auto rhsNullPredicate = dynamic_cast<NullPredicate const *>(&rhs);
    if (nullptr == rhsNullPredicate) {
        return false;
    }
    return hasNot == rhsNullPredicate->hasNot &&
           util::ptrCompare<ValueExpr>(value, rhsNullPredicate->value);
}


}}} // namespace lsst::qserv::query
