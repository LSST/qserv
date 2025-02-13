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
#include "replica/util/Performance.h"

// Qserv headers
#include "replica/proto/protocol.pb.h"
#include "util/TimeUtils.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using json = nlohmann::json;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.Performance");

}  // namespace

namespace lsst::qserv::replica {

Performance::Performance()
        : c_create_time(util::TimeUtils::now()),
          c_start_time(0),
          w_receive_time(0),
          w_start_time(0),
          w_finish_time(0),
          c_finish_time(0) {}

void Performance::update(ProtocolPerformance const& workerPerformanceInfo) {
    w_receive_time = workerPerformanceInfo.receive_time();
    w_start_time = workerPerformanceInfo.start_time();
    w_finish_time = workerPerformanceInfo.finish_time();
}

uint64_t Performance::setUpdateStart() {
    uint64_t const t = c_start_time;
    c_start_time = util::TimeUtils::now();
    return t;
}

uint64_t Performance::setUpdateFinish() {
    uint64_t const t = c_finish_time;
    c_finish_time = util::TimeUtils::now();
    return t;
}

ostream& operator<<(ostream& os, Performance const& p) {
    os << "Performance "
       << " c.create:" << p.c_create_time << " c.start:" << p.c_start_time
       << " w.receive:" << p.w_receive_time << " w.start:" << p.w_start_time
       << " w.finish:" << p.w_finish_time << " c.finish:" << p.c_finish_time
       << " length.sec:" << (p.c_finish_time ? (p.c_finish_time - p.c_start_time) / 1000. : '*');
    return os;
}

WorkerPerformance::WorkerPerformance()
        : receive_time(util::TimeUtils::now()), start_time(0), finish_time(0) {}

uint64_t WorkerPerformance::setUpdateStart() { return start_time.exchange(util::TimeUtils::now()); }

uint64_t WorkerPerformance::setUpdateFinish() { return finish_time.exchange(util::TimeUtils::now()); }

unique_ptr<ProtocolPerformance> WorkerPerformance::info() const {
    auto ptr = make_unique<ProtocolPerformance>();
    ptr->set_receive_time(receive_time);
    ptr->set_start_time(start_time);
    ptr->set_finish_time(finish_time);
    return ptr;
}

json WorkerPerformance::toJson() const {
    return json::object({{"receive_time", receive_time.load()},
                         {"start_time", start_time.load()},
                         {"finish_time", finish_time.load()}});
}

ostream& operator<<(ostream& os, WorkerPerformance const& p) {
    os << "WorkerPerformance "
       << " receive:" << p.receive_time << " start:" << p.start_time << " finish:" << p.finish_time
       << " length.sec:" << (p.finish_time ? (p.finish_time - p.receive_time) / 1000. : '*');
    return os;
}

}  // namespace lsst::qserv::replica
