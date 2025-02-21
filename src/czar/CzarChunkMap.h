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

#ifndef LSST_QSERV_CZAR_CZARCHUNKMAP_H
#define LSST_QSERV_CZAR_CZARCHUNKMAP_H

// System headers
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>

// Qserv headers
#include "global/clock_defs.h"
#include "util/Issue.h"

namespace lsst::qserv::qmeta {
class QMeta;
struct QMetaChunkMap;
}  // namespace lsst::qserv::qmeta

namespace lsst::qserv::czar {

class ActiveWorker;
class CzarFamilyMap;

class ChunkMapException : public util::Issue {
public:
    ChunkMapException(Context const& ctx, std::string const& msg) : util::Issue(ctx, msg) {}
};

/// The czar is expected to heavily use the
///    `getMaps() -> WorkerChunkMap -> getSharedScanChunkMap()`
/// to send jobs to workers, as that gets an ordered list of all chunks
/// the worker should handle during a shared scan.
///    `getMaps() -> ChunkMap` is expected to be more useful if there is a
///  failure and a chunk query needs to go to a different worker.
///
/// Workers failing or new workers being added is expected to be a rare event.
/// The current algorithm to split chunks between the workers tries to split
/// the work evenly. However, if a new worker is added, it's likely that
/// the new distribution of chunks for shared scans will put the chunks on
/// different workers than previously, which in turn will result in the system
/// being less efficient until all the old scans are complete. If workers
/// being added or removed from the system becomes frequent, the algorithm should
/// probably change to try to maintain some chunk location consistency once
/// the system is up.
class CzarChunkMap {
public:
    using Ptr = std::shared_ptr<CzarChunkMap>;
    using SizeT = uint64_t;

    std::string cName(const char* func) { return std::string("CzarChunkMap::") + func; }

    CzarChunkMap(CzarChunkMap const&) = delete;
    CzarChunkMap& operator=(CzarChunkMap const&) = delete;

    static Ptr create() { return Ptr(new CzarChunkMap()); }

    ~CzarChunkMap();

    class WorkerChunksData;

    /// Essentially a structure for storing data about which tables and workers are associated with this
    /// chunk.
    class ChunkData {
    public:
        using Ptr = std::shared_ptr<ChunkData>;
        ChunkData(int chunkId_) : _chunkId(chunkId_) {}

        std::string cName(const char* func) {
            return std::string("ChunkData::") + func + " " + std::to_string(_chunkId);
        }
        int64_t getChunkId() const { return _chunkId; }
        SizeT getTotalBytes() const { return _totalBytes; }

        std::weak_ptr<WorkerChunksData> getPrimaryScanWorker() const { return _primaryScanWorker; }

        /// Add `worker` to the `_workerHasThisMap` to indicate that worker has a copy
        /// of this chunk.
        void addToWorkerHasThis(std::shared_ptr<WorkerChunksData> const& worker);

        /// Return a copy of _workerHasThisMap.
        std::map<std::string, std::weak_ptr<WorkerChunksData>> getWorkerHasThisMapCopy() const;

        std::string dump() const;

        friend CzarChunkMap;
        friend CzarFamilyMap;

    private:
        int64_t const _chunkId;  ///< The Id number for this chunk.
        SizeT _totalBytes = 0;   ///< The total number of bytes used by all tables in this chunk.
        std::weak_ptr<WorkerChunksData> _primaryScanWorker;  ///< The worker to be used to shared scans.

        /// Key is databaseName+tableName, value is size in bytes.
        std::map<std::pair<std::string, std::string>, SizeT> _dbTableMap;

        /// Map of workers that have this chunk
        std::map<std::string, std::weak_ptr<WorkerChunksData>> _workerHasThisMap;

        /// Add up the bytes in each table for this chunk to get `_totalBytes`
        void _calcTotalBytes();
    };

    /// Essentially a structure for storing which chunks are associated with a worker.
    class WorkerChunksData {
    public:
        using Ptr = std::shared_ptr<WorkerChunksData>;
        WorkerChunksData(std::string const& workerId) : _workerId(workerId) {}

        std::string cName(const char* func) {
            return std::string("WorkerChunksData::") + func + " " + _workerId;
        }

        /// Return the worker's id string.
        std::string const& getWorkerId() const { return _workerId; }

        /// Return the number of bytes contained in all chunks/tables to be
        /// accessed in a full table scan on this worker.
        SizeT getSharedScanTotalSize() const { return _sharedScanTotalSize; }

        /// Return true if this worker is dead, according to `ActiveWorkerMap`.
        bool isDead();

        /// Return a reference to `_sharedScanChunkMap`. A copy of the pointer
        /// to this class (or the containing map) should be held to ensure the reference.
        std::map<int, ChunkData::Ptr> const& getSharedScanChunkMap() const { return _sharedScanChunkMap; }

        std::string dump() const;

        friend CzarChunkMap;
        friend CzarFamilyMap;

    private:
        std::string const _workerId;

        /// Map of all chunks found on the worker where key is chunkId
        std::map<int, ChunkData::Ptr> _chunkDataMap;

        /// Map of chunks this worker will handle during shared scans.
        /// Since scans are done in order of chunk id numbers, it helps
        /// to have this in chunk id number order.
        /// At some point, this should be sent to workers so they
        /// can make more accurate time estimates for chunk completion.
        std::map<int, ChunkData::Ptr> _sharedScanChunkMap;

        /// The total size (in bytes) of all chunks on this worker that
        /// are to be used in shared scans.
        SizeT _sharedScanTotalSize = 0;

        /// Used to determine if this worker is alive and set
        /// when the test is made.
        std::shared_ptr<ActiveWorker> _activeWorker;
    };

    using WorkerChunkMap = std::map<std::string, WorkerChunksData::Ptr>;
    using ChunkMap = std::map<int, ChunkData::Ptr>;
    using ChunkVector = std::vector<ChunkData::Ptr>;

    /// Sort the chunks in `chunksSortedBySize` in descending order by total size in bytes.
    static void sortChunks(ChunkVector& chunksSortedBySize);

    /// Calculate the total bytes in each chunk and then sort the resulting ChunkVector by chunk size,
    /// descending.
    static void calcChunkMap(ChunkMap const& chunkMap, ChunkVector& chunksSortedBySize);

    /// Verify that all chunks belong to at least one worker and that all chunks are represented in shared
    /// scans.
    /// @throws ChunkMapException
    void verify();

    static std::string dumpChunkMap(ChunkMap const& chunkMap);

    static std::string dumpWorkerChunkMap(WorkerChunkMap const& wcMap);

    /// Return shared pointers to `_chunkMap` and `_workerChunkMap`, which should be held until
    /// finished with the data.
    std::pair<std::shared_ptr<CzarChunkMap::ChunkMap const>,
              std::shared_ptr<CzarChunkMap::WorkerChunkMap const>>
    getMaps() const {
        std::lock_guard<std::mutex> lck(_mapMtx);
        return {_chunkMap, _workerChunkMap};
    }

    /// Use the information from the registry to `organize` `_chunkMap` and `_workerChunkMap`
    /// into their expected formats, which also should define where a chunk is always
    /// run during shared scans.
    /// This is a critical function for defining which workers will handle which jobs.
    /// @return a vector of ChunkData::Ptr of chunks where no worker was found.
    std::shared_ptr<CzarChunkMap::ChunkVector> organize();

private:
    CzarChunkMap();

    /// Return shared pointers to `_chunkMap` and `_workerChunkMap`, which should be held until
    /// finished with the data.
    std::pair<std::shared_ptr<CzarChunkMap::ChunkMap>, std::shared_ptr<CzarChunkMap::WorkerChunkMap>>
    _getMaps() const {
        std::lock_guard<std::mutex> lck(_mapMtx);
        return {_chunkMap, _workerChunkMap};
    }

    /// Map of all workers and which chunks they contain.
    std::shared_ptr<WorkerChunkMap> _workerChunkMap{new WorkerChunkMap()};

    /// Map of all chunks in the system with chunkId number as the key and the values contain
    /// information about the tables in those chunks and which worker is responsible for
    /// handling the chunk in a shared scan.
    std::shared_ptr<ChunkMap> _chunkMap{new ChunkMap()};

    mutable std::mutex _mapMtx;  ///< protects _workerChunkMap, _chunkMap (TODO:UJ may not be needed anymore)

    friend CzarFamilyMap;
};

/// This class is used to organize worker chunk table information so that it
/// can be used to send jobs to the appropriate worker and inform workers
/// what chunks they can expect to handle in shared scans, focusing at the
/// family level.
/// The data for the maps is provided by the Replicator and stored in the
/// QMeta database.
/// When the data is changed, there is a timestamp that is updated, which
/// will cause new maps to be made by this class.
///
/// The maps generated should be treated as constant objects stored with
/// shared pointers. As such, it should be possible for numerous threads
/// to use each map simultaneously provided they have their own pointers
/// to the maps.
/// The pointers to the maps are mutex protected to safely allow map updates.
//
// TODO:UJ move this to its own header file.
//
// TODO:UJ Currently, each family only has one database and they share a name.
//   Once a table mapping databases to families is available, it needs to be
//   used to map databases to families in this class.
class CzarFamilyMap {
public:
    using Ptr = std::shared_ptr<CzarFamilyMap>;
    typedef std::map<std::string, CzarChunkMap::Ptr> FamilyMapType;
    typedef std::map<std::string, std::string> DbNameToFamilyNameType;

    static Ptr create(std::shared_ptr<qmeta::QMeta> const& qmeta);

    CzarFamilyMap() = delete;
    CzarFamilyMap(CzarFamilyMap const&) = delete;
    CzarFamilyMap& operator=(CzarFamilyMap const&) = delete;

    ~CzarFamilyMap() = default;

    /// For unit testing only
    /// @param dbNameToFamilyNameType - valid map of db to family name for the unit test.
    // TODO::UJ define member instance for `_dbNameToFamilyName`
    CzarFamilyMap(std::shared_ptr<DbNameToFamilyNameType> const& dbNameToFamilyName) {}

    std::string cName(const char* fName) const {
        return std::string("CzarFamilyMap::") + ((fName == nullptr) ? "?" : fName);
    }

    /// Family names are unknown until a table has been added to the database, so
    /// the dbName will be used as the family name until the table exists.
    std::string getFamilyNameFromDbName(std::string const& dbName) const {
        // TODO:UJ use a member instance of std::shared_ptr<DbNameToFamilyNameType>
        //     once info is available in QMeta.
        return dbName;
    }

    /// Return the chunk map for the database `dbName`
    CzarChunkMap::Ptr getChunkMap(std::string const& dbName) const {
        auto familyName = getFamilyNameFromDbName(dbName);
        return _getChunkMap(familyName);
    }

    /// Read the registry information from the database, if not already set.
    bool read();

    /// Make a new FamilyMapType map including ChunkMap and WorkerChunkMap from the data
    /// in `qChunkMap`. Each family has its own ChunkMap and WorkerChunkMap.
    /// @param qChunkMap - data source for the family map
    /// @param usingChunkSize - true if the distribution of chunks will depend on the
    ///                  size of the chunks/
    ///
    /// NOTE: This is likely an expensive operation and should probably only
    ///   be called if new workers have been added or chunks have been moved.
    std::shared_ptr<FamilyMapType> makeNewMaps(qmeta::QMetaChunkMap const& qChunkMap, bool usingChunkSize);

    /// Insert the new element described by the parameters into the `newFamilyMap` as appropriate.
    void insertIntoMaps(std::shared_ptr<FamilyMapType> const& newFamilyMap, std::string const& workerId,
                        std::string const& dbName, std::string const& tableName, int64_t chunkIdNum,
                        CzarChunkMap::SizeT sz);

    /// Verify the `familyMap` does not have errors.
    static void verify(std::shared_ptr<FamilyMapType> const& familyMap);

private:
    /// Try to `_read` values for maps from `qmeta`.
    CzarFamilyMap(std::shared_ptr<qmeta::QMeta> const& qmeta);

    /// Read the registry information from the database, stopping if
    /// it hasn't been updated.
    // TODO:UJ add a changed timestamp (similar to the existing updated timestamp)
    //    to the registry database and only update when changed.
    bool _read();

    /// Return the chunk map for the `familyName`
    CzarChunkMap::Ptr _getChunkMap(std::string const& familyName) const {
        std::lock_guard<std::mutex> familyLock(_familyMapMtx);
        auto iter = _familyMap->find(familyName);
        if (iter == _familyMap->end()) {
            return nullptr;
        }
        return iter->second;
    }

    std::shared_ptr<qmeta::QMeta> _qmeta;  ///< Database connection to collect json worker list.

    /// The last time the maps were updated with information from the replicator.
    TIMEPOINT _lastUpdateTime;  // initialized to 0;

    std::shared_ptr<FamilyMapType const> _familyMap{new FamilyMapType()};
    mutable std::mutex _familyMapMtx;  ///< protects _familyMap, _timeStamp, and _qmeta.
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_CZARCHUNKMAP_H
