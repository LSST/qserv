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
#include "WorkerResourceLists.h"

#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <list>

// Third-party headers

// Qserv headers
#include "global/Bug.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.WorkerResources");
}

namespace lsst {
namespace qserv {
namespace czar {

std::string DbResource::getDbNameFromResource(std::string const& chunkResource) {
    ResourceUnit ru(chunkResource);
    if (ru.unitType() != ResourceUnit::UnitType::DBCHUNK) {
        return "";
    }
    return ru.db();
}


bool DbResource::insert(int chunkId) {
    std::lock_guard<std::mutex> lg(_mtx);
    return _chunkSet.insert(chunkId).second;
}


deque<int> DbResource::getDeque() {
    lock_guard<mutex> lg(_mtx);
    deque<int> dq(_chunkSet.begin(), _chunkSet.end()); // queue doesn't have this constructor
    return dq;
}


std::ostream& DbResource::dumpOS(std::ostream &os) const {
    lock_guard<mutex>  lg(_mtx);
    os << "DBResource:" << _dbName << " sz=" << _getSize() << " {";
    bool first = true;
    for (int chunkId:_chunkSet) {
        if (first) {
            first = false;
        } else {
            os << ",";
        }
        os << chunkId;
    }
    os << "}";
    return os;
}


std::string DbResource::dump() const {
    std::ostringstream os;
    dumpOS(os);
    return os.str();
}


std::ostream& operator<<(std::ostream &os, DbResource const& dbr) {
    return dbr.dumpOS(os);
}



bool WorkerResource::insert(string const& dbChunkResourceName) {
    ResourceUnit ru(dbChunkResourceName);
    string dbName = ru.db();
    int chunkId = ru.chunk();
    lock_guard<mutex> lg(_dbMtx);
    auto iter = _dbResources.find(dbName);
    if (iter == _dbResources.end()) {
        DbResource::Ptr dbR = make_shared<DbResource>(dbName);
        auto result = _dbResources.emplace(dbName, dbR);
        iter = result.first;
    }
    DbResource::Ptr const& dbR = iter->second;
    return dbR->insert(chunkId);
}


deque<int> WorkerResource::getDequeFor(string const& dbName) {
    deque<int> dq;
    lock_guard<mutex> lg(_dbMtx);
    auto iter = _dbResources.find(dbName);
    if (iter == _dbResources.end()) {
        return dq;
    }
    DbResource::Ptr const& dbR = iter->second;
    dq = dbR->getDeque();
    return dq;
}


std::ostream& WorkerResource::dumpOS(std::ostream &os) const {
    lock_guard<mutex>  lg(_dbMtx);
    os << "WorkerResource:" << _resourceName;
    for (auto const& dbRes:_dbResources) {
        os << "{db=" << dbRes.first << "{" << *(dbRes.second) << "}}";
    }
    return os;
}


std::string WorkerResource::dump() const {
    std::ostringstream os;
    dumpOS(os);
    return os.str();
}


std::ostream& operator<<(std::ostream &os, WorkerResource const& wr) {
    return wr.dumpOS(os);
}


map<string, deque<int>> WorkerResourceLists::getDequesFor(string const& dbName) {
    std::lock_guard<std::mutex> lg(_workerMapMtx);
    map<string, deque<int>> dqMap;
    for (auto&& elem:_workers) {
        string wName = elem.first;
        WorkerResource::Ptr const& wr = elem.second;
        dqMap.emplace(wName, wr->getDequeFor(dbName));
    }
    return dqMap;
}


bool WorkerResourceLists::readIn(std::string const& fName) {
    LOGS(_log, LOG_LVL_INFO, "&&& WorkerResources::readIn " << fName);

    // TODO:UJ THIS IS HARD CODED to wise_01 database. This would change
    //         to use chunk resource information for all databases
    //         pulled from a database or something similar.
    //         The final version would need to determine xrootd affinity
    //         and match the chunk list to that so 'leadChunks' are consistent.
    //         Possible way to determine affinity:
    //             Send out chunk resource requests with 'affinity' message
    //             instead of task message. The affinity message tell
    //             the worker it has the 'leadChunk' for that resource,
    //             and it sends back its worker id to the czar. These are
    //             both time stamped and cached -> sent back to central db
    //             Repeat individually when cache value is old, inaccurate,
    //             and/or refresh db oportunistically.

    fstream fs;
    fs.open(fName,ios::in);

    // working sets
    map<string, set<int>> workerChunkMap;
    set<int> foundChunks;

    // final map. The chunks should be in numerical order for each worker.
    map<string, deque<string>> resourceMap;
    if (fs.is_open()){
       string line;
       map<string, set<int>> tmpMap;
       while(getline(fs, line)) {
           auto pos = line.find_first_of(" ");
           string wNameShort = line.substr(0, pos);
           string chunkIdStr = line.substr(pos+1);
           int chunkId = std::stoi(chunkIdStr);
           //LOGS(_log, LOG_LVL_INFO, "&&& line='" << line << "' name=" << wNameShort << " chunk=" << chunkIdStr << " c=" << chunkId);

           /* &&&
           // Avoid making duplicate chunk entries
           auto ret = foundChunks.insert(chunkId);
           bool elementWasInserted = ret.second;
           if (elementWasInserted) {
               // need to add entry to the worker
               set<int>& chunkSet = workerChunkMap[wNameShort];
               chunkSet.insert(chunkId);
           }
           */
           // need to add entry to the worker
           tmpMap[wNameShort].insert(chunkId);

       }

       // Try to make fairly even distribution accross workers.
       list<string> workerNames;
       for (auto const& elem:tmpMap) {
           string name = elem.first;
           workerNames.push_back(name);
       }

       while (!workerNames.empty()) {
           for (auto wIter = workerNames.begin(); wIter != workerNames.end();) {
               auto wIterAdvanced = false;
               string wName = *wIter;
               set<int>& tmpChunkSet = tmpMap[wName];
               set<int>& chunkSet = workerChunkMap[wName];
               bool done = false;
               auto cIter = tmpChunkSet.begin();
               for (int j=0; j<10 && !done;) {
                   if (cIter == tmpChunkSet.end()) {
                       // no more chunks on this worker
                       done = true;
                       LOGS(_log, LOG_LVL_INFO, "&&& worker empty " << wName << " elemCount=" << chunkSet.size());
                       auto wIterToDel = wIter++;
                       wIterAdvanced = true;
                       workerNames.erase(wIterToDel);
                   } else {
                       int chunkId = *cIter;
                       auto ret = foundChunks.insert(chunkId);
                       bool elementInserted = ret.second;
                       if (elementInserted) {
                           ++j;
                           chunkSet.insert(chunkId);
                       }
                       auto cIterToDel = cIter++;
                       tmpChunkSet.erase(cIterToDel);
                   }
               }
               if (!wIterAdvanced) ++wIter;
           }
       }



       // At this point, there's a map of short worker names and integer chunkIds.
       // This needs to be turned in to a map of sets of chunk resource name keyed by
       // worker resource name.
       LOGS(_log, LOG_LVL_INFO, "&&& workerChunkMap sz=" << workerChunkMap.size());
       for (auto const& elem:workerChunkMap) {
           string shortName = elem.first;
           set<int> const& chunkInts = elem.second;
           string workerResourceN = "/worker/" + shortName;
           auto& chunkStrs = resourceMap[workerResourceN];
           for (int j:chunkInts) {
               string chunkResourceN = "/chk/wise_01/" + to_string(j);
               chunkStrs.push_back(chunkResourceN);
               //LOGS(_log, LOG_LVL_INFO, "&&& wRN=" << workerResourceN << " cRN=" << chunkResourceN);
           }
           LOGS(_log, LOG_LVL_INFO, "&&& wRes=" << workerResourceN << " chunks sz=" << chunkStrs.size());
       }


       // Finally, use resourceMap to load _workers.
       // It is expected that information from databases will
       // arrive in workerResourceName + chunkResourceName format.
       {
           std::lock_guard<std::mutex> lg(_workerMapMtx);
           LOGS(_log, LOG_LVL_INFO, "&&& resourceMap size=" << resourceMap.size());
           for (auto const& elem:resourceMap) {
               string wName = elem.first;
               auto res = _insertWorker(wName);
               WorkerResource::Ptr const& wr = res.first;
               deque<string> const& dq = elem.second;
               for(auto const& res:dq) {
                   wr->insert(res);
               }
           }
       }

       fs.close(); //close the file object.

       LOGS(_log, LOG_LVL_INFO, "&&& readIn " << *this);
       return true;
    }
    return false;
}





void WorkerResourceLists::setMonoNodeTest() {
    string wName("/worker/5257fbab-c49c-11eb-ba7a-1856802308a2");
    std::lock_guard<std::mutex> lg(_workerMapMtx);
    auto res = _insertWorker(wName);
    /* &&&
    auto iter = _workers.find(wName);
    if (iter == _workers.end()) {
        throw Bug("setMonoNodeTest Failed to find " + wName);
    }
    WorkerResource::Ptr const& wr = iter->second;
    */
    WorkerResource::Ptr const& wr = res.first;

    deque<string> dq = fillChunkIdSet();
    for(auto const& res:dq) {
        wr->insert(res);
    }
}

deque<string> WorkerResourceLists::fillChunkIdSet() {
    // &&& values for mono node test. TODO:UJ fill from database table or ?
    // &&& Make a function to convert these to dbName and chunkID using ResourceUnit and insert into the map.

    deque<string> dq;
    dq.push_back("/chk/qservTest_case01_qserv/1234567890");
    dq.push_back("/chk/qservTest_case01_qserv/6630");
    dq.push_back("/chk/qservTest_case01_qserv/6631");
    dq.push_back("/chk/qservTest_case01_qserv/6800");
    dq.push_back("/chk/qservTest_case01_qserv/6801");
    dq.push_back("/chk/qservTest_case01_qserv/6968");
    dq.push_back("/chk/qservTest_case01_qserv/6970");
    dq.push_back("/chk/qservTest_case01_qserv/6971");
    dq.push_back("/chk/qservTest_case01_qserv/7138");
    dq.push_back("/chk/qservTest_case01_qserv/7140");
    dq.push_back("/chk/qservTest_case01_qserv/7308");
    dq.push_back("/chk/qservTest_case01_qserv/7310");
    dq.push_back("/chk/qservTest_case01_qserv/7478");
    dq.push_back("/chk/qservTest_case01_qserv/7648");
    dq.push_back("/chk/qservTest_case02_qserv/1234567890");
    dq.push_back("/chk/qservTest_case02_qserv/7480");
    dq.push_back("/chk/qservTest_case03_qserv/1234567890");
    dq.push_back("/chk/qservTest_case03_qserv/7165");
    dq.push_back("/chk/qservTest_case04_qserv/1234567890");
    dq.push_back("/chk/qservTest_case04_qserv/6970");
    dq.push_back("/chk/qservTest_case04_qserv/7138");
    dq.push_back("/chk/qservTest_case01_qserv/7140");
    dq.push_back("/chk/qservTest_case01_qserv/7308");
    dq.push_back("/chk/qservTest_case01_qserv/7310");
    dq.push_back("/chk/qservTest_case01_qserv/7478");
    dq.push_back("/chk/qservTest_case01_qserv/7648");
    dq.push_back("/chk/qservTest_case02_qserv/1234567890");
    dq.push_back("/chk/qservTest_case02_qserv/7480");
    dq.push_back("/chk/qservTest_case03_qserv/1234567890");
    dq.push_back("/chk/qservTest_case03_qserv/7165");
    dq.push_back("/chk/qservTest_case04_qserv/1234567890");
    dq.push_back("/chk/qservTest_case04_qserv/6970");
    dq.push_back("/chk/qservTest_case04_qserv/7138");
    dq.push_back("/chk/qservTest_case04_qserv/7140");
    dq.push_back("/chk/qservTest_case04_qserv/7308");
    dq.push_back("/chk/qservTest_case04_qserv/7310");
    dq.push_back("/chk/qservTest_case05_qserv/1234567890");
    dq.push_back("/chk/qservTest_case05_qserv/4763");
    dq.push_back("/chk/qservTest_case05_qserv/4766");
    dq.push_back("/chk/qservTest_case05_qserv/4770");
    dq.push_back("/chk/qservTest_case05_qserv/4771");
    dq.push_back("/chk/qservTest_case05_qserv/4773");
    dq.push_back("/chk/qservTest_case05_qserv/4776");
    dq.push_back("/chk/qservTest_case05_qserv/4784");
    dq.push_back("/chk/qservTest_case05_qserv/4786");
    dq.push_back("/chk/qservTest_case05_qserv/4789");
    dq.push_back("/chk/qservTest_case05_qserv/4933");
    dq.push_back("/chk/qservTest_case05_qserv/4935");
    dq.push_back("/chk/qservTest_case05_qserv/4937");
    dq.push_back("/chk/qservTest_case05_qserv/4938");
    dq.push_back("/chk/qservTest_case05_qserv/4939");
    dq.push_back("/chk/qservTest_case05_qserv/4943");
    dq.push_back("/chk/qservTest_case05_qserv/4951");
    dq.push_back("/chk/qservTest_case05_qserv/4952");
    dq.push_back("/chk/qservTest_case05_qserv/4959");
    dq.push_back("/chk/qservTest_case05_qserv/5107");
    dq.push_back("/chk/qservTest_case05_qserv/5108");
    dq.push_back("/chk/qservTest_case05_qserv/5113");
    dq.push_back("/chk/qservTest_case05_qserv/5128");
    dq.push_back("/chk/qservTest_case05_qserv/5129");
    dq.push_back("/chk/qservTest_case05_qserv/5279");
    dq.push_back("/chk/qservTest_case05_qserv/5285");
    dq.push_back("/chk/qservTest_case05_qserv/5286");
    dq.push_back("/chk/qservTest_case05_qserv/5294");
    dq.push_back("/chk/qservTest_case05_qserv/5300");
    dq.push_back("/chk/qservTest_case05_qserv/5443");
    dq.push_back("/chk/qservTest_case05_qserv/5444");
    dq.push_back("/chk/qservTest_case05_qserv/5447");
    dq.push_back("/chk/qservTest_case05_qserv/5453");
    dq.push_back("/chk/qservTest_case05_qserv/5455");
    dq.push_back("/chk/qservTest_case05_qserv/5458");
    dq.push_back("/chk/qservTest_case05_qserv/5461");
    dq.push_back("/chk/qservTest_case05_qserv/5468");
    dq.push_back("/chk/qservTest_case05_qserv/5470");
    dq.push_back("/chk/qservTest_case05_qserv/5612");
    dq.push_back("/chk/qservTest_case05_qserv/5616");
    dq.push_back("/chk/qservTest_case05_qserv/5620");
    dq.push_back("/chk/qservTest_case05_qserv/5621");
    dq.push_back("/chk/qservTest_case05_qserv/5629");
    dq.push_back("/chk/qservTest_case05_qserv/5634");
    dq.push_back("/chk/qservTest_case05_qserv/5636");
    dq.push_back("/chk/qservTest_case05_qserv/5782");
    dq.push_back("/chk/qservTest_case05_qserv/5784");
    dq.push_back("/chk/qservTest_case05_qserv/5786");
    dq.push_back("/chk/qservTest_case05_qserv/5790");
    dq.push_back("/chk/qservTest_case05_qserv/5793");
    dq.push_back("/chk/qservTest_case05_qserv/5796");
    dq.push_back("/chk/qservTest_case05_qserv/5800");
    dq.push_back("/chk/qservTest_case05_qserv/5801");
    dq.push_back("/chk/qservTest_case05_qserv/5802");
    dq.push_back("/chk/qservTest_case05_qserv/5804");
    dq.push_back("/chk/qservTest_case05_qserv/5807");
    dq.push_back("/chk/qservTest_case05_qserv/5810");
    dq.push_back("/chk/qservTest_case05_qserv/5952");
    dq.push_back("/chk/qservTest_case05_qserv/5953");
    dq.push_back("/chk/qservTest_case05_qserv/5958");
    dq.push_back("/chk/qservTest_case05_qserv/5959");
    dq.push_back("/chk/qservTest_case05_qserv/5964");
    dq.push_back("/chk/qservTest_case05_qserv/5969");
    dq.push_back("/chk/qservTest_case05_qserv/5978");
    dq.push_back("/chk/qservTest_case05_qserv/6127");
    dq.push_back("/chk/qservTest_case05_qserv/6132");
    dq.push_back("/chk/qservTest_case05_qserv/6146");
    dq.push_back("/chk/qservTest_case05_qserv/6152");
    dq.push_back("/chk/qservTest_case05_qserv/6293");
    dq.push_back("/chk/qservTest_case05_qserv/6299");
    dq.push_back("/chk/qservTest_case05_qserv/6304");
    dq.push_back("/chk/qservTest_case05_qserv/6305");
    dq.push_back("/chk/qservTest_case05_qserv/6311");
    dq.push_back("/chk/qservTest_case05_qserv/6312");
    dq.push_back("/chk/qservTest_case05_qserv/6318");
    dq.push_back("/chk/qservTest_case05_qserv/6323");
    dq.push_back("/chk/qservTest_case05_qserv/6462");
    dq.push_back("/chk/qservTest_case05_qserv/6467");
    dq.push_back("/chk/qservTest_case05_qserv/6473");
    dq.push_back("/chk/qservTest_case05_qserv/6478");
    dq.push_back("/chk/qservTest_case05_qserv/6479");
    dq.push_back("/chk/qservTest_case05_qserv/6480");
    dq.push_back("/chk/qservTest_case05_qserv/6485");
    dq.push_back("/chk/qservTest_case05_qserv/6487");
    dq.push_back("/chk/qservTest_case05_qserv/6491");
    dq.push_back("/chk/qservTest_case05_qserv/6634");
    dq.push_back("/chk/qservTest_case05_qserv/6635");
    dq.push_back("/chk/qservTest_case05_qserv/6636");
    dq.push_back("/chk/qservTest_case05_qserv/6638");
    dq.push_back("/chk/qservTest_case05_qserv/6641");
    dq.push_back("/chk/qservTest_case05_qserv/6647");
    dq.push_back("/chk/qservTest_case05_qserv/6650");
    dq.push_back("/chk/qservTest_case05_qserv/6653");
    dq.push_back("/chk/qservTest_case05_qserv/6654");
    dq.push_back("/chk/qservTest_case05_qserv/6659");
    dq.push_back("/chk/qservTest_case05_qserv/6662");
    dq.push_back("/chk/qservTest_case05_qserv/6803");
    dq.push_back("/chk/qservTest_case05_qserv/6804");
    dq.push_back("/chk/qservTest_case05_qserv/6808");
    dq.push_back("/chk/qservTest_case05_qserv/6810");
    dq.push_back("/chk/qservTest_case05_qserv/6811");
    dq.push_back("/chk/qservTest_case05_qserv/6812");
    dq.push_back("/chk/qservTest_case05_qserv/6813");
    dq.push_back("/chk/qservTest_case05_qserv/6814");
    dq.push_back("/chk/qservTest_case05_qserv/6815");
    dq.push_back("/chk/qservTest_case05_qserv/6819");
    dq.push_back("/chk/qservTest_case05_qserv/6821");
    dq.push_back("/chk/qservTest_case05_qserv/6823");
    dq.push_back("/chk/qservTest_case05_qserv/6825");
    dq.push_back("/chk/qservTest_case05_qserv/6827");
    dq.push_back("/chk/qservTest_case05_qserv/6830");
    dq.push_back("/chk/qservTest_case05_qserv/6832");
    dq.push_back("/chk/qservTest_case05_qserv/6833");
    dq.push_back("/chk/qservTest_case05_qserv/6976");
    dq.push_back("/chk/qservTest_case05_qserv/6977");
    dq.push_back("/chk/qservTest_case05_qserv/6982");
    dq.push_back("/chk/qservTest_case05_qserv/6987");
    dq.push_back("/chk/qservTest_case05_qserv/6988");
    dq.push_back("/chk/qservTest_case05_qserv/6993");
    dq.push_back("/chk/qservTest_case05_qserv/6998");
    dq.push_back("/chk/qservTest_case05_qserv/6999");
    dq.push_back("/chk/qservTest_case05_qserv/7003");
    dq.push_back("/chk/qservTest_case05_qserv/7312");
    dq.push_back("/chk/qservTest_case05_qserv/7313");
    dq.push_back("/chk/qservTest_case05_qserv/7318");
    dq.push_back("/chk/qservTest_case05_qserv/7322");
    dq.push_back("/chk/qservTest_case05_qserv/7323");
    dq.push_back("/chk/qservTest_case05_qserv/7329");
    dq.push_back("/chk/qservTest_case05_qserv/7333");
    dq.push_back("/chk/qservTest_case05_qserv/7334");
    dq.push_back("/chk/qservTest_case05_qserv/7340");
    dq.push_back("/chk/qservTest_case05_qserv/7483");
    dq.push_back("/chk/qservTest_case05_qserv/7485");
    dq.push_back("/chk/qservTest_case05_qserv/7487");
    dq.push_back("/chk/qservTest_case05_qserv/7489");
    dq.push_back("/chk/qservTest_case05_qserv/7491");
    dq.push_back("/chk/qservTest_case05_qserv/7494");
    dq.push_back("/chk/qservTest_case05_qserv/7496");
    dq.push_back("/chk/qservTest_case05_qserv/7498");
    dq.push_back("/chk/qservTest_case05_qserv/7500");
    dq.push_back("/chk/qservTest_case05_qserv/7502");
    dq.push_back("/chk/qservTest_case05_qserv/7506");
    dq.push_back("/chk/qservTest_case05_qserv/7508");
    dq.push_back("/chk/qservTest_case05_qserv/7510");
    dq.push_back("/chk/qservTest_case05_qserv/7511");
    dq.push_back("/chk/qservTest_case05_qserv/7512");
    dq.push_back("/chk/qservTest_case05_qserv/7513");
    dq.push_back("/chk/qservTest_case05_qserv/7656");
    dq.push_back("/chk/qservTest_case05_qserv/7659");
    dq.push_back("/chk/qservTest_case05_qserv/7662");
    dq.push_back("/chk/qservTest_case05_qserv/7667");
    dq.push_back("/chk/qservTest_case05_qserv/7668");
    dq.push_back("/chk/qservTest_case05_qserv/7671");
    dq.push_back("/chk/qservTest_case05_qserv/7674");
    dq.push_back("/chk/qservTest_case05_qserv/7680");
    dq.push_back("/chk/qservTest_case05_qserv/7683");
    dq.push_back("/chk/qservTest_case05_qserv/7823");
    dq.push_back("/chk/qservTest_case05_qserv/7824");
    dq.push_back("/chk/qservTest_case05_qserv/7830");
    dq.push_back("/chk/qservTest_case05_qserv/7834");
    dq.push_back("/chk/qservTest_case05_qserv/7835");
    dq.push_back("/chk/qservTest_case05_qserv/7841");
    dq.push_back("/chk/qservTest_case05_qserv/7842");
    dq.push_back("/chk/qservTest_case05_qserv/7848");
    dq.push_back("/chk/qservTest_case05_qserv/7992");
    dq.push_back("/chk/qservTest_case05_qserv/7997");
    dq.push_back("/chk/qservTest_case05_qserv/8003");
    dq.push_back("/chk/qservTest_case05_qserv/8009");
    dq.push_back("/chk/qservTest_case05_qserv/8016");
    dq.push_back("/chk/qservTest_case05_qserv/8017");
    dq.push_back("/chk/qservTest_case05_qserv/8021");
    dq.push_back("/chk/qservTest_case05_qserv/8022");
    dq.push_back("/chk/qservTest_case05_qserv/8163");
    dq.push_back("/chk/qservTest_case05_qserv/8168");
    dq.push_back("/chk/qservTest_case05_qserv/8174");
    dq.push_back("/chk/qservTest_case05_qserv/8188");
    dq.push_back("/chk/qservTest_case05_qserv/8336");
    dq.push_back("/chk/qservTest_case05_qserv/8337");
    dq.push_back("/chk/qservTest_case05_qserv/8341");
    dq.push_back("/chk/qservTest_case05_qserv/8351");
    dq.push_back("/chk/qservTest_case05_qserv/8356");
    dq.push_back("/chk/qservTest_case05_qserv/8361");
    dq.push_back("/chk/qservTest_case05_qserv/8503");
    dq.push_back("/chk/qservTest_case05_qserv/8509");
    dq.push_back("/chk/qservTest_case05_qserv/8512");
    dq.push_back("/chk/qservTest_case05_qserv/8515");
    dq.push_back("/chk/qservTest_case05_qserv/8518");
    dq.push_back("/chk/qservTest_case05_qserv/8519");
    dq.push_back("/chk/qservTest_case05_qserv/8523");
    dq.push_back("/chk/qservTest_case05_qserv/8526");
    dq.push_back("/chk/qservTest_case05_qserv/8529");
    dq.push_back("/chk/qservTest_case05_qserv/8673");
    dq.push_back("/chk/qservTest_case05_qserv/8674");
    dq.push_back("/chk/qservTest_case05_qserv/8677");
    dq.push_back("/chk/qservTest_case05_qserv/8682");
    dq.push_back("/chk/qservTest_case05_qserv/8685");
    dq.push_back("/chk/qservTest_case05_qserv/8690");
    dq.push_back("/chk/qservTest_case05_qserv/8697");
    dq.push_back("/chk/qservTest_case05_qserv/8699");
    LOGS(_log, LOG_LVL_WARN, "&&& chunkIdset size=" << dq.size());
    return dq;
}


std::ostream& WorkerResourceLists::dumpOS(std::ostream &os) const {
    lock_guard<mutex>  lg(_workerMapMtx);
    os << "WorkerResourceLists:\n";
    for (auto const& worker:_workers) {
        os << "{worker=" << worker.first << "{" << *(worker.second) << "}}\n";
    }
    return os;
}

std::string WorkerResourceLists::dump() const {
    std::ostringstream os;
    dumpOS(os);
    return os.str();
}


std::ostream& operator<<(std::ostream &os, WorkerResourceLists const& wrs) {
    return wrs.dumpOS(os);
}


}}} // namespace lsst::qserv::czar

