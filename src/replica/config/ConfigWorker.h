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
#ifndef LSST_QSERV_REPLICA_CONFIGWORKER_H
#define LSST_QSERV_REPLICA_CONFIGWORKER_H

// System headers
#include <cstdint>
#include <iosfwd>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/config/ConfigHost.h"

// This header declarations
namespace lsst::qserv::replica {

/**
 * Class ConfigQservWorker encapsulates various parameters describing
 * the Qserv worker.
 */
class ConfigQservWorker {
public:
    ConfigHost host;    // The host name (and IP address) of the worker's management service
    uint16_t port = 0;  // The port number of the worker's management service

    /// @return JSON representation of the object
    nlohmann::json toJson() const;

    /// @return 'true' if workers objects have the same values of attributes
    bool operator==(ConfigQservWorker const& other) const;

    /// @return 'true' if workers objects don't have the same values of attributes
    bool operator!=(ConfigQservWorker const& other) const { return !(operator==(other)); }
};

/**
 * Class ConfigWorker encapsulates various parameters describing a worker.
 */
class ConfigWorker {
public:
    std::string name;  // The logical name of a worker

    bool isEnabled = true;    // The worker is allowed to participate in the replication operations
    bool isReadOnly = false;  // The worker can only serve as a source of replicas.
                              // New replicas can't be placed on it.

    ConfigHost svcHost;    // The host name (and IP address) of the worker service
    uint16_t svcPort = 0;  // The port number of the worker service

    ConfigHost httpSvcHost;    // The host name (and IP address) of the HTTP-based worker service
    uint16_t httpSvcPort = 0;  // The port number of the HTTP-based worker service

    ConfigHost fsHost;    // The host name (and IP address) of the file service for the worker
    uint16_t fsPort = 0;  // The port number for the file service for the worker

    std::string dataDir;  // An absolute path to the data directory under which the MySQL
                          // database folders are residing.

    ConfigHost loaderHost;    // The host name (and IP address) of the ingest (loader) service
    uint16_t loaderPort = 0;  // The port number of the ingest service

    std::string loaderTmpDir;  // An absolute path to the temporary directory which would be used
                               // by the service. The folder must be write-enabled for a user
                               // under which the service will be run.

    ConfigHost exporterHost;    // The host name (and IP address) of the data exporting service
    uint16_t exporterPort = 0;  // The port number of the data exporting service

    std::string exporterTmpDir;  // An absolute path to the temporary directory which would be used
                                 // by the service. The folder must be write-enabled for a user
                                 // under which the service will be run.

    ConfigHost httpLoaderHost;    // The host name (and IP address) of the HTTP-based ingest (loader) service
    uint16_t httpLoaderPort = 0;  // The port number of the HTTP-based ingest service

    std::string httpLoaderTmpDir;  // An absolute path to the temporary directory which would be used
                                   // by the HTTP-based service. The folder must be write-enabled for a user
                                   // under which the service will be run.

    ConfigQservWorker qservWorker;  // Parameters of the corresponding Qserv worker.

    /**
     * This function treats its numeric input as a tri-state variable, where
     * any negative value means no user input was provided, 0 value represents
     * the boolean 'false', and any other positive number represents 'true'.
     *
     * @param in  The tri-state input number to be evaluated.
     * @param out A reference to the output variable to be updated if required
     *   conditions are met.
     */
    static void update(int const in, bool& out) {
        if (in == 0)
            out = false;
        else if (in > 0)
            out = true;
    }

    /**
     * This function assumes that any non-empty value given on the input
     * means an explicit user input that needs to be checked against the present
     * state of the parameter to decide if a change is needed.
     *
     * @param in  The input string to be evaluated.
     * @param out A reference to the output variable to be updated if required
     *   conditions are met.
     */
    static void update(std::string const& in, std::string& out) {
        if (!in.empty()) out = in;
    }

    /**
     * This function is specialized for 16-bit port numbers. It treats
     * any non-zero input as an explicit user input that needs to be checked
     * against the present state of the parameter to decide if a change is needed.
     *
     * @param in  The input number to be evaluated.
     * @param out A reference to the output variable to be updated if required
     *   conditions are met.
     */
    static void update(uint16_t const in, uint16_t& out) {
        if (in != 0) out = in;
    }

    /**
     * Construct from a JSON object.
     * @param obj The object to be used as a source of the worker's state.
     * @throw std::invalid_argument If the input object can't be parsed, or if it has
     *   incorrect schema.
     */
    explicit ConfigWorker(nlohmann::json const& obj);

    ConfigWorker() = default;
    ConfigWorker(ConfigWorker const&) = default;
    ConfigWorker& operator=(ConfigWorker const&) = default;

    /// @return JSON representation of the object
    nlohmann::json toJson() const;

    /// @return 'true' if workers objects have the same values of attributes
    bool operator==(ConfigWorker const& other) const;

    /// @return 'true' if workers objects don't have the same values of attributes
    bool operator!=(ConfigWorker const& other) const { return !(operator==(other)); }
};

std::ostream& operator<<(std::ostream& os, ConfigWorker const& info);

}  // namespace lsst::qserv::replica

#endif  // LSST_QSERV_REPLICA_CONFIGWORKER_H
