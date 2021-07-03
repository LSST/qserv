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
#ifndef LSST_QSERV_LOADER_MASTERSERVER_H
#define LSST_QSERV_LOADER_MASTERSERVER_H

// system headers
#include <cstdlib>
#include <iostream>
#include <boost/bind.hpp>
#include <boost/asio.hpp>

// Qserv headers
#include "loader/ServerUdpBase.h"
#include "loader/MWorkerList.h"

namespace lsst {
namespace qserv {
namespace loader {

class LoaderMsg;
class CentralMaster;

class MasterServer : public ServerUdpBase {
public:
    MasterServer(boost::asio::io_service& io_service, std::string const& host, int port, CentralMaster* centralMaster)
        : ServerUdpBase(io_service, host, port), _centralMaster(centralMaster) {}

    ~MasterServer() override = default;

    BufferUdp::Ptr parseMsg(BufferUdp::Ptr const& data,
                            boost::asio::ip::udp::endpoint const& endpoint) override;


    BufferUdp::Ptr workerAddRequest(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                    boost::asio::ip::udp::endpoint const& senderEndpoint);

    BufferUdp::Ptr workerListRequest(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                     boost::asio::ip::udp::endpoint const& senderEndpoint);

    BufferUdp::Ptr workerInfoRequest(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                     boost::asio::ip::udp::endpoint const& senderEndpoint);

    BufferUdp::Ptr workerKeysInfo(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                                boost::asio::ip::udp::endpoint const& senderEndpoint);

    // Replies should only be sent on errors and always be sent to the server port. A reply
    // that gets lost in transmission is going to be a nuisance, requiring extra timeouts.
    BufferUdp::Ptr prepareReplyMsg(boost::asio::ip::udp::endpoint const& senderEndpoint,
                                   LoaderMsg const& inMsg,
                                   int status, std::string const& msgTxt); // TODO shows up in both MasterServer and WorkerServer

private:
    CentralMaster* _centralMaster;
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_MASTERSERVER_H
