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
#ifndef LSST_QSERV_LOADER_CENTRAL_CLIENT_H
#define LSST_QSERV_LOADER_CENTRAL_CLIENT_H

// system headers
#include <thread>
#include <vector>

// third party headers
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// Qserv headers
#include "loader/Central.h"
#include "loader/ClientServer.h"
#include "util/Command.h"


namespace lsst {
namespace qserv {
namespace loader {

class ClientConfig;

/// This class is used to track the status and value of jobs inserting
/// key-value pairs to the system or looking up key-value pairs. The
/// Tracker base class provides a means of notifying other threads
/// that the task is complete.
class KeyInfoData : public util::Tracker {
public:
    using Ptr = std::shared_ptr<KeyInfoData>;
    KeyInfoData(CompositeKey const& key_, int chunk_, int subchunk_) :
        key(key_), chunk(chunk_), subchunk(subchunk_) {}

    CompositeKey key;
    int chunk;
    int subchunk;
    bool success{false};

    friend std::ostream& operator<<(std::ostream& os, KeyInfoData const& data);
};

/// This class is 'Central' to the client. The client maintains a UDP port
/// so replies to its request can be sent directly back to it.
/// 'Central' provides access to the master and a DoList for handling requests.
/// TODO Maybe base this on CentralWorker or have a common base class?
class CentralClient : public Central {
public:
    /// The client needs to know the master's IP and its own IP.
    CentralClient(boost::asio::io_service& ioService_,
                  std::string const& hostName, ClientConfig const& cfg);

    void start();

    ~CentralClient() override = default;

    std::string const& getHostName() const { return _hostName; }
    int getUdpPort() const { return _udpPort; }
    int getTcpPort() const { return 0; } ///< No tcp port at this time.

    /// @return the default worker's host name.
    std::string getDefWorkerHost() const { return _defWorkerHost; }
    /// @return the default worker's UDP port
    int getDefWorkerPortUdp() const { return _defWorkerPortUdp; }

    /// Asynchronously request a key value insert to the workers.
    /// This can block if too many key insert requests are already in progress.
    /// @return - a KeyInfoData object for checking the job's status or
    ///           nullptr if CentralClient is already trying to insert the key
    ///           but value doesn't match the existing value. This indicates
    ///           there is an input data error.
    KeyInfoData::Ptr keyInsertReq(CompositeKey const& key, int chunk, int subchunk);
    /// Handle a workers response to the keyInserReq call
    void handleKeyInsertComplete(LoaderMsg const& inMsg, BufferUdp::Ptr const& data);

    /// Asynchronously request a key value lookup from the workers. It returns a
    /// KeyInfoData object to be used to track job status and get the value of the key.
    /// This can block if too many key lookup requests are already in progress.
    KeyInfoData::Ptr keyLookupReq(CompositeKey const& key);
    /// Handle a workers response to the keyInfoReq call.
    void handleKeyLookup(LoaderMsg const& inMsg, BufferUdp::Ptr const& data);

    std::string getOurLogId() const override { return "client"; }

private:
    void _keyInsertReq(CompositeKey const& key, int chunk, int subchunk); ///< see keyInsertReq()
    void _handleKeyInsertComplete(LoaderMsg const& inMsg, std::unique_ptr<proto::KeyInfo>& protoBuf);

    void _keyLookupReq(CompositeKey const& key); ///< see keyLookReq()
    void _handleKeyLookup(LoaderMsg const& inMsg, std::unique_ptr<proto::KeyInfo>& protoBuf);



    /// Create commands to add a key to the index and track that they are done.
    /// It should keep trying this until it works, and then drop it from _waitingKeyInsertMap.
    struct KeyInsertReqOneShot : public DoListItem {
        using Ptr = std::shared_ptr<KeyInsertReqOneShot>;

        KeyInsertReqOneShot(CentralClient* central_, CompositeKey const& key_, int chunk_, int subchunk_) :
            cmdData(std::make_shared<KeyInfoData>(key_, chunk_, subchunk_)), central(central_) {
            setOneShot(true);
        }

        util::CommandTracked::Ptr createCommand() override;

        /// TODO Have this function take result codes (such as 'success') as arguments
        /// and put them in cmdData.
        void keyInsertComplete();

        KeyInfoData::Ptr cmdData;
        CentralClient* central;
    };

    /// Create commands to lookup a key in the index and get its value.
    /// It should keep trying this until it works and then drop it from _waitingKeyInfoMap.
    struct KeyLookupReqOneShot : public DoListItem {
        using Ptr = std::shared_ptr<KeyLookupReqOneShot>;

        KeyLookupReqOneShot(CentralClient* central_, CompositeKey const& key_) :
            cmdData(std::make_shared<KeyInfoData>(key_, -1, -1)), central(central_) { setOneShot(true); }

        util::CommandTracked::Ptr createCommand() override;

        // TODO Have this function take result codes as arguments and put them in cmdData.
        void keyInfoComplete(CompositeKey const& key, int chunk, int subchunk, bool success);

        KeyInfoData::Ptr cmdData;
        CentralClient* central;
    };


    /// TODO The worker IP becomes default worker as it should be able to get
    ///      that information from the master in the future. DM-16555
    const std::string _hostName;
    const int         _udpPort;

    // If const is removed, these will need mutex protection.
    const std::string _defWorkerHost;    ///< Default worker host
    const int         _defWorkerPortUdp; ///< Default worker UDP port


    size_t const _doListMaxLookups = 1000; ///< Maximum number of concurrent lookups in DoList, set by config
    size_t const _doListMaxInserts = 1000; ///< Maximum number of concurrent inserts in DoList, set by config
    /// Time to sleep between checking requests when at max length, set by config
    int const _maxRequestSleepTime = 100000;

    std::map<CompositeKey, KeyInsertReqOneShot::Ptr> _waitingKeyInsertMap; ///< Map of current insert requests.
    std::mutex _waitingKeyInsertMtx; ///< protects _waitingKeyInsertMap, _doListMaxInserts

    std::map<CompositeKey, KeyLookupReqOneShot::Ptr> _waitingKeyLookupMap; ///< Map of current look up requests.
    std::mutex _waitingKeyLookupMtx; ///< protects _waitingKeyLookMap, _doListMaxLookups
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_CENTRAL_CLIENT_H
