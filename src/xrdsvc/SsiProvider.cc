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
/// Implement XrdSsiProviderServer to provide Qserv's SsiService
/// implementation. Link this file when building a plugin to be used as
/// ssi.svclib or oss.statlib.

// Class header
#include "xrdsvc/SsiProvider.h"

// System headers
#include <sstream>
#include <sys/types.h>

// Third party headers
#include "boost/filesystem.hpp"
#include "XrdSsi/XrdSsiCluster.hh"
#include "XrdSsi/XrdSsiLogger.hh"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/ResourceUnit.h"
#include "wconfig/WorkerConfig.h"
#include "wpublish/ChunkInventory.h"
#include "xrdsvc/XrdName.h"

/******************************************************************************/
/*                               G l o b a l s                                */
/******************************************************************************/

// The following are global sysbols that point to an instance of our provider
// object. The SSI framework looks for these symbols when the shared library
// plug-in is loaded. The framework must find a valid provider object at load
// time or it will refuse to use the shared library. As the library is never
// unloaded, the object does not need to be deleted.
//
XrdSsiProvider* XrdSsiProviderServer = new lsst::qserv::xrdsvc::SsiProviderServer;

XrdSsiProvider* XrdSsiProviderLookup = XrdSsiProviderServer;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.SsiProvider");
}

/******************************************************************************/
/*                            D e s t r u c t o r                             */
/******************************************************************************/

namespace lsst::qserv::xrdsvc {

SsiProviderServer::~SsiProviderServer() {}

/******************************************************************************/
/*                                  I n i t                                   */
/******************************************************************************/

bool SsiProviderServer::Init(XrdSsiLogger* logP, XrdSsiCluster* clsP, std::string cfgFn, std::string parms,
                             int argc, char** argv) {
    lsst::qserv::xrdsvc::XrdName x;

    if (argc != 2) {
        LOGS(_log, LOG_LVL_TRACE, "argc: " << argc);
        LOGS(_log, LOG_LVL_FATAL,
             "Incorrect xrdssi configuration, launch "
             "xrootd with option '-+xrdssi /path/to/xrdssi/cfg/file'");
        exit(EXIT_FAILURE);
    }

    LOGS(_log, LOG_LVL_DEBUG, "Qserv xrdssi plugin configuration file: " << argv[1]);

    std::string workerConfigFile = argv[1];
    auto const workerConfig = wconfig::WorkerConfig::create(workerConfigFile);
    LOGS(_log, LOG_LVL_DEBUG, "Qserv xrdssi plugin configuration: " << *workerConfig);

    // Save the ssi logger as it places messages in another file than our log.
    //
    _logSsi = logP;

    // Save the cluster object as we will need to use it to inform the cluster
    // when chunks come and go. We also can use it to schedule ourselves. The
    // object or its absence will indicate whether or not we need to provide
    // any service other than QueryResource().
    //
    _cmsSsi = clsP;

    // We would process the configuration file (if present), any present
    // parameters and the command line arguments. However, at the moment, we
    // have nothing of interest in any of these arguments. So, we ignore them.
    //

    // Herald our initialization
    //
    LOGS(_log, LOG_LVL_DEBUG, "SsiProvider initializing...");
    _logSsi->Msg("Qserv", "Provider Initializing");

    // Initialize the inventory. We need to be able to handle QueryResource()
    // calls either in the data provider and the metadata provider (we can be
    // either one).
    //
    _chunkInventory.init(x.getName(), workerConfig->getMySqlConfig());

    // If we are a data provider (i.e. xrootd) then we need to get the service
    // object. It will print the exported paths. Otherwise, we need to print
    // them here. This is kludgy and should be corrected when we transition to a
    // single shared memory inventory object which should do this by itself.
    //
    if (clsP && clsP->DataContext()) {
        _service.reset(new SsiService(logP));
    } else {
        std::ostringstream ss;
        ss << "Provider valid paths(ci): ";
        _chunkInventory.dbgPrint(ss);
        LOGS(_log, LOG_LVL_DEBUG, ss.str());
        _logSsi->Msg("Qserv", ss.str().c_str());
    }

    // We have completed full initialization. Return sucess.
    //
    return true;
}

/******************************************************************************/
/*                         Q u e r y R e s o u r c e                          */
/******************************************************************************/

XrdSsiProvider::rStat SsiProviderServer::QueryResource(char const* rName, char const* contact) {
    // Validate resource name based on its proposed type
    return notPresent;
}

void SsiProviderServer::ResourceAdded(const char* rName) {
    // Handle resource based on its proposed type
    return;
}

void SsiProviderServer::ResourceRemoved(const char* rName) {
    // Handle resource based on its proposed type
    return;
}

}  // namespace lsst::qserv::xrdsvc
