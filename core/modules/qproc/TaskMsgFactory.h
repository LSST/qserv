// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2017 LSST Corporation.
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

#ifndef LSST_QSERV_QPROC_TASKMSGFACTORY_H
#define LSST_QSERV_QPROC_TASKMSGFACTORY_H
/**
  * @file
  *
  * @brief TaskMsgFactory is a factory for TaskMsg (protobuf) objects.
  *
  * @author Daniel L. Wang, SLAC
  */

// System headers
#include <iostream>
#include <memory>

// Qserv headers
#include "global/DbTable.h"
#include "global/intTypes.h"
#include "proto/worker.pb.h"
#include "qmeta/types.h"

namespace lsst {
namespace qserv {
namespace qproc {

class ChunkQuerySpec;

/// TaskMsgFactory is a factory for TaskMsg (protobuf) objects.
/// All member variables must be thread safe.
class TaskMsgFactory {
public:
    using Ptr = std::shared_ptr<TaskMsgFactory>;

    TaskMsgFactory(uint64_t session) : _session(session) {}
    virtual ~TaskMsgFactory() {}

    /// Construct a TaskMsg and serialize it to a stream
    virtual void serializeMsg(ChunkQuerySpec const& s,
                      std::string const& chunkResultName,
                      QueryId queryId, int jobId, int attemptCount,
                      qmeta::CzarId czarId,
                      std::ostream& os);

    /// Use the provided information to fill in taskMsg.
    /// @return true if successful.
    bool fillTaskMsg(proto::TaskMsg* taskMsg, ChunkQuerySpec const& s,
                     std::string const& chunkResultName,
                     QueryId queryId, int jobId, int attemptCount,
                     qmeta::CzarId czarId);

private:
    void _addFragment(proto::TaskMsg& taskMsg, std::string const& resultName,
                      DbTableSet const& subChunkTables, std::vector<int> const& subChunkIds,
                      std::vector<std::string> const& queries);

    /// All member variable need to be thread safe.
    uint64_t const _session;
};

}}} // namespace lsst::qserv::qproc

#endif // LSST_QSERV_QPROC_TASKMSGFACTORY_H

