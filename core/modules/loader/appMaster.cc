// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2019 AURA/LSST.
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


// System headers
#include <iostream>

// qserv headers
#include "loader/CentralMaster.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.appMaster");
}

using namespace lsst::qserv::loader;
using  boost::asio::ip::udp;


int main(int argc, char* argv[]) {
    std::string mCfgFile("core/modules/loader/config/master.cnf");
    if (argc > 1) {
        mCfgFile = argv[1];
    }
    LOGS(_log, LOG_LVL_INFO, "masterCfg=" << mCfgFile);

    std::string const ourHost = boost::asio::ip::host_name();
    boost::asio::io_service ioService;

    MasterConfig mCfg(mCfgFile);
    CentralMaster cMaster(ioService, ourHost, mCfg);
    try {
        cMaster.start();
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "cMaster.start() failed e=" << e.what());
        return 1;
    }
    cMaster.runServer();

    bool loop = true;
    while(loop) {
        sleep(10);
    }
    ioService.stop();
    LOGS(_log, LOG_LVL_INFO, "master DONE");
}

