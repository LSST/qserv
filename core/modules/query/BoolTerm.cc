// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2016 AURA/LSST.
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
  * @brief BoolTerm implementations.
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "query/BoolTerm.h"

// System headers
#include <algorithm>
#include <iterator>
#include <stdexcept>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "query/Predicate.h"
#include "query/QueryTemplate.h"
#include "query/ValueExpr.h"
#include "util/IterableFormatter.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.query.BoolTerm");
}

namespace lsst {
namespace qserv {
namespace query {

////////////////////////////////////////////////////////////////////////
// BoolTerm section
////////////////////////////////////////////////////////////////////////
std::ostream& operator<<(std::ostream& os, BoolTerm const& bt) {
    bt.dbgPrint(os);
    return os;
}
std::ostream& operator<<(std::ostream& os, BoolTerm const* bt) {
    (nullptr == bt) ? os << "nullptr" : os << *bt;
    return os;
}
std::ostream& LogicalTerm::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}
std::ostream& BoolFactor::putStream(std::ostream& os) const {
    return QueryTemplate::renderDbg(os, *this);
}
std::ostream& UnknownTerm::putStream(std::ostream& os) const {
    return os << "--UNKNOWNTERM--";
}
std::ostream& PassTerm::putStream(std::ostream& os) const {
    return os << _text;
}
std::ostream& PassListTerm::putStream(std::ostream& os) const {
    std::copy(_terms.begin(), _terms.end(),
              std::ostream_iterator<std::string>(os, " "));
    return os;
}
std::ostream& BoolTermFactor::putStream(std::ostream& os) const {
    if (_term) { return _term->putStream(os); }
    return os;
}

namespace {
    template <typename Plist>
    inline void renderList(QueryTemplate& qt,
                           Plist const& lst,
                           BoolTerm::OpPrecedence listOpPrecedence,
                           std::string const& sep) {
        int count=0;
        typename Plist::const_iterator i;
        for(i = lst.begin(); i != lst.end(); ++i) {
            if (!sep.empty() && ++count > 1) { qt.append(sep); }
            if (!*i) { throw std::logic_error("Bad list term"); }
            BoolTerm *asBoolTerm = dynamic_cast<BoolTerm*>(&**i);
            BoolTerm::OpPrecedence termOpPrecedence = asBoolTerm
                ? asBoolTerm->getOpPrecedence()
                : BoolTerm::OTHER_PRECEDENCE;
            bool parensNeeded = listOpPrecedence > termOpPrecedence;
            if (parensNeeded) { qt.append("("); }
            (**i).renderTo(qt);
            if (parensNeeded) { qt.append(")"); }
        }
    }
}

void OrTerm::renderTo(QueryTemplate& qt) const {
    renderList(qt, _terms, getOpPrecedence(), "OR");
}
void AndTerm::renderTo(QueryTemplate& qt) const {
    renderList(qt, _terms, getOpPrecedence(), "AND");
}
void BoolFactor::renderTo(QueryTemplate& qt) const {
    std::string s;
    if (_hasNot) {
        qt.append("NOT");
    }
    renderList(qt, _terms, getOpPrecedence(), s);
}
void UnknownTerm::renderTo(QueryTemplate& qt) const {
    qt.append("unknown");
}
void PassTerm::renderTo(QueryTemplate& qt) const {
    qt.append(_text);
}
void PassListTerm::renderTo(QueryTemplate& qt) const {
    qt.append("(");
    StringVector::const_iterator i;
    bool isFirst=true;
    for(i=_terms.begin(); i != _terms.end(); ++i) {
        if (!isFirst) {
            qt.append(",");
        }
        qt.append(*i);
        isFirst = false;
    }
    qt.append(")");
}
void BoolTermFactor::renderTo(QueryTemplate& qt) const {
    if (_term) { _term->renderTo(qt); }
}

std::shared_ptr<BoolTerm> LogicalTerm::getReduced() {
    // Can I eliminate myself?
    if (_terms.size() == 1) {
        std::shared_ptr<BoolTerm> reduced = _terms.front()->getReduced();
        if (reduced) { return reduced; }
        else { return _terms.front(); }
    } else { // Get reduced versions of my children.
        // FIXME: Apply reduction on each term.
        // If reduction was successful on any child, construct a new LogicalTerm of the same subclass type
        // (AndTerm, OrTerm, etc).
    }
    return std::shared_ptr<BoolTerm>();
}

bool BoolFactor::_reduceTerms(BoolFactorTerm::PtrVector& newTerms,
                              BoolFactorTerm::PtrVector& oldTerms) {
    typedef BoolFactorTerm::PtrVector::iterator Iter;
    bool hasReduction = false;
    for(Iter i=oldTerms.begin(), e=oldTerms.end(); i != e; ++i) {
        BoolFactorTerm& term = **i;
        BoolTermFactor* btf = dynamic_cast<BoolTermFactor*>(&term);

        if (btf) {
            if (btf->_term) {
                std::shared_ptr<BoolTerm> reduced = btf->_term->getReduced();
                if (reduced) {
                    BoolFactor* f =  dynamic_cast<BoolFactor*>(reduced.get());
                    if (f) { // factor in a term in a factor --> factor
                        newTerms.insert(newTerms.end(),
                                        f->_terms.begin(), f->_terms.end());
                        hasReduction = true;
                    } else {
                        // still a reduction in the term, replace
                        std::shared_ptr<BoolTermFactor> newBtf;
                        newBtf = std::make_shared<BoolTermFactor>();
                        newBtf->_term = reduced;
                        newTerms.push_back(newBtf);
                        hasReduction = true;
                    }
                } else { // The bfterm's term couldn't be reduced,
                    // so just add it.
                    newTerms.push_back(*i);
                }
            } else { // Term-less bool term factor. Ignore.
                hasReduction = true;
            }
        } else {
            // add old bfterm
            newTerms.push_back(*i);
        }
    }
    return hasReduction;
}

bool BoolFactor::_checkParen(BoolFactorTerm::PtrVector& terms) {
    if (terms.size() != 3) { return false; }

    PassTerm* pt = dynamic_cast<PassTerm*>(terms.front().get());
    if (!pt || (pt->_text != "(")) { return false; }

    pt = dynamic_cast<PassTerm*>(terms.back().get());
    if (!pt || (pt->_text != ")")) { return false; }

    auto boolTermFactorPtr = std::dynamic_pointer_cast<BoolTermFactor>(terms[1]);
    if (nullptr == boolTermFactorPtr) {
        return true;
    }
    auto logicalTermPtr = std::dynamic_pointer_cast<LogicalTerm>(boolTermFactorPtr->_term);
    if (nullptr != logicalTermPtr) {
        return false; // don't remove parens from an AND or an OR.
    }

    return true;
}

std::shared_ptr<BoolTerm> BoolFactor::getReduced() {
    // Get reduced versions of my children.
    BoolFactorTerm::PtrVector newTerms;
    bool hasReduction = false;
    hasReduction = _reduceTerms(newTerms, _terms);
    // Parentheses reduction
    if (_checkParen(newTerms)) {
        newTerms.erase(newTerms.begin());
        newTerms.pop_back();
        hasReduction = true;
    }
    if (hasReduction) {
        return std::make_shared<BoolFactor>(newTerms, _hasNot);
    } else {
        return std::shared_ptr<BoolTerm>();
    }
}

namespace {
    struct syntaxCopy {
        inline BoolTerm::Ptr operator()(BoolTerm::Ptr const& t) {
            return t ? t->copySyntax() : BoolTerm::Ptr();
        }
        inline BoolFactorTerm::Ptr operator()(BoolFactorTerm::Ptr const& t) {
            return t ? t->copySyntax() : BoolFactorTerm::Ptr();
        }
    };

    struct deepCopy {
        inline BoolTerm::Ptr operator()(BoolTerm::Ptr const& t) {
            return t ? t->clone() : BoolTerm::Ptr();
        }
        inline BoolFactorTerm::Ptr operator()(BoolFactorTerm::Ptr const& t) {
            return t ? t->clone() : BoolFactorTerm::Ptr();
        }
    };

    template <typename List, class Copy>
    inline void copyTerms(List& dest, List const& src) {
        std::transform(src.begin(), src.end(), std::back_inserter(dest), Copy());
    }
} // anonymous namespace

std::shared_ptr<BoolTerm> OrTerm::clone() const {
    std::shared_ptr<OrTerm> ot = std::make_shared<OrTerm>();
    copyTerms<BoolTerm::PtrVector, deepCopy>(ot->_terms, _terms);
    return ot;
}
std::shared_ptr<BoolTerm> AndTerm::clone() const {
    std::shared_ptr<AndTerm> t = std::make_shared<AndTerm>();
    copyTerms<BoolTerm::PtrVector, deepCopy>(t->_terms, _terms);
    return t;
}
std::shared_ptr<BoolTerm> BoolFactor::clone() const {
    std::shared_ptr<BoolFactor> t = std::make_shared<BoolFactor>();
    t->_hasNot = _hasNot;
    copyTerms<BoolFactorTerm::PtrVector, deepCopy>(t->_terms, _terms);
    return t;
}
std::shared_ptr<BoolTerm> UnknownTerm::clone() const {
    return  std::make_shared<UnknownTerm>(); // TODO what is unknown now?
}
BoolFactorTerm::Ptr PassListTerm::clone() const {
    PassListTerm* p = new PassListTerm;
    std::copy(_terms.begin(), _terms.end(), std::back_inserter(p->_terms));
    return BoolFactorTerm::Ptr(p);
}
BoolFactorTerm::Ptr BoolTermFactor::clone() const {
    BoolTermFactor* p = new BoolTermFactor;
    if (_term) { p->_term = _term->clone(); }
    return BoolFactorTerm::Ptr(p);
}
// copySyntax
std::shared_ptr<BoolTerm> OrTerm::copySyntax() const {
    std::shared_ptr<OrTerm> ot = std::make_shared<OrTerm>();
    copyTerms<BoolTerm::PtrVector, syntaxCopy>(ot->_terms, _terms);
    return ot;
}
bool OrTerm::merge(const BoolTerm& other) {
    auto otherOr = dynamic_cast<const OrTerm*>(&other);
    if (nullptr == otherOr) {
        return false;
    }
    _terms.insert(_terms.end(), otherOr->_terms.begin(), otherOr->_terms.end());
    return true;
}
void OrTerm::dbgPrint(std::ostream& os) const {
    os << "OrTerm(" << util::printable(_terms) << ")";
}
bool OrTerm::operator==(const BoolTerm& rhs) const {
    auto rhsOrTerm = dynamic_cast<OrTerm const *>(&rhs);
    if (nullptr == rhsOrTerm) {
        return false;
    }
    return util::vectorPtrCompare<BoolTerm>(_terms, rhsOrTerm->_terms);
}
std::shared_ptr<BoolTerm> AndTerm::copySyntax() const {
    std::shared_ptr<AndTerm> at = std::make_shared<AndTerm>();
    copyTerms<BoolTerm::PtrVector, syntaxCopy>(at->_terms, _terms);
    return at;
}
bool AndTerm::merge(const BoolTerm& other) {
    auto otherAnd = dynamic_cast<const AndTerm*>(&other);
    if (nullptr == otherAnd) {
        return false;
    }
    _terms.insert(_terms.end(), otherAnd->_terms.begin(), otherAnd->_terms.end());
    return true;
}
void AndTerm::dbgPrint(std::ostream& os) const {
    os << "AndTerm(" << util::printable(_terms) << ")";
}
bool AndTerm::operator==(const BoolTerm& rhs) const {
    auto rhsAndTerm = dynamic_cast<AndTerm const *>(&rhs);
    if (nullptr == rhsAndTerm) {
        return false;
    }
    return util::vectorPtrCompare<BoolTerm>(_terms, rhsAndTerm->_terms);
}
std::shared_ptr<BoolTerm> BoolFactor::copySyntax() const {
    std::shared_ptr<BoolFactor> bf = std::make_shared<BoolFactor>();
    bf->_hasNot = _hasNot;
    copyTerms<BoolFactorTerm::PtrVector, syntaxCopy>(bf->_terms, _terms);
    return bf;
}
void BoolFactor::dbgPrint(std::ostream& os) const {
    os << "BoolFactor(" << util::printable(_terms);
    if (_hasNot) {
        os << ", has NOT";
    }
    os << ")";
}
void UnknownTerm::dbgPrint(std::ostream& os) const {
    os << "UnknownTerm()";
}
bool UnknownTerm::operator==(const BoolTerm& rhs) const {
    return true;
}
BoolFactorTerm::Ptr PassTerm::copySyntax() const {
    PassTerm* p = new PassTerm;
    p->_text = _text;
    return BoolFactorTerm::Ptr(p);
}
void PassTerm::dbgPrint(std::ostream& os) const {
    os << "PassTerm('";
    if ("(" == _text) os << "LHP";
    else if (")" == _text) os << "RHP";
    else os << _text;
    os << "')";
}
bool PassTerm::operator==(const BoolFactorTerm& rhs) const {
    auto rhsPassTerm = dynamic_cast<PassTerm const *>(&rhs);
    if (nullptr == rhsPassTerm) {
        return false;
    }
    return _text == rhsPassTerm->_text;
}
BoolFactorTerm::Ptr PassListTerm::copySyntax() const {
    PassListTerm* p = new PassListTerm;
    p->_terms = _terms;
    return BoolFactorTerm::Ptr(p);
}
void PassListTerm::dbgPrint(std::ostream& os) const {
    os << "PassListTerm(" << util::printable(_terms) << ")";
}
bool PassListTerm::operator==(const BoolFactorTerm& rhs) const {
    auto rhsTerm = dynamic_cast<PassListTerm const *>(&rhs);
    if (nullptr == rhsTerm) {
        return false;
    }
    return _terms == rhsTerm->_terms;
}
BoolFactorTerm::Ptr BoolTermFactor::copySyntax() const {
    BoolTermFactor* p = new BoolTermFactor;
    if (_term) { p->_term = _term->copySyntax(); }
    return BoolFactorTerm::Ptr(p);
}
void BoolTermFactor::dbgPrint(std::ostream& os) const {
    os << "BoolTermFactor(" << _term << ")";
}
bool BoolTermFactor::operator==(const BoolFactorTerm& rhs) const {
    auto rhsTerm = dynamic_cast<BoolTermFactor const *>(&rhs);
    if (nullptr == rhsTerm) {
        return false;
    }
    return util::ptrCompare<BoolTerm>(_term, rhsTerm->_term);
}


}}} // namespace lsst::qserv::query
