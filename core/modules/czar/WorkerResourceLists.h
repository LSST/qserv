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
#ifndef LSST_QSERV_CZAR_WORKERRESOURCES_H
#define LSST_QSERV_CZAR_WORKERRESOURCES_H

// System headers
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>

// Qserv headers
#include "global/ResourceUnit.h"


// This header declarations
namespace lsst {
namespace qserv {
namespace czar {


/// Store the chunk id numbers for a specific database.
class DbResource {
public:
    using Ptr = std::shared_ptr<DbResource>;

    static std::string getDbNameFromResource(std::string const& chunkResource);

    DbResource(std::string const& dbName) : _dbName(dbName) {};
    DbResource() = delete;
    DbResource(DbResource const&) = delete;
    DbResource& operator=(DbResource const&) = delete;

    ~DbResource() = default;

    /// @return true if it was inserted.
    bool insert(int chunkId);

    /// @return a deque (a copy) with all the elements in _chunkSet, in order.
    std::deque<int> getDeque();

    size_t getSize() const {
        std::lock_guard<std::mutex> lg(_mtx);
        return _getSize();
    }

    virtual std::ostream& dumpOS(std::ostream &os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream& os, DbResource const& dbr);

private:
    size_t _getSize() const { return _chunkSet.size(); }

    std::string const _dbName;  // name of the database
    std::set<int> _chunkSet;
    mutable std::mutex _mtx;
};


/// This class constructs deques of integers relating to the resources found on
/// the workers. It uses the information in the chunk resource
///   Format: "/chk/<dbname>/<chunk id number>"
/// to create lists of integers in relating to the appropriate databases.
/// It is important to keep the chunk id numbers in numerical order so
/// that the constructed UberJobs will complete as quickly as possible
/// and free up system resources.
class WorkerResource {
public:
    using Ptr = std::shared_ptr<WorkerResource>;
    WorkerResource(std::string const& name) : _resourceName(name) {}
    WorkerResource(WorkerResource const&) = delete;
    WorkerResource& operator=(WorkerResource const&) = delete;

    virtual ~WorkerResource() = default;

    /// The 'dbChunkResourceName' contains the db name and chunk id number.
    /// insert() uses both of these to create all needed entries.
    /// UberJobs do not use chunk resource names after this point.
    bool insert(std::string const& dbChunkResourceName);

    std::deque<int> getDequeFor(std::string const& dbName);

    virtual std::ostream& dumpOS(std::ostream &os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream& os, WorkerResource const& wr);

private:
    std::string _resourceName;
    std::map<std::string, DbResource::Ptr> _dbResources; /// Map of resource chunks on the worker (key is dbName).
    mutable std::mutex _dbMtx;
};


class WorkerResourceLists {
public:

    WorkerResourceLists() = default;
    WorkerResourceLists(WorkerResourceLists const&) = delete;
    WorkerResourceLists& operator=(WorkerResourceLists const&) = delete;

    virtual ~WorkerResourceLists() = default;

    std::pair<WorkerResource::Ptr, bool> insertWorker(std::string const& wResourceName) {
        std::lock_guard<std::mutex> lg(_workerMapMtx);
        return _insertWorker(wResourceName);
    }

    std::map<std::string, std::deque<int>> getDequesFor(std::string const& dbName);

    /// mono-node test functions
    void setMonoNodeTest();
    std::deque<std::string> fillChunkIdSet();

    /// Read in the worker resources from a text file with name 'fName'.
    /// The file contains entries like "db06 2453"
    /// TODO:UJ &&& should the dummy chunk be added to all found workers ???
    bool readIn(std::string const& fName);

    virtual std::ostream& dumpOS(std::ostream &os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream& os, WorkerResourceLists const& wr);

private:
    /// Insert a new worker into the map. Must lock _workerMapMtx before calling
    /// @return pair<WorkerResource::Ptr, bool - true if new element inserted.>
    std::pair<WorkerResource::Ptr, bool> _insertWorker(std::string const& wResourceName) {
        WorkerResource::Ptr  newWr = std::make_shared<WorkerResource>(wResourceName);
        auto result = _workers.emplace(wResourceName, newWr);
        auto iter = result.first;
        return std::pair<WorkerResource::Ptr, bool>(iter->second, result.second);
    }

    std::map<std::string, WorkerResource::Ptr> _workers;
    mutable std::mutex _workerMapMtx;
};



}}} // namespace lsst::qserv::czar
#endif // LSST_QSERV_CZAR_WORKERRESOURCES_H
