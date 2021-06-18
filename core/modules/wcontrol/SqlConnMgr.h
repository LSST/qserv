// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2020 LSST Corporation.
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

#ifndef LSST_QSERV_WCONTROL_SQLCONNMGR_H
#define LSST_QSERV_WCONTROL_SQLCONNMGR_H

// System headers
#include <assert.h>
#include <atomic>
#include <condition_variable>
#include <mutex>

// Qserv headers
#include "util/InstanceCount.h" //&&&


namespace lsst {
namespace qserv {
namespace wcontrol {

class SqlConnLock;

/// A way to limit the number of simultaneous MySQL connections related to
/// user queries and the worker scheduler. The total number of
/// maxSqlConnections should be significantly lower than MySQL max_connections
/// since other things may need to make connections to MySQL and running out
/// of connections is extremely painful for qserv.
/// The number of connections for shared scan connections
/// (maxScanSqlConnections)should be lower than the total (maxSqlConnections).
/// This allows interactive queries to go through even when shared scans
/// have the system heavily loaded.
///
class SqlConnMgr {
public:
    using Ptr = std::shared_ptr<SqlConnMgr>;
    SqlConnMgr(int maxSqlConnections, int maxScanSqlConnections)
        : _maxSqlConnections(maxSqlConnections), _maxScanSqlConnections(maxScanSqlConnections) {
        assert( _maxSqlConnections > 1);
        assert( _maxScanSqlConnections > 1);
        assert( _maxSqlConnections >= _maxScanSqlConnections);
    }
    SqlConnMgr() = delete;
    SqlConnMgr(SqlConnMgr const&) = delete;
    SqlConnMgr& operator=(SqlConnMgr const&) = delete;
    virtual ~SqlConnMgr() = default;

    int getTotalCount() { return _totalCount; }
    int getSqlConnCount() { return _sqlConnCount; }

    virtual std::ostream& dump(std::ostream &os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream &out, SqlConnMgr const& mgr);

    friend class SqlConnLock;

private:
    void _take(bool scanQuery);
    void _release();

    std::atomic<int> _totalCount{0};
    std::atomic<int> _sqlConnCount{0};
    int _maxSqlConnections;
    int _maxScanSqlConnections;
    std::mutex _mtx;
    std::condition_variable _tCv;
};


/// RAII class to support SqlConnMgr
class SqlConnLock {
public:
    SqlConnLock(SqlConnMgr& sqlConnMgr, bool scanQuery)
      : _sqlConnMgr(sqlConnMgr) {
        _sqlConnMgr._take(scanQuery);
    }
    SqlConnLock() = delete;
    SqlConnLock(SqlConnLock const&) = delete;
    SqlConnLock& operator=(SqlConnLock const&) = delete;

    ~SqlConnLock() { _sqlConnMgr._release(); }

private:
    SqlConnMgr& _sqlConnMgr;

    util::InstanceCount _ic{"SqlConnLock&&&"};
};


}}} // namespace lsst::qserv::wcontrol

#endif // LSST_QSERV_WCONTROL_TRANSMITMGR_H
