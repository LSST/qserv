// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
 *
 */
#ifndef LSST_QSERV_LOADER_CLIENTCONFIG_H
#define LSST_QSERV_LOADER_CLIENTCONFIG_H

// Qserv headers
#include "loader/ConfigBase.h"

namespace lsst {
namespace qserv {
namespace loader {

/// A class for reading the configuration file for the client which consists of
/// a collection of key-value pairs and provide access functions for those values.
///
class ClientConfig : public ConfigBase {
public:
    explicit ClientConfig(std::string const& configFileName)
        : ClientConfig(util::ConfigStore(configFileName)) {}

    ClientConfig() = delete;
    ClientConfig(ClientConfig const&) = delete;
    ClientConfig& operator=(ClientConfig const&) = delete;

    std::string getMasterHost() const { return _masterHost->getValue(); }
    int getMasterPortUdp() const { return _masterPortUdp->getInt(); }
    int getDefWorkerPortUdp() const { return _defWorkerPortUdp->getInt(); }
    std::string getDefWorkerHost() const { return _defWorkerHost->getValue(); }
    int getClientPortUdp() const { return _clientPortUdp->getInt(); }
    int getThreadPoolSize() const { return _threadPoolSize->getInt(); }
    int getLoopSleepTime() const { return _loopSleepTime->getInt(); } // TODO: Maybe chrono types for times
    int getMaxLookups() const { return _maxLookups->getInt(); }
    int getMaxInserts() const { return _maxInserts->getInt(); }
    int getMaxRequestSleepTime() const { return _maxRequestSleepTime->getInt(); }
    int getIOThreads() const { return _iOThreads->getInt(); }

    std::ostream& dump(std::ostream &os) const override;

    std::string const header{"client"};
private:
    ClientConfig(util::ConfigStore const& configStore);

    /// Master host name
    ConfigElement::Ptr _masterHost{
        ConfigElement::create(cfgList, header, "masterHost", ConfigElement::STRING, true)};
    /// Master UDP port
    ConfigElement::Ptr _masterPortUdp{
        ConfigElement::create(cfgList, header, "masterPortUdp", ConfigElement::INT, true)};
    /// UDP port for default worker. Reasonable value - 9876
    ConfigElement::Ptr _clientPortUdp{
        ConfigElement::create(cfgList, header, "clientPortUdp", ConfigElement::INT, true)};
    /// Default worker host name
    ConfigElement::Ptr _defWorkerHost{
        ConfigElement::create(cfgList, header, "defWorkerHost", ConfigElement::STRING, true)};
    /// Default worker UDP port. Reasonable value - 9876
    ConfigElement::Ptr _defWorkerPortUdp{
        ConfigElement::create(cfgList, header, "defWorkerPortUdp", ConfigElement::INT, true)};
    /// Size of the thread pool. Reasonable value - 10
    ConfigElement::Ptr _threadPoolSize{
        ConfigElement::create(cfgList, header, "threadPoolSize", ConfigElement::INT, true)};
    /// Time spent sleeping between checking elements in the DoList in micro seconds. 100000
    ConfigElement::Ptr _loopSleepTime{
        ConfigElement::create(cfgList, header, "loopSleepTime", ConfigElement::INT, false, "100000")};
    /// Maximum number of lookup requests allowed in the DoList.
    ConfigElement::Ptr _maxLookups{
        ConfigElement::create(cfgList, header, "maxLookups", ConfigElement::INT, false, "90000")};
    /// Maximum number of insert requests allowed in the DoList.
    ConfigElement::Ptr _maxInserts{
        ConfigElement::create(cfgList, header, "maxInserts", ConfigElement::INT, false, "90000")};
    /// When reaching maxInserts or maxLookups, sleep this long before trying to add more,
    /// in micro seconds. 100000micro = 0.1sec
    ConfigElement::Ptr _maxRequestSleepTime{
        ConfigElement::create(cfgList, header,
                              "maxRequestSleepTime", ConfigElement::INT, false, "100000")};
    /// Number of IO threads the server should run.
    ConfigElement::Ptr _iOThreads{
        ConfigElement::create(cfgList, header, "iOThreads", ConfigElement::INT, false, "4")};
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_CLIENTCONFIG_H
