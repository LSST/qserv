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
#ifndef LSST_QSERV_LOADER_CLIENTSERVER_H
#define LSST_QSERV_LOADER_CLIENTSERVER_H

// system headers
#include <cstdlib>
#include <iostream>

// third party headers
#include <boost/asio.hpp>
#include <boost/bind.hpp>

// Qserv headers
#include "loader/LoaderMsg.h"
#include "loader/ServerUdpBase.h"


namespace lsst {
namespace qserv {
namespace loader {

class CentralClient;

/// This class implements a UDP server for the client so that message replies can be
/// sent directly to the client instead of passed back through the chain of workers
/// that were queried when looking for the worker that could handle this client's
/// request.
/// TODO This class should also be able to handle list of workers and their ranges from
/// the master.
class ClientServer : public ServerUdpBase {
public:
    // The base class default constructor, copy constructor, and operator= have been set to delete.
    ClientServer(boost::asio::io_service& ioService, std::string const& host, int port,
                 CentralClient* centralClient)
        : ServerUdpBase(ioService, host, port), _centralClient(centralClient) {}

    ~ClientServer() override = default;

    /// Parse enough of an incoming message so it can be passed to the proper handler.
    BufferUdp::Ptr parseMsg(BufferUdp::Ptr const& data,
                            boost::asio::ip::udp::endpoint const& endpoint) override;

    /// Build a reply to a message that was received, usually used to handle unknown or unexpected messages.
    /// @return a pointer to a buffer with the constructed message.
    // TODO shows up in both MasterServer and WorkerServer
    BufferUdp::Ptr prepareReplyToMsg(boost::asio::ip::udp::endpoint const& senderEndpoint,
                                     LoaderMsg const& inMsg,
                                     int status, std::string const& msgTxt);

private:
    /// Construct basic replies to unknown and unexpected messages.
    void _msgRecievedHandler(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                             boost::asio::ip::udp::endpoint const& senderEndpoint);


    CentralClient* _centralClient;
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_CLIENTSERVER_H
