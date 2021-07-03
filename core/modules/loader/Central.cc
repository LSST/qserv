// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 AURA/LSST.
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
#include "Central.h"

// Third-party headers
#include <boost/asio.hpp>
#include <iostream>

// qserv headers
#include "loader/LoaderMsg.h"
#include "proto/loader.pb.h"
#include "proto/ProtoImporter.h"


// LSST headers
#include "lsst/log/Log.h"


namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.Central");
}


namespace lsst {
namespace qserv {
namespace loader {

void Central::_initialize() {
    // Order is important here.
    _pool = util::ThreadPool::newThreadPool(_threadPoolSize, _queue);

    doList = std::make_shared<DoList>(*this);

    std::thread t([this](){ _checkDoList(); });
    _checkDoListThread = std::move(t);
}

Central::~Central() {
    _loop = false;
    _pool->shutdownPool();
    for (std::thread& thd : _ioServiceThreads) {
        thd.join();
    }
}


void Central::run() {
    std::thread thd([this]() { ioService.run(); });
    _ioServiceThreads.push_back(std::move(thd));
}


void Central::_checkDoList() {
    while(_loop) {
        // Run and then sleep for a second. TODO A more advanced timer should be used
        doList->checkList();
        usleep(_loopSleepTime);
    }
}


std::ostream& operator<<(std::ostream& os, ChunkSubchunk const& csc) {
    os << "chunk=" << csc.chunk << " subchunk=" << csc.subchunk;
    return os;
}


}}} // namespace lsst::qserv::loader
