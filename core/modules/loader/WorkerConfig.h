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
#ifndef LSST_QSERV_LOADER_WORKERCONFIG_H
#define LSST_QSERV_LOADER_WORKERCONFIG_H

// Qserv headers
#include "loader/ConfigBase.h"

namespace lsst {
namespace qserv {
namespace loader {

/// A class for reading the configuration file for the worker which consists of
/// a collection of key-value pairs and provide access functions for those values.
///
class WorkerConfig : public ConfigBase {
public:
    explicit WorkerConfig(std::string const& configFileName)
        : WorkerConfig(util::ConfigStore(configFileName)) {}

    WorkerConfig() = delete;
    WorkerConfig(WorkerConfig const&) = delete;
    WorkerConfig& operator=(WorkerConfig const&) = delete;

    std::string getMasterHost() const { return _masterHost->getValue(); }
    int getMasterPortUdp() const { return _masterPortUdp->getInt(); }
    int getWPortUdp() const { return _wPortUdp->getInt(); }
    int getWPortTcp() const { return _wPortTcp->getInt(); }
    int getThreadPoolSize() const { return _threadPoolSize->getInt(); }
    int getRecentAddLimit() const { return _recentAddLimit->getInt(); }
    double getThresholdNeighborShift() const { return _thresholdNeighborShift->getDouble(); }
    int getMaxKeysToShift() const { return _maxKeysToShift->getInt(); }
    int getLoopSleepTime() const { return _loopSleepTime->getInt(); }
    int getIOThreads() const { return _iOThreads->getInt(); }

    std::ostream& dump(std::ostream &os) const override;

    std::string const header{"worker"};
private:
    WorkerConfig(util::ConfigStore const& configStore);

    /// Master host name
    ConfigElement::Ptr _masterHost{
        ConfigElement::create(cfgList, header, "masterHost", ConfigElement::STRING, true)};
    /// Master UDP port
    ConfigElement::Ptr _masterPortUdp{
        ConfigElement::create(cfgList, header, "masterPortUdp", ConfigElement::INT, true)};
    /// UDP port for this worker. Reasonable value - 9876
    ConfigElement::Ptr _wPortUdp{
        ConfigElement::create(cfgList, header, "wPortUdp", ConfigElement::INT, true)};
    /// TCP port for this worker. Reasonable value - 9877
    ConfigElement::Ptr _wPortTcp{
        ConfigElement::create(cfgList, header, "wPortTcp", ConfigElement::INT, true)};
    /// Size of the thread pool. Reasonable value - 10
    ConfigElement::Ptr _threadPoolSize{
        ConfigElement::create(cfgList, header, "threadPoolSize", ConfigElement::INT, true)};
    /// Time limit for for a key added to the system to be considered recent seconds - 60000 = 1 minute
    ConfigElement::Ptr _recentAddLimit{
        ConfigElement::create(cfgList, header, "recentAddLimit", ConfigElement::INT, true)};
    /// If a worker has this many times the number of keys as the neighbor, keys should be shifted to
    /// the neighbor. "1.10" indicates keys should be shifted if one worker has 10% or more keys
    /// than the other.
    ConfigElement::Ptr _thresholdNeighborShift{
        ConfigElement::create(cfgList, header, "thresholdNeighborShift", ConfigElement::FLOAT, true)};
    /// The maximum number of keys to shift in a single iteration. During a shift iteration,
    /// there are no new key inserts or lookups. 10000 may be a reasonable value.
    ConfigElement::Ptr _maxKeysToShift{
        ConfigElement::create(cfgList, header, "maxKeysToShift", ConfigElement::INT, true)};
    /// Time spent sleeping between checking elements in the DoList in micro seconds. 100000
    ConfigElement::Ptr _loopSleepTime{
        ConfigElement::create(cfgList, header, "loopSleepTime", ConfigElement::INT, false, "100000")};
    /// Number of IO threads the server should run.
    ConfigElement::Ptr _iOThreads{
        ConfigElement::create(cfgList, header, "iOThreads", ConfigElement::INT, false, "5")};
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_WORKERCONFIG_H
