/*
 * LSST Data Management System
 * Copyright 2015 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */

// Class header
#include "proxy/czarProxy.h"

// System headers
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <sys/types.h>
#include <unistd.h>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "czar/Czar.h"

#include "qdisp/Executive.h" // &&& TODO:UJ remove, only here for uberJobsEnabled def.


namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.proxy.czarProxy");

void initMDC() {
    LOG_MDC("LWP", std::to_string(lsst::log::lwpID()));
}

std::shared_ptr<lsst::qserv::czar::Czar> _czar;

// mutex is used for czar initialization only which could potentially
// happen simultaneously from several threads.
std::mutex _czarMutex;

}

namespace lsst {
namespace qserv {
namespace proxy {

void
initCzar(std::string const& czarName) {
    std::lock_guard<std::mutex> lock(_czarMutex);

    // ignore repeated calls (they are hard to filter on mysql-proxy side)
    if (czar::Czar::getCzar() != nullptr) {
        return;
    }

    // add some MDC data in every thread
    LOG_MDC_INIT(initMDC);

    // Find QSERV_CONFIG envvar value
    auto qConfig = std::getenv("QSERV_CONFIG");
    if (not qConfig or *qConfig == '\0') {
        throw std::runtime_error("QSERV_CONFIG is not defined");
    }

    // get czar name from parameter, QSERV_CZAR_NAME can override
    std::string name;
    auto qName = std::getenv("QSERV_CZAR_NAME");
    if (qName and *qName != '\0') {
        name = qName;
    } else {
        name = czarName;
        if (name.empty()) {
            // if still unknown generate something ~unique
            name = "czar." + std::to_string(getpid());
        }
    }

    _czar = czar::Czar::createCzar(qConfig, name);


    /// &&& TODO:UJ This is a hack to read in a file that has the basic worker ids
    ///             and the chunks on each worker from a file in the same directory as
    ///             qConfig, named workerchunkdata.txt
    if (uberJobsEnabled) { /// &&& delete this block once there's a reasonable way to get this information.
        std::string fpath = qConfig;
        auto pos = fpath.find_last_of("/");
        std::string fname = fpath.substr(0, pos);
        fname += "/workerchunkdata.txt";
        LOGS(_log, LOG_LVL_WARN, "&&& fpath=" << fpath << " fname=" << fname);
        _czar->setupWorkerResources(fname);
    }
}


czar::SubmitResult
submitQuery(std::string const& query, std::map<std::string, std::string> const& hints) {
    if (not ::_czar) {
        throw std::runtime_error("czarProxy/submitQuery(): czar instance not initialized");
    }
    return ::_czar->submitQuery(query, hints);
}

void
killQuery(std::string const& query, std::string const& clientId) {
    if (not ::_czar) {
        throw std::runtime_error("czarProxy/killQuery(): czar instance not initialized");
    }
    ::_czar->killQuery(query, clientId);
}

void log(std::string const& loggername, std::string const& level,
         std::string const& filename, std::string const& funcname,
         unsigned int lineno, std::string const& message) {
    auto logger = lsst::log::Log::getLogger(loggername);
    auto levelPtr = log4cxx::Level::toLevel(level);
    if (logger.isEnabledFor(levelPtr->toInt())) {
        logger.logMsg(levelPtr,
                      log4cxx::spi::LocationInfo(filename.c_str(), funcname.c_str(), lineno),
                      message);
    }
}


}}} // namespace lsst::qserv::proxy
