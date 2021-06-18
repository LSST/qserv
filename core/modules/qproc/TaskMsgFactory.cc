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
  * @brief TaskMsgFactory is a factory for TaskMsg (protobuf) objects.
  *
  * @author Daniel L. Wang, SLAC
  */

// Class header
#include "qproc/TaskMsgFactory.h"

// System headers
#include <stdexcept>

// Third-party headers
#include <google/protobuf/arena.h>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/intTypes.h"
#include "qmeta/types.h"
#include "qproc/ChunkQuerySpec.h"
#include "qproc/QueryProcessingBug.h"
#include "util/common.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qproc.TaskMsgFactory");
}

namespace lsst {
namespace qserv {
namespace qproc {


bool TaskMsgFactory::fillTaskMsg(proto::TaskMsg* taskMsg, ChunkQuerySpec const& chunkQuerySpec,
                                 std::string const& chunkResultName, QueryId queryId, int jobId,
                                 int attemptCount, qmeta::CzarId czarId) {
    std::string resultTable("Asdfasfd");
    if (!chunkResultName.empty()) { resultTable = chunkResultName; }
    // shared
    taskMsg->set_session(_session);
    taskMsg->set_db(chunkQuerySpec.db);
    taskMsg->set_protocol(2);
    taskMsg->set_queryid(queryId);
    taskMsg->set_jobid(jobId);
    taskMsg->set_attemptcount(attemptCount);
    taskMsg->set_czarid(czarId);
    //LOGS(_log, LOG_LVL_INFO, "&&& _makeMsg ses=" << _session << " db=" << chunkQuerySpec.db << " qId=" << queryId << " jId=" << jobId << " att=" << attemptCount << " cz=" << czarId);
    // scanTables (for shared scans)
    // check if more than 1 db in scanInfo
    std::string db;
    for(auto const& sTbl : chunkQuerySpec.scanInfo.infoTables) {
        if (db.empty()) {
            db = sTbl.db;
        }
    }

    for(auto const& sTbl : chunkQuerySpec.scanInfo.infoTables) {
        lsst::qserv::proto::TaskMsg_ScanTable *msgScanTbl = taskMsg->add_scantable();
        sTbl.copyToScanTable(msgScanTbl);
    }

    taskMsg->set_scanpriority(chunkQuerySpec.scanInfo.scanRating);
    taskMsg->set_scaninteractive(chunkQuerySpec.scanInteractive);

    // per-chunk
    taskMsg->set_chunkid(chunkQuerySpec.chunkId);
    // per-fragment
    // TODO refactor to simplify
    if (chunkQuerySpec.nextFragment.get()) {
        ChunkQuerySpec const* sPtr = &chunkQuerySpec;
        while(sPtr) {
            LOGS(_log, LOG_LVL_TRACE, "nextFragment");
            for(unsigned int t=0;t<(sPtr->queries).size();t++){
                LOGS(_log, LOG_LVL_TRACE, (sPtr->queries).at(t));
            }
            // Linked fragments will not have valid subChunkTables vectors,
            // So, we reuse the root fragment's vector.
            _addFragment(*taskMsg, resultTable, chunkQuerySpec.subChunkTables,
                         sPtr->subChunkIds, sPtr->queries);
            sPtr = sPtr->nextFragment.get();
        }
    } else {
        LOGS(_log, LOG_LVL_TRACE, "no nextFragment");
        for(unsigned int t=0;t<(chunkQuerySpec.queries).size();t++){
            LOGS(_log, LOG_LVL_TRACE, (chunkQuerySpec.queries).at(t));
        }
        _addFragment(*taskMsg, resultTable, chunkQuerySpec.subChunkTables,
                     chunkQuerySpec.subChunkIds, chunkQuerySpec.queries);
    }
    return true;
}


void TaskMsgFactory::_addFragment(proto::TaskMsg& taskMsg, std::string const& resultName,
                                  DbTableSet const& subChunkTables,
                                  std::vector<int> const& subChunkIds,
                                  std::vector<std::string> const& queries) {
     proto::TaskMsg::Fragment* frag = taskMsg.add_fragment();
     frag->set_resulttable(resultName);

     for(auto& qry : queries)  {
         frag->add_query(qry);
     }

     proto::TaskMsg_Subchunk sc;

     // Add the db+table pairs to the subchunk.
     for(auto& tbl : subChunkTables) {
         proto::TaskMsg_Subchunk_DbTbl* dbTbl = sc.add_dbtbl();
         dbTbl->set_db(tbl.db);
         dbTbl->set_tbl(tbl.table);
         LOGS(_log, LOG_LVL_TRACE, "added dbtbl=" << tbl.db << "." << tbl.table);
     }

     for(auto& subChunkId : subChunkIds) {
         sc.add_id(subChunkId);
     }

     frag->mutable_subchunks()->CopyFrom(sc);
}


void TaskMsgFactory::serializeMsg(ChunkQuerySpec const& s, std::string const& chunkResultName,
                                  QueryId queryId, int jobId, int attemptCount, qmeta::CzarId czarId,
                                  std::ostream& os) {
    google::protobuf::Arena arena;
    proto::TaskMsg* taskMsg = google::protobuf::Arena::CreateMessage<proto::TaskMsg>(&arena);
    fillTaskMsg(taskMsg, s, chunkResultName, queryId, jobId, attemptCount, czarId);

    taskMsg->SerializeToOstream(&os);
}

}}} // namespace lsst::qserv::qproc
