// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2015 AURA/LSST.
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
 * @brief Implementation of helpers for ChunkSpec
 *
 * @author Daniel L. Wang, SLAC
 */

#include "qproc/ChunkSpec.h"

// System headers
#include <algorithm>
#include <cassert>
#include <iterator>
#include <stdexcept>

// Qserv headers
#include "util/Bug.h"
#include "util/IterableFormatter.h"

namespace {  // File-scope helpers
/// A "good" number of subchunks to include in a chunk query.  This is
/// a guess. The best value is an open question
// TODO:UJ `ChunkSpecFragmenter` has the purpose of limiting the
//         number of subchunks per ChunkSpec (which works out to
//         subchunkids per Job).
//         Each subchunk gets its own task on the worker, so this
//         is probably no longer helpful. Making the limit absurdly
//         high should have the effect of disabling the code
//         while checking if there are unexpected side effects.
// int const GOOD_SUBCHUNK_COUNT = 20;
int const GOOD_SUBCHUNK_COUNT = 2'000'000;
}  // namespace

namespace lsst::qserv::qproc {

std::ostream& operator<<(std::ostream& os, ChunkSpec const& c) {
    os << "ChunkSpec("
       << "chunkId=" << c.chunkId << ", "
       << "subChunks=" << util::printable(c.subChunks);
    os << ")";
    return os;
}

/// ChunkSpecVector intersection.
/// Computes ChunkSpecVector intersection, overwriting dest with the result.
///
/// precondition: Elements in ChunkSpecVector should be sorted and minimized in
/// the sense that there should be only one element for a particular
/// chunkId. i.e., for all ChunkSpec A element, there is no ChunkSpec B element
/// in the same vector where A.chunkId == B.chunkId.
void intersectSorted(ChunkSpecVector& dest, ChunkSpecVector const& a) {
    ChunkSpecVector tmp;
    ChunkSpecVector::iterator di = dest.begin();
    ChunkSpecVector::iterator de = dest.end();
    ChunkSpecVector::const_iterator ai = a.begin();
    ChunkSpecVector::const_iterator ae = a.end();
    for (; (di != de) && (ai != ae); ++di) {  // march down dest vector
        // For each item in dest, advance through a to find a matching chunkId.
        while (ai->chunkId < di->chunkId) {
            ++ai;
        }

        if (ai->chunkId == di->chunkId) {
            ChunkSpec cs = *di;
            // On a match, perform the intersection.
            cs.restrict(*ai);
            if (di->chunkId != ChunkSpec::CHUNKID_INVALID) {
                tmp.push_back(cs);
            }
        }
    }
    tmp.swap(dest);
}

ChunkSpecVector intersect(ChunkSpecVector const& a, ChunkSpecVector const& b) {
    ChunkSpecVector asort(a);
    ChunkSpecVector bsort(b);
    normalize(asort);
    normalize(bsort);
    intersectSorted(asort, bsort);
    return asort;
}

/// Merge and eliminate duplicates.
void normalize(ChunkSpecVector& specs) {
    // An in-place algorithm is possible, but slightly more difficult to
    // understand and debug.
    ChunkSpecVector output;
    std::sort(specs.begin(), specs.end());
    // Merge duplicate chunkId entries.
    for (ChunkSpecVector::iterator i = specs.begin(), e = specs.end(); i != e;) {  // Increment according to j
        i->normalize();
        ChunkSpecVector::iterator j;
        for (j = i + 1; (j != e) && i->chunkId == j->chunkId; ++j) {
            j->normalize();
            // Same chunkId, then merge and mark.
            i->mergeUnion(*j);
        }
        output.push_back(*i);
        i = j;
    }
    specs.swap(output);
}

////////////////////////////////////////////////////////////////////////
// ChunkSpec
////////////////////////////////////////////////////////////////////////
bool ChunkSpec::shouldSplit() const { return subChunks.size() > (unsigned)GOOD_SUBCHUNK_COUNT; }

ChunkSpec ChunkSpec::intersect(ChunkSpec const& cs) const {
    ChunkSpec output(*this);
    output.normalize();
    ChunkSpec rhs(cs);
    rhs.normalize();
    output.restrict(rhs);
    return output;
}

void ChunkSpec::restrict(ChunkSpec const& rhs) {
    if (chunkId != rhs.chunkId) {
        throw util::Bug(ERR_LOC, "ChunkSpec::merge with different chunkId");
    }
    Int32Vector output;
    output.reserve(rhs.subChunks.size());
    std::set_intersection(subChunks.begin(), subChunks.end(), rhs.subChunks.begin(), rhs.subChunks.end(),
                          std::insert_iterator<Int32Vector>(output, output.end()));
    subChunks.swap(output);
}

void ChunkSpec::mergeUnion(ChunkSpec const& rhs) {
    if (chunkId != rhs.chunkId) {
        throw util::Bug(ERR_LOC, "ChunkSpec::merge with different chunkId");
    }
    Int32Vector output(subChunks.size() + rhs.subChunks.size());
    std::merge(subChunks.begin(), subChunks.end(), rhs.subChunks.begin(), rhs.subChunks.end(),
               output.begin());
    output.erase(std::unique(output.begin(), output.end()), output.end());
    subChunks.swap(output);
}

void ChunkSpec::normalize() {
    std::sort(subChunks.begin(), subChunks.end());
    subChunks.erase(std::unique(subChunks.begin(), subChunks.end()), subChunks.end());
}

bool ChunkSpec::operator<(ChunkSpec const& rhs) const {
    typedef std::vector<int32_t>::const_iterator VecIter;

    if (chunkId < rhs.chunkId)
        return true;
    else if (chunkId > rhs.chunkId)
        return false;
    else {
        // Ideally, we would use std::mismatch(f1,l1,f2,l2), but that algo is
        // unavailable until c++14
        if (subChunks.size() != rhs.subChunks.size()) {
            return subChunks.size() < rhs.subChunks.size();
        }
        std::pair<VecIter, VecIter> mism =
                std::mismatch(subChunks.begin(), subChunks.end(), rhs.subChunks.begin());
        if (mism.first == subChunks.end()) {
            return mism.second != rhs.subChunks.end();
        } else if (mism.second == rhs.subChunks.end()) {
            return false;
        } else {  // both are valid;
            return ((*mism.first) < (*mism.second));
        }
    }
}

bool ChunkSpec::operator==(ChunkSpec const& rhs) const {
    if (chunkId != rhs.chunkId) return false;
    return subChunks == rhs.subChunks;
}

ChunkSpec ChunkSpec::makeFake(int chunkId, bool withSubChunks) {
    ChunkSpec cs;
    cs.chunkId = chunkId;
    assert(chunkId < 1000000);
    if (withSubChunks) {
        int base = 1000 * chunkId;
        cs.subChunks.push_back(base);
        cs.subChunks.push_back(base + 10);
        cs.subChunks.push_back(base + 20);
    }
    return cs;
}

////////////////////////////////////////////////////////////////////////
// ChunkSpecFragmenter
////////////////////////////////////////////////////////////////////////
ChunkSpecFragmenter::ChunkSpecFragmenter(ChunkSpec const& s)
        : _original(s), _pos(_original.subChunks.begin()) {}
ChunkSpec ChunkSpecFragmenter::get() const {
    ChunkSpec c;
    c.chunkId = _original.chunkId;
    Iter posEnd = _pos + GOOD_SUBCHUNK_COUNT;
    Iter end = _original.subChunks.end();
    if (posEnd >= end) {
        posEnd = end;
    }
    c.subChunks.resize(posEnd - _pos);
    std::copy(_pos, posEnd, c.subChunks.begin());
    return c;
}

void ChunkSpecFragmenter::next() { _pos += GOOD_SUBCHUNK_COUNT; }

bool ChunkSpecFragmenter::isDone() { return _pos >= _original.subChunks.end(); }
////////////////////////////////////////////////////////////////////////
// ChunkSpecSingle
////////////////////////////////////////////////////////////////////////
// precondition: !spec.subChunks.empty()
ChunkSpecSingle::Vector ChunkSpecSingle::makeVector(ChunkSpec const& spec) {
    Vector vector;
    if (spec.subChunks.empty()) {
        throw util::Bug(ERR_LOC, "Attempted subchunk spec list without subchunks.");
    }
    ChunkSpecSingle s;
    s.chunkId = spec.chunkId;
    std::vector<int>::const_iterator i;
    for (i = spec.subChunks.begin(); i != spec.subChunks.end(); ++i) {
        s.subChunkId = *i;
        vector.push_back(s);
    }
    return vector;
}

std::ostream& operator<<(std::ostream& os, ChunkSpecSingle const& c) {
    os << "(" << c.chunkId << "," << c.subChunkId << ")";
    return os;
}

}  // namespace lsst::qserv::qproc
