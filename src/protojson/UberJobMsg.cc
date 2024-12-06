/*
 * LSST Data Management System
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

// Class header
#include "protojson/UberJobMsg.h"

#include <stdexcept>

// Qserv headers
#include "http/Client.h"
#include "http/MetaModule.h"
#include "http/RequestBodyJSON.h"
#include "qdisp/JobQuery.h"
#include "qdisp/JobDescription.h"
#include "qproc/ChunkQuerySpec.h"
#include "util/common.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace nlohmann;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.protojson.UberJobMsg");
}  // namespace

namespace lsst::qserv::protojson {

UberJobMsg::UberJobMsg(unsigned int metaVersion, std::string const& replicationInstanceId,
                       std::string const& replicationAuthKey, CzarContactInfo::Ptr const& czInfo,
                       string const& workerId, QueryId qId, UberJobId ujId, int rowLimit, int maxTableSizeMB,
                       ScanInfo::Ptr const& scanInfo_,
                       std::vector<std::shared_ptr<qdisp::JobQuery>> const& jobs)
        : _metaVersion(metaVersion),
          _replicationInstanceId(replicationInstanceId),
          _replicationAuthKey(replicationAuthKey),
          _czInfo(czInfo),
          _workerId(workerId),
          _qId(qId),
          _ujId(ujId),
          _rowLimit(rowLimit),
          _maxTableSizeMB(maxTableSizeMB),
          _scanInfo(scanInfo_) {
    LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::UberJobMsg start");

    for (auto& jobPtr : jobs) {
        // This creates the JobMsg objects for all relates jobs and their fragments.
        auto jobMsg = JobMsg::create(jobPtr, _jobSubQueryTempMap, _jobDbTablesMap);
        _jobMsgVect->push_back(jobMsg);
    }
    LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::UberJobMsg end");
}

json UberJobMsg::serializeJson() const {
    LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::serializeJson a");

    json ujmJson = {{"version", _metaVersion},
                    {"instance_id", _replicationInstanceId},
                    {"auth_key", _replicationAuthKey},
                    {"worker", _workerId},
                    {"queryid", _qId},
                    {"uberjobid", _ujId},
                    {"czarinfo", _czInfo->serializeJson()},
                    {"rowlimit", _rowLimit},
                    {"subqueries_map", _jobSubQueryTempMap->serializeJson()},
                    {"dbtables_map", _jobDbTablesMap->serializeJson()},
                    {"maxtablesizemb", _maxTableSizeMB},
                    {"scaninfo", _scanInfo->serializeJson()},
                    {"jobs", json::array()}};

    auto& jsJobs = ujmJson["jobs"];
    for (auto const& jbMsg : *_jobMsgVect) {
        //&&&json jsJob = jbMsg->serializeJson();
        //&&&jsJobs.push_back(jsJob);
        jsJobs.emplace_back(jbMsg->serializeJson());
    }

    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " &&& ujmJson=" << ujmJson);
    return ujmJson;
}

UberJobMsg::Ptr UberJobMsg::createFromJson(nlohmann::json const& ujmJson) {
    LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::createFromJson ujmJson=" << ujmJson);
    try {
        if (ujmJson["version"] != http::MetaModule::version) {
            LOGS(_log, LOG_LVL_ERROR, "UberJobMsg::createFromJson bad version " << ujmJson["version"]);
            return nullptr;
        }

        auto czInfo_ = CzarContactInfo::createFromJson(ujmJson["czarinfo"]);
        if (czInfo_ == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, "UberJobMsg::createFromJson czar could not be parsed in " << ujmJson);
            return nullptr;
        }

        LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::createFromJson b");
        auto scanInfo_ = ScanInfo::createFromJson(ujmJson["scaninfo"]);
        if (scanInfo_ == nullptr) {
            LOGS(_log, LOG_LVL_ERROR,
                 "UberJobMsg::createFromJson scanInfo could not be parsed in " << ujmJson);
            return nullptr;
        }

        auto metaVersion = http::RequestBodyJSON::required<unsigned int>(ujmJson, "version");
        auto replicationInstanceId = http::RequestBodyJSON::required<string>(ujmJson, "instance_id");
        auto replicationAuthKey = http::RequestBodyJSON::required<string>(ujmJson, "auth_key");
        auto workerId = http::RequestBodyJSON::required<string>(ujmJson, "worker");
        auto qId = http::RequestBodyJSON::required<QueryId>(ujmJson, "queryid");
        auto ujId = http::RequestBodyJSON::required<UberJobId>(ujmJson, "uberjobid");
        auto rowLimit = http::RequestBodyJSON::required<int>(ujmJson, "rowlimit");
        auto maxTableSizeMB = http::RequestBodyJSON::required<int>(ujmJson, "maxtablesizemb");
        auto czInfo = CzarContactInfo::createFromJson(ujmJson["czarinfo"]);
        auto jsUjJobs = http::RequestBodyJSON::required<json>(ujmJson, "jobs");

        LOGS(_log, LOG_LVL_TRACE,
             " &&& " << metaVersion << replicationInstanceId << replicationAuthKey << workerId << qId << ujId
                     << rowLimit << jsUjJobs);

        std::vector<std::shared_ptr<qdisp::JobQuery>> emptyJobs;

        Ptr ujmPtr = Ptr(new UberJobMsg(metaVersion, replicationInstanceId, replicationAuthKey, czInfo,
                                        workerId, qId, ujId, rowLimit, maxTableSizeMB, scanInfo_, emptyJobs));

        LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::createFromJson m");
        auto const& jsSubQueriesMap = http::RequestBodyJSON::required<json>(ujmJson, "subqueries_map");
        ujmPtr->_jobSubQueryTempMap = JobSubQueryTempMap::createFromJson(jsSubQueriesMap);

        auto jsDbTablesMap = http::RequestBodyJSON::required<json>(ujmJson, "dbtables_map");
        ujmPtr->_jobDbTablesMap = JobDbTablesMap::createFromJson(jsDbTablesMap);

        for (auto const& jsUjJob : jsUjJobs) {
            JobMsg::Ptr jobMsgPtr =
                    JobMsg::createFromJson(jsUjJob, ujmPtr->_jobSubQueryTempMap, ujmPtr->_jobDbTablesMap);
            ujmPtr->_jobMsgVect->push_back(jobMsgPtr);
        }
        LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::createFromJson end");

        return ujmPtr;
    } catch (invalid_argument const& exc) {
        LOGS(_log, LOG_LVL_ERROR, "UberJobMsg::createFromJson invalid " << exc.what() << " json=" << ujmJson);
    }
    LOGS(_log, LOG_LVL_WARN, "&&& UberJobMsg::createFromJson end error");
    return nullptr;
}

std::string UberJobMsg::dump() const {
    stringstream os;
    os << "&&& NEEDS CODE";
    return os.str();
}

JobMsg::Ptr JobMsg::create(std::shared_ptr<qdisp::JobQuery> const& jobPtr,
                           JobSubQueryTempMap::Ptr const& jobSubQueryTempMap,
                           JobDbTablesMap::Ptr const& jobDbTablesMap) {
    auto jMsg = Ptr(new JobMsg(jobPtr, jobSubQueryTempMap, jobDbTablesMap));
    return jMsg;
}

JobMsg::JobMsg(std::shared_ptr<qdisp::JobQuery> const& jobPtr,
               JobSubQueryTempMap::Ptr const& jobSubQueryTempMap, JobDbTablesMap::Ptr const& jobDbTablesMap)
        : _jobSubQueryTempMap(jobSubQueryTempMap), _jobDbTablesMap(jobDbTablesMap) {
    LOGS(_log, LOG_LVL_WARN, "&&& JobMsg::JobMsg start");
    auto const descr = jobPtr->getDescription();
    if (descr == nullptr) {
        throw util::Bug(ERR_LOC, cName(__func__) + " description=null for job=" + jobPtr->getIdStr());
    }
    auto chunkQuerySpec = descr->getChunkQuerySpec();
    _jobId = descr->id();
    _attemptCount = descr->getAttemptCount();  // &&& may need to increment descr->AttemptCount at this time.
    _chunkQuerySpecDb = chunkQuerySpec->db;
    _scanRating = chunkQuerySpec->scanInfo->scanRating;
    _scanInteractive = chunkQuerySpec->scanInteractive;
    _chunkId = chunkQuerySpec->chunkId;
    _chunkResultName = descr->getChunkResultName();

    // Add scan tables (&&& not sure is this is the same for all jobs or not)
    for (auto const& sTbl : chunkQuerySpec->scanInfo->infoTables) {
        int index = jobDbTablesMap->findDbTable(make_pair(sTbl.db, sTbl.table));
        jobDbTablesMap->setScanRating(index, sTbl.scanRating, sTbl.lockInMemory);
        _chunkScanTableIndexes.push_back(index);
    }

    // Add fragments
    _jobFragments = JobFragment::createVect(*chunkQuerySpec, jobSubQueryTempMap, jobDbTablesMap);
    LOGS(_log, LOG_LVL_WARN, "&&& JobMsg::JobMsg end");
}

nlohmann::json JobMsg::serializeJson() const {
    LOGS(_log, LOG_LVL_WARN, "&&& JobMsg::serializeJson a");
    auto jsJobMsg = nlohmann::json({{"jobId", _jobId},
                                    {"attemptCount", _attemptCount},
                                    {"querySpecDb", _chunkQuerySpecDb},
                                    {"scanPriority", _scanRating},
                                    {"scanInteractive", _scanInteractive},
                                    {"chunkId", _chunkId},
                                    {"chunkresultname", _chunkResultName},
                                    {"chunkscantables_indexes", nlohmann::json::array()},
                                    {"queryFragments", json::array()}});

    // These are indexes into _jobDbTablesMap, which is shared between all JobMsg in this UberJobMsg.
    // &&& TODO:UJ queries appear to work even when "chunkscantables_indexes" is wrong
    auto& jsqCstIndexes = jsJobMsg["chunkscantables_indexes"];
    for (auto const& index : _chunkScanTableIndexes) {
        jsqCstIndexes.push_back(index);
    }

    auto& jsqFrags = jsJobMsg["queryFragments"];
    for (auto& jFrag : *_jobFragments) {
        //&&&auto jsFrag = jFrag->serializeJson();
        //&&&jsqFrags.push_back(jsFrag);
        jsqFrags.emplace_back(jFrag->serializeJson());
    }

    LOGS(_log, LOG_LVL_WARN, "&&& JobMsg::serializeJson end");
    return jsJobMsg;
}

JobMsg::JobMsg(JobSubQueryTempMap::Ptr const& jobSubQueryTempMap, JobDbTablesMap::Ptr const& jobDbTablesMap,
               JobId jobId, int attemptCount, std::string const& chunkQuerySpecDb, int scanRating,
               bool scanInteractive, int chunkId, std::string const& chunkResultName)
        : _jobId(jobId),
          _attemptCount(attemptCount),
          _chunkQuerySpecDb(chunkQuerySpecDb),
          _scanRating(scanRating),
          _scanInteractive(scanInteractive),
          _chunkId(chunkId),
          _chunkResultName(chunkResultName),
          _jobSubQueryTempMap(jobSubQueryTempMap),
          _jobDbTablesMap(jobDbTablesMap) {}

JobMsg::Ptr JobMsg::createFromJson(nlohmann::json const& ujJson,
                                   JobSubQueryTempMap::Ptr const& jobSubQueryTempMap,
                                   JobDbTablesMap::Ptr const& jobDbTablesMap) {
    LOGS(_log, LOG_LVL_WARN, "&&& JobMsg::createFromJson ujJson=" << ujJson);
    JobId jobId = http::RequestBodyJSON::required<JobId>(ujJson, "jobId");
    int attemptCount = http::RequestBodyJSON::required<int>(ujJson, "attemptCount");
    string chunkQuerySpecDb = http::RequestBodyJSON::required<string>(ujJson, "querySpecDb");
    int scanRating = http::RequestBodyJSON::required<int>(ujJson, "scanPriority");
    bool scanInteractive = http::RequestBodyJSON::required<bool>(ujJson, "scanInteractive");
    int chunkId = http::RequestBodyJSON::required<int>(ujJson, "chunkId");
    string chunkResultName = http::RequestBodyJSON::required<string>(ujJson, "chunkresultname");

    json jsQFrags = http::RequestBodyJSON::required<json>(ujJson, "queryFragments");

    Ptr jMsgPtr = Ptr(new JobMsg(jobSubQueryTempMap, jobDbTablesMap, jobId, attemptCount, chunkQuerySpecDb,
                                 scanRating, scanInteractive, chunkId, chunkResultName));
    json jsChunkTblIndexes = http::RequestBodyJSON::required<json>(ujJson, "chunkscantables_indexes");
    jMsgPtr->_chunkScanTableIndexes = jsChunkTblIndexes.get<std::vector<int>>();
    jMsgPtr->_jobFragments =
            JobFragment::createVectFromJson(jsQFrags, jMsgPtr->_jobSubQueryTempMap, jMsgPtr->_jobDbTablesMap);

    LOGS(_log, LOG_LVL_WARN, "&&& JobMsg::createFromJson end");
    return jMsgPtr;
}

json JobSubQueryTempMap::serializeJson() const {
    LOGS(_log, LOG_LVL_WARN, "&&& JobSubQueryTempMap::serializeJson a");

    // std::map<int, std::string> _qTemplateMap;
    json jsSubQueryTemplateMap = {{"subquerytemplate_map", json::array()}};

    LOGS(_log, LOG_LVL_TRACE,
         "&&& JobSubQueryTempMap::serializeJson jsSubQueryTemplateMap=" << jsSubQueryTemplateMap);
    auto& jsSqtMap = jsSubQueryTemplateMap["subquerytemplate_map"];
    for (auto const& [key, templ] : _qTemplateMap) {
        json jsElem = {{"index", key}, {"template", templ}};
        jsSqtMap.push_back(jsElem);
    }

    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " &&& " << jsSqtMap);

    LOGS(_log, LOG_LVL_WARN, "&&& JobSubQueryTempMap::serializeJson end");
    return jsSubQueryTemplateMap;
}

JobSubQueryTempMap::Ptr JobSubQueryTempMap::createFromJson(nlohmann::json const& ujJson) {
    LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson a");
    Ptr sqtMapPtr = create();
    LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson b");
    auto& sqtMap = sqtMapPtr->_qTemplateMap;
    LOGS(_log, LOG_LVL_WARN, "&&& JobSubQueryTempMap::createFromJson " << ujJson);
    auto const& jsElements = ujJson["subquerytemplate_map"];
    LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson c");
    for (auto const& jsElem : jsElements) {
        LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson c1");
        LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson jsElem=" << jsElem);
        //&&&int index = jsElem["index"];
        int index = http::RequestBodyJSON::required<int>(jsElem, "index");
        LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson c2");
        //&&&string templ = jsElem["template"];
        string templ = http::RequestBodyJSON::required<string>(jsElem, "template");
        LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson c3");
        auto res = sqtMap.insert(make_pair(index, templ));
        LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson c4");
        if (!res.second) {
            throw invalid_argument(sqtMapPtr->cName(__func__) + "index=" + to_string(index) + "=" + templ +
                                   " index already found in " + to_string(ujJson));
        }
        LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson c5");
    }
    LOGS(_log, LOG_LVL_WARN, "JobSubQueryTempMap::createFromJson end");
    return sqtMapPtr;
}

int JobSubQueryTempMap::findSubQueryTemp(string const& qTemp) {
    // The expected number of templates is expected to be small, less than 4,
    // so this shouldn't be horribly expensive.
    for (auto const& [key, temp] : _qTemplateMap) {
        if (temp == qTemp) {
            LOGS(_log, LOG_LVL_WARN, "&&& JobSubQueryTempMap::findSubQueryTemp end key=" << key);
            return key;
        }
    }

    // Need to insert
    int index = _qTemplateMap.size();
    _qTemplateMap[index] = qTemp;
    LOGS(_log, LOG_LVL_WARN, "&&& JobSubQueryTempMap::findSubQueryTemp end index=" << index);
    return index;
}

int JobDbTablesMap::findDbTable(pair<string, string> const& dbTablePair) {
    // The expected number of templates is expected to be small, less than 4,
    // so this shouldn't be horribly expensive.
    for (auto const& [key, dbTbl] : _dbTableMap) {
        if (dbTablePair == dbTbl) {
            return key;
        }
    }

    // Need to insert
    int index = _dbTableMap.size();
    _dbTableMap[index] = dbTablePair;
    return index;
}

json JobDbTablesMap::serializeJson() const {
    json jsDbTablesMap = {{"dbtable_map", json::array()}, {"scanrating_map", json::array()}};

    auto& jsDbTblMap = jsDbTablesMap["dbtable_map"];
    for (auto const& [key, valPair] : _dbTableMap) {
        json jsDbTbl = {{"index", key}, {"db", valPair.first}, {"table", valPair.second}};
        jsDbTblMap.push_back(jsDbTbl);
    }

    auto& jsScanRatingMap = jsDbTablesMap["scanrating_map"];
    for (auto const& [key, valPair] : _scanRatingMap) {
        json jsScanR = {{"index", key}, {"scanrating", valPair.first}, {"lockinmem", valPair.second}};
        jsScanRatingMap.push_back(jsScanR);
    }

    LOGS(_log, LOG_LVL_WARN, cName(__func__) << " &&& " << jsDbTablesMap);

    return jsDbTablesMap;
}

JobDbTablesMap::Ptr JobDbTablesMap::createFromJson(nlohmann::json const& ujJson) {
    Ptr dbTablesMapPtr = create();
    auto& dbTblMap = dbTablesMapPtr->_dbTableMap;
    auto& scanRMap = dbTablesMapPtr->_scanRatingMap;

    LOGS(_log, LOG_LVL_WARN, "&&& JobDbTablesMap::createFromJson " << ujJson);

    json const& jsDbTbl = ujJson["dbtable_map"];
    LOGS(_log, LOG_LVL_WARN, "&&& JobDbTablesMap::createFromJson dbtbl=" << jsDbTbl);
    for (auto const& jsElem : jsDbTbl) {
        //&&&int index = jsElem["index"];
        int index = http::RequestBodyJSON::required<int>(jsElem, "index");
        //&&&string db = jsElem["db"];
        string db = http::RequestBodyJSON::required<string>(jsElem, "db");
        //&&&string tbl = jsElem["table"];
        string tbl = http::RequestBodyJSON::required<string>(jsElem, "table");
        auto res = dbTblMap.insert(make_pair(index, make_pair(db, tbl)));
        if (!res.second) {
            throw invalid_argument(dbTablesMapPtr->cName(__func__) + " index=" + to_string(index) + "=" + db +
                                   +"." + tbl + " index already found in " + to_string(jsDbTbl));
        }
    }

    json const& jsScanR = ujJson["scanrating_map"];
    LOGS(_log, LOG_LVL_WARN, "&&& JobDbTablesMap::createFromJson jsScanR=" << jsScanR);
    for (auto const& jsElem : jsScanR) {
        //&&&int index = jsElem["index"];
        int index = http::RequestBodyJSON::required<int>(jsElem, "index");
        //&&&int scanR = jsElem["scanrating"];
        int scanR = http::RequestBodyJSON::required<int>(jsElem, "scanrating");
        //&&&bool lockInMem = jsElem["lockinmem"];
        bool lockInMem = http::RequestBodyJSON::required<bool>(jsElem, "lockinmem");
        auto res = scanRMap.insert(make_pair(index, make_pair(scanR, lockInMem)));
        if (!res.second) {
            throw invalid_argument(dbTablesMapPtr->cName(__func__) + " index=" + to_string(index) + "=" +
                                   to_string(scanR) + +", " + to_string(lockInMem) +
                                   " index already found in " + to_string(jsDbTbl));
        }
    }

    return dbTablesMapPtr;
}

void JobDbTablesMap::setScanRating(int index, int scanRating, bool lockInMemory) {
    auto iter = _scanRatingMap.find(index);
    if (iter == _scanRatingMap.end()) {
        _scanRatingMap[index] = make_pair(scanRating, lockInMemory);
    } else {
        auto& elem = *iter;
        auto& pr = elem.second;
        auto& [sRating, lInMem] = pr;
        if (sRating != scanRating || lInMem != lockInMemory) {
            auto [dbName, tblName] = getDbTable(index);
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) << " unexpected change in scanRating for " << dbName << "." << tblName
                                 << " from " << sRating << " to " << scanRating << " lockInMemory from "
                                 << lInMem << " to " << lockInMemory);
            if (scanRating > sRating) {
                sRating = scanRating;
                lInMem = lockInMemory;
            }
        }
    }
}

JobFragment::JobFragment(JobSubQueryTempMap::Ptr const& jobSubQueryTempMap,
                         JobDbTablesMap::Ptr const& jobDbTablesMap)
        : _jobSubQueryTempMap(jobSubQueryTempMap), _jobDbTablesMap(jobDbTablesMap) {}

JobFragment::VectPtr JobFragment::createVect(qproc::ChunkQuerySpec const& chunkQuerySpec,
                                             JobSubQueryTempMap::Ptr const& jobSubQueryTempMap,
                                             JobDbTablesMap::Ptr const& jobDbTablesMap) {
    LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::createVect start");
    VectPtr jFragments{new Vect()};
    if (chunkQuerySpec.nextFragment.get()) {
        qproc::ChunkQuerySpec const* sPtr = &chunkQuerySpec;
        while (sPtr) {
            LOGS(_log, LOG_LVL_TRACE, "nextFragment");
            for (unsigned int t = 0; t < (sPtr->queries).size(); t++) {  // &&& del loop
                LOGS(_log, LOG_LVL_DEBUG, __func__ << " q=" << (sPtr->queries).at(t));
            }
            /* &&&
            for (auto const& sbi : sPtr->subChunkIds) {  // &&& del loop
                LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::createVect a2a");
                LOGS(_log, LOG_LVL_DEBUG, __func__ << " sbi=" << sbi);
            }
            */
            // Linked fragments will not have valid subChunkTables vectors,
            // So, we reuse the root fragment's vector.
            _addFragment(*jFragments, chunkQuerySpec.subChunkTables, sPtr->subChunkIds, sPtr->queries,
                         jobSubQueryTempMap, jobDbTablesMap);
            sPtr = sPtr->nextFragment.get();
        }
    } else {
        LOGS(_log, LOG_LVL_TRACE, "no nextFragment");
        for (unsigned int t = 0; t < (chunkQuerySpec.queries).size(); t++) {  // &&& del loop
            LOGS(_log, LOG_LVL_TRACE, (chunkQuerySpec.queries).at(t));
        }
        _addFragment(*jFragments, chunkQuerySpec.subChunkTables, chunkQuerySpec.subChunkIds,
                     chunkQuerySpec.queries, jobSubQueryTempMap, jobDbTablesMap);
    }

    LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::createVect end");
    return jFragments;
}

//&&&void JobFragment::_addFragment(std::vector<Ptr>& jFragments, std::string const& resultTblName,
void JobFragment::_addFragment(std::vector<Ptr>& jFragments, DbTableSet const& subChunkTables,
                               std::vector<int> const& subchunkIds, std::vector<std::string> const& queries,
                               JobSubQueryTempMap::Ptr const& subQueryTemplates,
                               JobDbTablesMap::Ptr const& dbTablesMap) {
    LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::_addFragment a");
    Ptr jFrag = Ptr(new JobFragment(subQueryTemplates, dbTablesMap));

    // queries: The query string is stored in `_jobSubQueryTempMap` and the list of
    // integer indexes, `_subQueryTempIndexes`, points back to the specific template.
    for (auto& qry : queries) {
        int index = jFrag->_jobSubQueryTempMap->findSubQueryTemp(qry);
        jFrag->_jobSubQueryTempIndexes.push_back(index);
        LOGS(_log, LOG_LVL_TRACE, jFrag->cName(__func__) << "&&& added frag=" << qry << " index=" << index);
    }

    // Add the db+table pairs to the subchunks for the fragment.
    for (auto& tbl : subChunkTables) {
        int index = jFrag->_jobDbTablesMap->findDbTable(make_pair(tbl.db, tbl.table));
        jFrag->_jobDbTablesIndexes.push_back(index);
        LOGS(_log, LOG_LVL_TRACE,
             jFrag->cName(__func__) << " added dbtbl=" << tbl.db << "." << tbl.table << " index=" << index);
    }

    // Add subchunk id numbers
    for (auto& subchunkId : subchunkIds) {
        jFrag->_subchunkIds.push_back(subchunkId);
        LOGS(_log, LOG_LVL_TRACE, jFrag->cName(__func__) << " added subchunkId=" << subchunkId);
    }

    jFragments.push_back(move(jFrag));
    LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::_addFragment end");
}

string JobFragment::dump() const {
    stringstream os;
    os << " templateIndexes={";
    for (int j : _jobSubQueryTempIndexes) {
        os << j << ", ";
    }
    os << "} subchunkIds={";
    for (int j : _subchunkIds) {
        os << j << ", ";
    }
    os << "} dbtbl={";
    for (int j : _subchunkIds) {
        os << j << ", ";
    }
    os << "}";
    return os.str();
}

nlohmann::json JobFragment::serializeJson() const {
    json jsFragment = {{"subquerytemplate_indexes", _jobSubQueryTempIndexes},
                       {"dbtables_indexes", _jobDbTablesIndexes},
                       {"subchunkids", _subchunkIds}};

    LOGS(_log, LOG_LVL_TRACE, cName(__func__) << " " << jsFragment);
    return jsFragment;
}

JobFragment::VectPtr JobFragment::createVectFromJson(nlohmann::json const& jsFrags,
                                                     JobSubQueryTempMap::Ptr const& jobSubQueryTempMap,
                                                     JobDbTablesMap::Ptr const& dbTablesMap) {
    LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::createVectFromJson " << jsFrags);

    JobFragment::VectPtr jobFragments{new JobFragment::Vect()};

    for (auto const& jsFrag : jsFrags) {
        Ptr jobFrag = Ptr(new JobFragment(jobSubQueryTempMap, dbTablesMap));

        jobFrag->_jobSubQueryTempIndexes = jsFrag["subquerytemplate_indexes"].get<std::vector<int>>();
        for (int j : jobFrag->_jobSubQueryTempIndexes) {
            try {
                string tem = jobSubQueryTempMap->getSubQueryTemp(j);
                LOGS(_log, LOG_LVL_WARN, jobFrag->cName(__func__) << " &&&T j=" << j << " =" << tem);
            } catch (std::out_of_range const& ex) {
                LOGS(_log, LOG_LVL_ERROR,
                     jobFrag->cName(__func__) << " index=" << j << " not found in template map " << jsFrag);
                // rethrow as something callers expect.
                throw std::invalid_argument(jobFrag->cName(__func__) + " template index=" + to_string(j) +
                                            " " + ex.what());
            }
        }

        jobFrag->_jobDbTablesIndexes = jsFrag["dbtables_indexes"].get<std::vector<int>>();
        for (int j : jobFrag->_jobDbTablesIndexes) {
            try {
                auto dbTblPr = dbTablesMap->getDbTable(j);
                LOGS(_log, LOG_LVL_WARN,
                     jobFrag->cName(__func__)
                             << " &&&T j=" << j << " =" << dbTblPr.first << "." << dbTblPr.second);
            } catch (std::out_of_range const& ex) {
                LOGS(_log, LOG_LVL_ERROR,
                     jobFrag->cName(__func__) << " index=" << j << " not found in dbTable map " << jsFrag);
                // rethrow as something callers expect.
                throw std::invalid_argument(jobFrag->cName(__func__) + " dbtable index=" + to_string(j) +
                                            " " + ex.what());
            }
        }

        jobFrag->_subchunkIds = jsFrag["subchunkids"].get<std::vector<int>>();
        jobFragments->push_back(jobFrag);
    }

    LOGS(_log, LOG_LVL_WARN, "&&& JobFragment::createVectFromJson end");
    return jobFragments;
}

}  // namespace lsst::qserv::protojson
