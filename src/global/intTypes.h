// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2015 AURA/LSST.
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
#ifndef LSST_QSERV_INTTYPES_H
#define LSST_QSERV_INTTYPES_H
/**
 * @brief  Global int types
 *
 */
#include <stdint.h>
#include <set>
#include <vector>

namespace lsst::qserv {
typedef std::set<int> IntSet;
typedef std::vector<int> IntVector;
typedef std::vector<int32_t> Int32Vector;

/// Typedef for Query ID in query metadata.
typedef std::uint64_t QueryId;
typedef std::int64_t JobId;
typedef JobId UberJobId;           // These must be the same type.
typedef std::uint32_t CzarIdType;  // TODO:UJ remove qmeta::CzarId and rename this CzarId

/// Class to provide a consistent format for QueryIds in the log file
class QueryIdHelper {
public:
    /// Returns a standardized user query id string.
    /// @parameter qid - query id number.
    /// @parameter invalid - true, qid is not a valid user query id.
    static std::string makeIdStr(QueryId qid, bool invalid = false) {
        if (invalid) return "QID=?:";
        return "QID=" + std::to_string(qid) + ":";
    }

    /// Returns a standardized user query id string with jobId.
    /// @parameter qid - query id number.
    /// @parameter jobId - the job id number.
    /// @parameter invalid - true, qid is not a valid user query id.
    static std::string makeIdStr(QueryId qid, JobId jobId, bool invalid = false) {
        if (invalid) return makeIdStr(qid, true) + "?;";
        return makeIdStr(qid) + std::to_string(jobId) + ";";
    }
};

}  // namespace lsst::qserv
#endif  // LSST_QSERV_INTTYPES_H
