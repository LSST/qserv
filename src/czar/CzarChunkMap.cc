// -*- LSST-C++ -*-
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
#include "czar/CzarChunkMap.h"

// System headers
#include <sstream>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "qmeta/QMeta.h"
#include "cconfig/CzarConfig.h"
#include "czar/Czar.h"
#include "czar/CzarRegistry.h"
#include "qmeta/Exceptions.h"
#include "util/Bug.h"
#include "util/TimeUtils.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.CzarChunkMap");
}  // namespace

namespace lsst::qserv::czar {

CzarChunkMap::CzarChunkMap() {}

CzarChunkMap::~CzarChunkMap() { LOGS(_log, LOG_LVL_DEBUG, "CzarChunkMap::~CzarChunkMap()"); }

void CzarChunkMap::calcChunkMap(ChunkMap const& chunkMap, ChunkVector& chunksSortedBySize) {
    // Calculate total bytes for all chunks.
    for (auto&& [chunkIdNum, chunkData] : chunkMap) {
        chunkData->_calcTotalBytes();
        chunksSortedBySize.push_back(chunkData);
    }

    sortChunks(chunksSortedBySize);
}

void CzarChunkMap::sortChunks(std::vector<ChunkData::Ptr>& chunksSortedBySize) {
    /// Return true if a->_totalBytes > b->_totalBytes
    auto sortBySizeDesc = [](ChunkData::Ptr const& a, ChunkData::Ptr const& b) {
        if (b == nullptr && a != nullptr) return true;
        if (a == nullptr) return false;
        return a->_totalBytes > b->_totalBytes;
    };

    std::sort(chunksSortedBySize.begin(), chunksSortedBySize.end(), sortBySizeDesc);
}

void CzarChunkMap::verify() {
    auto&& wcMap = *_workerChunkMap;
    auto&& chunkMap = *_chunkMap;
    // Use a set to prevent duplicate ids caused by replication levels > 1.
    set<int64_t> allChunkIds;
    int errorCount = 0;
    for (auto const& [wkr, wkrData] : wcMap) {
        for (auto const& [chunkId, chunkData] : wkrData->_chunkDataMap) {
            allChunkIds.insert(chunkId);
        }
    }

    for (auto const& [chunkId, chunkDataPtr] : chunkMap) {
        if (chunkDataPtr == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " chunkId=" << chunkId << " had nullptr");
            ++errorCount;
            continue;
        }
        auto primeScanWkr = chunkDataPtr->_primaryScanWorker.lock();
        if (primeScanWkr == nullptr) {
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) << " chunkId=" << chunkId << " missing primaryScanWorker");
            ++errorCount;
            continue;
        }
        if (primeScanWkr->_sharedScanChunkMap.find(chunkId) == primeScanWkr->_sharedScanChunkMap.end()) {
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) << " chunkId=" << chunkId
                                 << " should have been (and was not) in the sharedScanChunkMap for "
                                 << primeScanWkr->_workerId);
            ++errorCount;
            continue;
        }
        auto iter = allChunkIds.find(chunkId);
        if (iter != allChunkIds.end()) {
            allChunkIds.erase(iter);
        } else {
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) << " chunkId=" << chunkId << " chunkId was not in allChunks list");
            ++errorCount;
            continue;
        }
    }

    auto missing = allChunkIds.size();
    if (missing > 0) {
        string allMissingIds;
        for (auto const& cId : allChunkIds) {
            allMissingIds += to_string(cId) + ",";
        }
        LOGS(_log, LOG_LVL_ERROR,
             cName(__func__) << " There were " << missing << " missing chunks from the scan list "
                             << allMissingIds);
        ++errorCount;
    }

    if (errorCount > 0) {
        // Original creation of the family map will keep re-reading until there are no problems.
        // _monitor will log this and keep using the old maps.
        throw ChunkMapException(ERR_LOC, "verification failed with " + to_string(errorCount) + " errors");
    }
}

string CzarChunkMap::dumpChunkMap(ChunkMap const& chunkMap) {
    stringstream os;
    os << "ChunkMap{";
    for (auto const& [cId, cDataPtr] : chunkMap) {
        os << "(cId=" << cId << ":";
        os << ((cDataPtr == nullptr) ? "null" : cDataPtr->dump()) << ")";
    }
    os << "}";
    return os.str();
}

string CzarChunkMap::dumpWorkerChunkMap(WorkerChunkMap const& wcMap) {
    stringstream os;
    os << "WorkerChunkMap{";
    for (auto const& [wId, wDataPtr] : wcMap) {
        os << "(wId=" << wId << ":";
        os << ((wDataPtr == nullptr) ? "null" : wDataPtr->dump()) << ")";
    }
    os << "}";
    return os.str();
}

void CzarChunkMap::ChunkData::_calcTotalBytes() {
    _totalBytes = 0;
    for (auto const& [key, val] : _dbTableMap) {
        _totalBytes += val;
    }
}

void CzarChunkMap::ChunkData::addToWorkerHasThis(std::shared_ptr<WorkerChunksData> const& worker) {
    if (worker == nullptr) {
        throw ChunkMapException(ERR_LOC, cName(__func__) + " worker was null");
    }

    _workerHasThisMap[worker->_workerId] = worker;
}

map<string, weak_ptr<CzarChunkMap::WorkerChunksData>> CzarChunkMap::ChunkData::getWorkerHasThisMapCopy()
        const {
    map<string, weak_ptr<WorkerChunksData>> newMap = _workerHasThisMap;
    return newMap;
}

shared_ptr<CzarChunkMap::ChunkVector> CzarChunkMap::organize() {
    auto chunksSortedBySize = make_shared<ChunkVector>();
    auto missingChunks = make_shared<ChunkVector>();

    calcChunkMap(*_chunkMap, *chunksSortedBySize);

    // At this point we have
    //  - _workerChunkMap has a map of workerData by worker id with each worker having a map of ChunkData
    //  - _chunkMap has a map of all chunkData by chunk id
    //  - chunksSortedBySize a list of chunks sorted with largest first.
    // From here need to assign shared scan chunk priority (i.e. the worker
    //    that will handle the chunk in shared scans, unless it is dead.)
    // Go through the chunksSortedBySize list and assign each chunk to worker that has both:
    //    - a copy of the chunk
    //    - the worker currently has the smallest totalScanSize.
    // When this is done, all workers should have lists of chunks with similar total sizes
    // and missing chunks should be empty.
    for (auto&& chunkData : *chunksSortedBySize) {
        SizeT smallest = std::numeric_limits<SizeT>::max();
        WorkerChunksData::Ptr smallestWkr = nullptr;
        // Find worker with smallest total size.
        for (auto&& [wkrId, wkrDataWeak] : chunkData->_workerHasThisMap) {
            auto wkrData = wkrDataWeak.lock();
            if (wkrData == nullptr) {
                LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " unexpected null weak ptr for " << wkrId);
                continue;  // maybe the next one will be okay.
            }

            LOGS(_log, LOG_LVL_DEBUG,
                 cName(__func__) << " wkrId=" << wkrData << " tsz=" << wkrData->_sharedScanTotalSize
                                 << " smallest=" << smallest);
            if (wkrData->_sharedScanTotalSize < smallest) {
                smallestWkr = wkrData;
                smallest = smallestWkr->_sharedScanTotalSize;
            }
        }
        if (smallestWkr == nullptr) {
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) + " no smallesWkr found for chunk=" + to_string(chunkData->_chunkId));
            missingChunks->push_back(chunkData);
        } else {
            smallestWkr->_sharedScanChunkMap[chunkData->_chunkId] = chunkData;
            smallestWkr->_sharedScanTotalSize += chunkData->_totalBytes;
            chunkData->_primaryScanWorker = smallestWkr;
            LOGS(_log, LOG_LVL_DEBUG,
                 " chunk=" << chunkData->_chunkId << " assigned to scan on " << smallestWkr->_workerId);
        }
    }
    return missingChunks;
}

string CzarChunkMap::ChunkData::dump() const {
    stringstream os;
    auto primaryWorker = _primaryScanWorker.lock();
    os << "{ChunkData id=" << _chunkId << " totalBytes=" << _totalBytes;
    os << " primaryWorker=" << ((primaryWorker == nullptr) ? "null" : primaryWorker->_workerId);
    os << " workers{";
    for (auto const& [wId, wData] : _workerHasThisMap) {
        os << "(" << wId << ")";
    }
    os << "} tables{";
    for (auto const& [dbTbl, sz] : _dbTableMap) {
        os << "(" << dbTbl.first << "." << dbTbl.second << " sz=" << sz << ")";
    }
    os << "}}";
    return os.str();
}

bool CzarChunkMap::WorkerChunksData::isDead() {
    if (_activeWorker == nullptr) {
        // At startup, these may not be available
        auto czarPtr = Czar::getCzar();
        if (czarPtr == nullptr) {
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) << " czarPtr is null, this should only happen in unit test.");
            return false;
        }
        auto awMap = Czar::getCzar()->getActiveWorkerMap();
        if (awMap == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " awMap is null.");
            return true;
        }
        _activeWorker = awMap->getActiveWorker(_workerId);
        if (_activeWorker == nullptr) {
            LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " activeWorker not found.");
            return true;
        }
    }
    auto wState = _activeWorker->getState();
    bool dead = wState == ActiveWorker::DEAD;
    if (dead) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " is dead");
    }
    return dead;
}

string CzarChunkMap::WorkerChunksData::dump() const {
    stringstream os;
    os << "{WorkerChunksData id=" << _workerId << " scanTotalSize=" << _sharedScanTotalSize;
    os << " chunkDataIds{";
    for (auto const& [chunkId, chunkData] : _chunkDataMap) {
        os << "(" << chunkId << ")";
    }
    os << "} sharedScanChunks{";
    for (auto const& [chunkId, chunkData] : _sharedScanChunkMap) {
        os << "(" << chunkId << ")";
    }
    os << "}}";
    return os.str();
}

CzarFamilyMap::Ptr CzarFamilyMap::create(std::shared_ptr<qmeta::QMeta> const& qmeta) {
    // There's nothing the czar can do until with user queries until there's been at least
    // one successful read of the database family tables, as the czar doesn't know where to find anything.
    Ptr newPtr = nullptr;
    while (newPtr == nullptr) {
        try {
            newPtr = Ptr(new CzarFamilyMap(qmeta));
        } catch (ChunkMapException const& exc) {
            LOGS(_log, LOG_LVL_WARN, "Could not create CzarFamilyMap, sleep and retry " << exc.what());
        }
        if (newPtr == nullptr) {
            this_thread::sleep_for(10s);
        }
    }

    return newPtr;
}

CzarFamilyMap::CzarFamilyMap(std::shared_ptr<qmeta::QMeta> const& qmeta) : _qmeta(qmeta) {
    try {
        auto mapsSet = _read();
        if (!mapsSet) {
            throw ChunkMapException(ERR_LOC, cName(__func__) + " maps were not set in constructor");
        }
    } catch (qmeta::QMetaError const& qExc) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) << " could not read DB " << qExc.what());
        throw ChunkMapException(ERR_LOC, cName(__func__) + " constructor failed read " + qExc.what());
    }
}

bool CzarFamilyMap::read() {
    bool mapsSet = false;
    try {
        mapsSet = _read();
    } catch (qmeta::QMetaError const& qExc) {
        LOGS(_log, LOG_LVL_ERROR, cName(__func__) + " could not read DB " << qExc.what());
    }
    return mapsSet;
}

bool CzarFamilyMap::_read() {
    LOGS(_log, LOG_LVL_TRACE, "CzarFamilyMap::_read() start");
    // If replacing the map, this may take a bit of time, but it's probably
    // better to wait for new maps if something changed.
    std::lock_guard gLock(_familyMapMtx);
    qmeta::QMetaChunkMap qChunkMap = _qmeta->getChunkMap(_lastUpdateTime);
    if (_lastUpdateTime == qChunkMap.updateTime) {
        LOGS(_log, LOG_LVL_DEBUG,
             cName(__func__) << " no need to read "
                             << util::TimeUtils::timePointToDateTimeString(_lastUpdateTime)
                             << " db=" << util::TimeUtils::timePointToDateTimeString(qChunkMap.updateTime));
        return false;
    }

    // Make the new maps.
    auto czConfig = cconfig::CzarConfig::instance();
    bool usingChunkSize = czConfig->getFamilyMapUsingChunkSize();
    shared_ptr<CzarFamilyMap::FamilyMapType> familyMapPtr = makeNewMaps(qChunkMap, usingChunkSize);

    verify(familyMapPtr);

    _familyMap = familyMapPtr;

    _lastUpdateTime = qChunkMap.updateTime;

    LOGS(_log, LOG_LVL_TRACE, "CzarChunkMap::_read() end");
    return true;
}

std::shared_ptr<CzarFamilyMap::FamilyMapType> CzarFamilyMap::makeNewMaps(
        qmeta::QMetaChunkMap const& qChunkMap, bool usingChunkSize) {
    // Create new maps.
    std::shared_ptr<FamilyMapType> newFamilyMap = make_shared<FamilyMapType>();

    // Workers -> Databases map
    LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " workers.sz=" << qChunkMap.workers.size());
    for (auto const& [workerId, dbs] : qChunkMap.workers) {
        // Databases -> Tables map
        for (auto const& [dbName, tables] : dbs) {
            // Tables -> Chunks map
            for (auto const& [tableName, chunks] : tables) {
                // vector of ChunkInfo
                for (qmeta::QMetaChunkMap::ChunkInfo const& chunkInfo : chunks) {
                    try {
                        int64_t chunkNum = chunkInfo.chunk;
                        CzarChunkMap::SizeT sz = 1;
                        if (usingChunkSize) {
                            sz = chunkInfo.size;
                        }
                        LOGS(_log, LOG_LVL_DEBUG,
                             cName(__func__) << "workerdId=" << workerId << " db=" << dbName << " table="
                                             << tableName << " chunk=" << chunkNum << " sz=" << sz);
                        insertIntoMaps(newFamilyMap, workerId, dbName, tableName, chunkNum, sz);
                    } catch (invalid_argument const& exc) {
                        throw ChunkMapException(
                                ERR_LOC, cName(__func__) + " invalid_argument workerdId=" + workerId +
                                                 " db=" + dbName + " table=" + tableName +
                                                 " chunk=" + to_string(chunkInfo.chunk) + " " + exc.what());
                    } catch (out_of_range const& exc) {
                        throw ChunkMapException(
                                ERR_LOC, cName(__func__) + " out_of_range workerdId=" + workerId +
                                                 " db=" + dbName + " table=" + tableName +
                                                 " chunk=" + to_string(chunkInfo.chunk) + " " + exc.what());
                    }
                }
            }
        }
    }

    // This needs to be done for each CzarChunkMap in the family map.
    for (auto&& [familyName, chunkMapPtr] : *newFamilyMap) {
        LOGS(_log, LOG_LVL_DEBUG, cName(__func__) << " working on " << familyName);
        auto missing = chunkMapPtr->organize();
        if (missing != nullptr && !missing->empty()) {
            // TODO:UJ Some element of the dashboard should be made aware of this. Also,
            // TODO:UJ maybe this should check all families before throwing.
            // TODO:UJ There are implications that maybe the replicator should not
            // TODO:UJ tell the czar about families/databases that do not have
            // TODO:UJ at least one copy of each chunk with data loaded on a worker.
            string chunkIdStr;
            for (auto const& chunkData : *missing) {
                chunkIdStr += to_string(chunkData->getChunkId()) + " ";
            }
            throw ChunkMapException(
                    ERR_LOC, cName(__func__) + " family=" + familyName + " is missing chunks " + chunkIdStr);
        }
    }

    return newFamilyMap;
}

void CzarFamilyMap::insertIntoMaps(std::shared_ptr<FamilyMapType> const& newFamilyMap, string const& workerId,
                                   string const& dbName, string const& tableName, int64_t chunkIdNum,
                                   CzarChunkMap::SizeT sz) {
    // Get the CzarChunkMap for this family
    auto familyName = getFamilyNameFromDbName(dbName);
    LOGS(_log, LOG_LVL_TRACE,
         cName(__func__) << " familyInsrt{w=" << workerId << " fN=" << familyName << " dbN=" << dbName
                         << " tblN=" << tableName << " chunk=" << chunkIdNum << " sz=" << sz << "}");
    auto& nfMap = *newFamilyMap;
    CzarChunkMap::Ptr czarChunkMap;
    auto familyIter = nfMap.find(familyName);
    if (familyIter == nfMap.end()) {
        czarChunkMap = CzarChunkMap::Ptr(new CzarChunkMap());
        nfMap[familyName] = czarChunkMap;
    } else {
        czarChunkMap = familyIter->second;
    }

    auto [chunkMapPtr, wcMapPtr] = czarChunkMap->_getMaps();

    CzarChunkMap::WorkerChunkMap& wcMap = *wcMapPtr;
    CzarChunkMap::ChunkMap& chunkMap = *chunkMapPtr;

    // Get or make the worker entry
    CzarChunkMap::WorkerChunksData::Ptr workerChunksData;
    auto iterWC = wcMap.find(workerId);
    if (iterWC == wcMap.end()) {
        workerChunksData = CzarChunkMap::WorkerChunksData::Ptr(new CzarChunkMap::WorkerChunksData(workerId));
        wcMap[workerId] = workerChunksData;
    } else {
        workerChunksData = iterWC->second;
    }

    // Get or make the ChunkData entry in chunkMap
    CzarChunkMap::ChunkData::Ptr chunkData;
    auto iterChunkData = chunkMap.find(chunkIdNum);
    if (iterChunkData == chunkMap.end()) {
        chunkData = CzarChunkMap::ChunkData::Ptr(new CzarChunkMap::ChunkData(chunkIdNum));
        chunkMap[chunkIdNum] = chunkData;
    } else {
        chunkData = iterChunkData->second;
    }

    // Set or verify the table information
    auto iterDT = chunkData->_dbTableMap.find({dbName, tableName});
    if (iterDT == chunkData->_dbTableMap.end()) {
        // doesn't exist so set it up
        chunkData->_dbTableMap[{dbName, tableName}] = sz;
    } else {
        // Verify that it matches other data
        auto const& dbTbl = iterDT->first;
        auto tblSz = iterDT->second;
        auto const& dbN = dbTbl.first;
        auto const& tblN = dbTbl.second;
        if (dbName != dbN || tblN != tableName || tblSz != sz) {
            LOGS(_log, LOG_LVL_ERROR,
                 cName(__func__) << " data mismatch for " << dbName << "." << tableName << "=" << sz << " vs "
                                 << dbN << "." << tblN << "=" << tblSz);
        }
    }

    // Link WorkerData the single chunkData instance for the chunkId
    workerChunksData->_chunkDataMap[chunkIdNum] = chunkData;

    // Add worker to the list of workers containing the chunk.
    chunkData->addToWorkerHasThis(workerChunksData);
}

void CzarFamilyMap::verify(std::shared_ptr<FamilyMapType> const& familyMap) {
    for (auto&& [familyName, czarChunkMapPtr] : *familyMap) {
        czarChunkMapPtr->verify();
    }
}

}  // namespace lsst::qserv::czar
