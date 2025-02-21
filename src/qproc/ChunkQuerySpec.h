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
#ifndef LSST_QSERV_QPROC_CHUNKQUERYSPEC_H
#define LSST_QSERV_QPROC_CHUNKQUERYSPEC_H
/**
 * @file
 *
 * @author Daniel L. Wang, SLAC
 */

// System headers
#include <ostream>
#include <string>
#include <vector>

// Third-party headers
#include <memory>

// Qserv headers
#include "global/DbTable.h"
#include "global/stringTypes.h"
#include "protojson/ScanTableInfo.h"

namespace lsst::qserv::qproc {

/// ChunkQuerySpec is a value class that bundles a set of queries with their
/// dependent db, chunkId, and set of subChunkIds. It has a pointer to another
/// ChunkQuerySpec as a means of allowing Specs to be easily fragmented for
/// dispatch in smaller pieces.
class ChunkQuerySpec {
public:
    using Ptr = std::shared_ptr<ChunkQuerySpec>;

    ChunkQuerySpec() {}
    ChunkQuerySpec(std::string const& db_, int chunkId_, protojson::ScanInfo::Ptr const& scanInfo_,
                   bool scanInteractive_)
            : db(db_), chunkId(chunkId_), scanInfo(scanInfo_), scanInteractive(scanInteractive_) {}

    // Contents could change
    std::string db{""};  ///< dominant db
    int chunkId{0};
    protojson::ScanInfo::Ptr scanInfo;  ///< shared-scan candidates
    // Consider saving subChunkTable templates, and substituting the chunkIds
    // and subChunkIds into them on-the-fly.
    bool scanInteractive{false};
    DbTableSet subChunkTables;
    std::vector<int> subChunkIds;
    std::vector<std::string> queries;
    // Consider promoting the concept of container of ChunkQuerySpec
    // in the hopes of increased code cleanliness.
    std::shared_ptr<ChunkQuerySpec> nextFragment;  ///< ad-hoc linked list (consider removal)
};

std::ostream& operator<<(std::ostream& os, ChunkQuerySpec const& c);

}  // namespace lsst::qserv::qproc

#endif  // LSST_QSERV_QPROC_CHUNKQUERYSPEC_H
