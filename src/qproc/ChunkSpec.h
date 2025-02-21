// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2015 LSST Corporation.
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
#ifndef LSST_QSERV_QPROC_CHUNKSPEC_H
#define LSST_QSERV_QPROC_CHUNKSPEC_H
/**
 * @file
 *
 * @brief ChunkSpec, ChunkSpecFragmenter, and ChunkSpecSingle declarations
 *
 * @author Daniel L. Wang, SLAC
 */

// System headers
#include <ostream>
#include <list>
#include <map>
#include <vector>

// Qserv headers
#include "global/intTypes.h"

namespace lsst::qserv::qproc {

/// ChunkSpec is a value class that bundles the per-chunk information that is
/// used to compose a concrete chunk query for a specific chunk from an input
/// parsed query statement. Contains a specification of chunkId and subChunkId
/// list.
/// Do not inherit.
struct ChunkSpec {
public:
    static int32_t const CHUNKID_INVALID = -1;

    ChunkSpec() : chunkId(CHUNKID_INVALID) {}
    ChunkSpec(int chunkId_, Int32Vector const& subChunks_) : chunkId(chunkId_), subChunks(subChunks_) {}

    int32_t chunkId;  ///< ChunkId of interest
    /// Subchunks of interest; empty indicates all subchunks are involved.
    Int32Vector subChunks;

    bool shouldSplit() const;

    /// @return the intersection with the chunk.
    /// If both ChunkSpecs have non-empty subChunks, but do not intersect,
    /// chunkId is set to be invalid (-1)
    ChunkSpec intersect(ChunkSpec const& cs) const;
    /// Restrict the existing ChunkSpec to contain no more than another
    /// (in-place intersection). Both must be normalized.
    void restrict(ChunkSpec const& cs);
    /// Merge another ChunkSpec with the same chunkId, assuming both are
    /// normalized. *this remains normalized upon completion.
    void mergeUnion(ChunkSpec const& rhs);
    void normalize();

    bool operator<(ChunkSpec const& rhs) const;
    bool operator==(ChunkSpec const& rhs) const;

    // For testing
    static ChunkSpec makeFake(int chunkId, bool withSubChunks = false);
};
std::ostream& operator<<(std::ostream& os, ChunkSpec const& c);

typedef std::vector<ChunkSpec> ChunkSpecVector;
typedef std::map<int, ChunkSpec> ChunkSpecMap;

/// Compute an intersection, assuming both dest and a are sorted and minimized.
void intersectSorted(ChunkSpecVector& dest, ChunkSpecVector const& a);

/// ChunkSpecVector intersection.
/// Computes ChunkSpecVector intersection, overwriting dest with the result.
ChunkSpecVector intersect(ChunkSpecVector const& a, ChunkSpecVector const& b);

/// Merge and eliminate duplicates.
void normalize(ChunkSpecVector& specs);

/// An iterating fragmenter to reduce the number of subChunkIds per ChunkSpec
/// TODO:UJ Fragmenting the the Jobs probably no longer makes sense, see
///         `GOOD_SUBCHUNK_COUNT` definition.
class ChunkSpecFragmenter {
public:
    ChunkSpecFragmenter(ChunkSpec const& s);
    ChunkSpec get() const;
    void next();
    bool isDone();

private:
    typedef std::vector<int32_t>::const_iterator Iter;
    ChunkSpec _original;
    Iter _pos;
};
/// A specification of ChunkSpec with only one subChunk
/// TODO: Consider renaming this. (SubChunkSpec?)
class ChunkSpecSingle {
public:
    typedef std::vector<ChunkSpecSingle> Vector;
    ChunkSpecSingle() : chunkId(-1), subChunkId(-1) {}
    int32_t chunkId;
    int32_t subChunkId;
    static Vector makeVector(ChunkSpec const& spec);
};
std::ostream& operator<<(std::ostream& os, ChunkSpecSingle const& c);

}  // namespace lsst::qserv::qproc

#endif  // LSST_QSERV_QPROC_CHUNKSPEC_H
