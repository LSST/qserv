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
#ifndef LSST_QSERV_LOADER_MASTERCONFIG_H
#define LSST_QSERV_LOADER_MASTERCONFIG_H


// Qserv headers
#include "loader/ConfigBase.h"


namespace lsst {
namespace qserv {
namespace loader {

/// A class for reading the configuration file for the master which consists of
/// a collection of key-value pairs and provide access functions for those values.
///
class MasterConfig : public ConfigBase {
public:
    /// Constructor can throw ConfigErr
    explicit MasterConfig(std::string const& configFileName)
        : MasterConfig(util::ConfigStore(configFileName)) {}

    MasterConfig() = delete;
    MasterConfig(MasterConfig const&) = delete;
    MasterConfig& operator=(MasterConfig const&) = delete;

    int getMasterPort() const { return _portUdp->getInt(); }
    int getThreadPoolSize() const { return _threadPoolSize->getInt(); }
    int getLoopSleepTime() const { return _loopSleepTime->getInt(); }
    int getMaxKeysPerWorker() const { return _maxKeysPerWorker->getInt(); }
    int getIOThreads() const { return _iOThreads->getInt(); }


    std::ostream& dump(std::ostream &os) const override;

    std::string const header{"master"}; ///< Header for values
private:
    MasterConfig(util::ConfigStore const& configStore);

    /// UDP port for the master - usually 9875
    ConfigElement::Ptr _portUdp{ConfigElement::create(cfgList, header, "portUdp", ConfigElement::INT, true)};
    /// Maximum average keys per worker before activating a new worker.
    ConfigElement::Ptr _maxKeysPerWorker{
        ConfigElement::create(cfgList, header, "maxKeysPerWorker", ConfigElement::INT, true)};
    /// Size of the master's thread pool - 10
    ConfigElement::Ptr _threadPoolSize{
        ConfigElement::create(cfgList, header, "threadPoolSize", ConfigElement::INT, true)};
    /// Time spent sleeping between checking elements in the DoList in microseconds. 0.1 seconds.
    ConfigElement::Ptr _loopSleepTime{
        ConfigElement::create(cfgList, header, "loopSleepTime", ConfigElement::INT, true)};
    /// Number of IO threads the server should run.
    ConfigElement::Ptr _iOThreads{
        ConfigElement::create(cfgList, header, "iOThreads", ConfigElement::INT, false, "5")};
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_MASTERCONFIG_H
