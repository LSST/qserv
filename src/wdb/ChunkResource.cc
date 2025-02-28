// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
 * @brief ChunkResource implementation
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "wdb/ChunkResource.h"

// System headers
#include <cstddef>
#include <mutex>

// Third-party headers
#include "boost/format.hpp"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/constants.h"
#include "sql/SqlResults.h"
#include "util/Bug.h"
#include "util/IterableFormatter.h"
#include "wbase/Base.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.wdb.ChunkResource");

}  // anonymous namespace

namespace lsst::qserv::wdb {
////////////////////////////////////////////////////////////////////////
// ChunkResource
////////////////////////////////////////////////////////////////////////
class ChunkResource::Info {
public:
    Info(std::string const& db_, int chunkId_, DbTableSet const& tables_, IntVector const& subChunkIds_)
            : db{db_}, chunkId{chunkId_}, tables{tables_}, subChunkIds{subChunkIds_} {}

    Info(std::string const& db_, int chunkId_, DbTableSet const& tables_)
            : db{db_}, chunkId{chunkId_}, tables{tables_} {}

    std::string db;
    int chunkId;
    DbTableSet tables;
    IntVector subChunkIds;
};
std::ostream& operator<<(std::ostream& os, ChunkResource::Info const& i) {
    os << "CrInfo(" << i.chunkId << "; ";
    std::copy(i.subChunkIds.begin(), i.subChunkIds.end(), std::ostream_iterator<int>(os, ","));
    os << ")";
    return os;
}
////////////////////////////////////////////////////////////////////////
// ChunkResource
////////////////////////////////////////////////////////////////////////
ChunkResource::ChunkResource(ChunkResourceMgr* mgr) : _mgr{mgr} {}

ChunkResource::ChunkResource(ChunkResourceMgr* mgr, ChunkResource::Info* info) : _mgr{mgr}, _info{info} {
    LOGS(_log, LOG_LVL_TRACE, "ChunkResource info=" << *info);
    _mgr->acquireUnit(*_info);
}
ChunkResource::ChunkResource(ChunkResource const& cr) : _mgr{cr._mgr}, _info{new Info(*cr._info)} {
    _mgr->acquireUnit(*_info);
}

ChunkResource& ChunkResource::operator=(ChunkResource const& cr) {
    _mgr = cr._mgr;
    _info.reset(new Info(*cr._info));
    _mgr->acquireUnit(*_info);
    return *this;
}

ChunkResource::~ChunkResource() {
    if (_info.get()) {
        _mgr->release(*_info);
    }
}

std::string const& ChunkResource::getDb() const { return _info->db; }

int ChunkResource::getChunkId() const { return _info->chunkId; }

DbTableSet const& ChunkResource::getTables() const { return _info->tables; }

IntVector const& ChunkResource::getSubChunkIds() const { return _info->subChunkIds; }

std::ostream& operator<<(std::ostream& os, const SQLBackend::LockStatus& ls) {
    switch (ls) {
        case SQLBackend::UNLOCKED:
            os << "UNLOCKED";
            break;
        case SQLBackend::LOCKED_OTHER:
            os << "LOCKED_OTHER";
            break;
        case SQLBackend::LOCKED_OURS:
            os << "LOCKED_OURS";
            break;
    }
    os << "unknown";
    return os;
}

/// ChunkEntry is an entry that represents table subchunks for a given
/// database and chunkid.
class ChunkEntry {
public:
    typedef std::map<int, int> SubChunkMap;           // subchunkid -> count
    typedef std::map<DbTable, SubChunkMap> TableMap;  // tablename -> subchunk map

    typedef std::shared_ptr<ChunkEntry> Ptr;

    ChunkEntry(int chunkId) : _chunkId(chunkId), _refCount(0) {}

    int getRefCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _refCount;
    }

    /// @return a copy of _tableMap
    TableMap getTableMapCopy() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _tableMap;
    }

    /// Acquire a resource, loading if needed
    void acquire(std::string const& db, DbTableSet const& dbTableSet, IntVector const& sc,
                 SQLBackend::Ptr backend) {
        ScTableVector needed;
        std::lock_guard<std::mutex> lock(_mutex);
        backend->memLockRequireOwnership();
        ++_refCount;  // Increase usage count
        LOGS(_log, LOG_LVL_TRACE,
             "Subchunk acquire refC=" << _refCount << " db=" << db << " tables["
                                      << util::printable(dbTableSet) << "]"
                                      << " sc[" << util::printable(sc) << "]");
        for (auto const& dbTbl : dbTableSet) {
            SubChunkMap& scm = _tableMap[dbTbl];  // implicit creation OK.
            IntVector::const_iterator i, e;
            for (i = sc.begin(), e = sc.end(); i != e; ++i) {
                SubChunkMap::iterator it = scm.find(*i);
                int last = 0;
                if (it == scm.end()) {
                    needed.push_back(ScTable(_chunkId, dbTbl, *i));
                } else {
                    last = it->second;
                }
                scm[*i] = last + 1;  // write new value
            }  // All subchunks
        }  // All tables
        // For now, every other user of this chunk must wait while
        // we fetch the resource.
        if (needed.size() > 0) {
            sql::SqlErrorObject err;
            bool loadOk = backend->load(needed, err);
            if (!loadOk) {
                // Release
                _release(needed);
                throw err;
            }
        }
    }

    /// Release a resource, flushing if no more users need it.
    void release(std::string const& db, DbTableSet const& dbTableSet, IntVector const& sc,
                 SQLBackend::Ptr backend) {
        std::lock_guard<std::mutex> lock(_mutex);
        backend->memLockRequireOwnership();
        StringVector::const_iterator ti, te;
        LOGS(_log, LOG_LVL_TRACE,
             "SubChunk release refC=" << _refCount << " db=" << db << " dbTableSet["
                                      << util::printable(dbTableSet) << "]"
                                      << " sc[" << util::printable(sc) << "]");
        for (auto const& dbTbl : dbTableSet) {
            SubChunkMap& scm = _tableMap[dbTbl];  // Should be in there.
            IntVector::const_iterator i, e;
            for (i = sc.begin(), e = sc.end(); i != e; ++i) {
                SubChunkMap::iterator it = scm.find(*i);  // Should be there
                if (it == scm.end()) {
                    throw util::Bug(
                            ERR_LOC,
                            "ChunkResource ChunkEntry::release: Error releasing un-acquired resource");
                }
                scm[*i] = it->second - 1;  // write new value
            }  // All subchunks
        }  // All tables
        --_refCount;
        _flush(db, backend);  // Discard resources no longer needed by anyone.
        // flush could be detached from the release function, to be called at a
        // high-water mark and/or on periodic intervals
    }

private:
    /// Flush resources no longer needed by anybody
    void _flush(std::string const& db, SQLBackend::Ptr backend) {
        ScTableVector discardable;
        for (auto& elem : _tableMap) {
            IntVector mapDiscardable;
            SubChunkMap& scm = elem.second;
            SubChunkMap::iterator si, se;
            for (si = scm.begin(), se = scm.end(); si != se; ++si) {
                if (si->second == 0) {
                    discardable.push_back(ScTable(_chunkId, elem.first, si->first));
                    mapDiscardable.push_back(si->first);
                } else if (si->second < 0) {
                    throw util::Bug(ERR_LOC,
                                    "ChunkResource ChunkEntry::flush: Invalid negative use count when "
                                    "flushing subchunks");
                }
            }  // All subchunks
            // Prune zero elements for this db+table+chunk
            // (invalidates iterators)
            IntVector::iterator di, de;
            for (di = mapDiscardable.begin(), de = mapDiscardable.end(); di != de; ++di) {
                scm.erase(*di);
            }
        }  // All tables
        // Delegate actual table dropping to the backend.
        if (discardable.size() > 0) {
            backend->discard(discardable);
        }
    }

    void _release(ScTableVector const& needed) {
        // _mutex should be held.
        // Release subChunkId for the right table
        for (auto const& elem : needed) {
            SubChunkMap& scm = _tableMap[elem.dbTable];
            --scm[elem.subChunkId];
        }
    }

    std::shared_ptr<SQLBackend> _backend;  ///< Delegate stage/unstage
    int _chunkId;
    int _refCount;       ///< Number of known users
    TableMap _tableMap;  ///< tables in use
    mutable std::mutex _mutex;
};

////////////////////////////////////////////////////////////////////////
// ChunkResourceMgr
////////////////////////////////////////////////////////////////////////

ChunkResourceMgr::Ptr ChunkResourceMgr::newMgr(SQLBackend::Ptr const& backend) {
    // return std::shared_ptr<ChunkResourceMgr>(new Impl(backend));
    return std::make_shared<ChunkResourceMgr>(backend);
}

ChunkResource ChunkResourceMgr::acquire(std::string const& db, int chunkId, DbTableSet const& tables) {
    // Make sure that the chunk is ready. (NOP right now.)
    LOGS(_log, LOG_LVL_TRACE,
         "acquire db=" << db << " chunkId=" << chunkId << " tables=" << util::printable(tables));
    ChunkResource cr(this, new ChunkResource::Info(db, chunkId, tables));
    return cr;
}

ChunkResource ChunkResourceMgr::acquire(std::string const& db, int chunkId, DbTableSet const& dbTableSet,
                                        IntVector const& subChunks) {
    ChunkResource cr(this, new ChunkResource::Info(db, chunkId, dbTableSet, subChunks));
    return cr;
}

void ChunkResourceMgr::release(ChunkResource::Info const& i) {
    std::lock_guard<std::mutex> lock(_mapMutex);
    Map& map = _getMap(i.db);
    ChunkEntry& ce = _getChunkEntry(map, i.chunkId);
    ce.release(i.db, i.tables, i.subChunkIds, _backend);
}

void ChunkResourceMgr::acquireUnit(ChunkResource::Info const& i) {
    std::lock_guard<std::mutex> lock(_mapMutex);
    Map& map = _getMap(i.db);  // Select db
    ChunkEntry& ce = _getChunkEntry(map, i.chunkId);
    // Actually acquire
    LOGS(_log, LOG_LVL_TRACE, "acquireUnit info=" << i);
    ce.acquire(i.db, i.tables, i.subChunkIds, _backend);
}

int ChunkResourceMgr::getRefCount(std::string const& db, int chunkId) {
    std::lock_guard<std::mutex> lock(_mapMutex);
    Map& map = _getMap(db);                // Select db
    Map::iterator it = map.find(chunkId);  // Select chunkId
    if (it == map.end()) {
        return 0;
    }
    ChunkEntry& ce = *(it->second.get());
    return ce.getRefCount();
}

ChunkResourceMgr::Map& ChunkResourceMgr::_getMap(std::string const& db) {
    DbMap::iterator it = _dbMap.find(db);
    if (it == _dbMap.end()) {
        DbMap::value_type v(db, Map());
        _dbMap.insert(v);
        it = _dbMap.find(db);
    }
    return it->second;
}

ChunkEntry& ChunkResourceMgr::_getChunkEntry(Map& m, int chunkId) {
    Map::iterator it = m.find(chunkId);  // Select chunkId
    if (it == m.end()) {                 // Insert if not exist
        Map::value_type v(chunkId, std::make_shared<ChunkEntry>(chunkId));
        m.insert(v);
        return *(v.second.get());
    }
    return *(it->second.get());
}

}  // namespace lsst::qserv::wdb
