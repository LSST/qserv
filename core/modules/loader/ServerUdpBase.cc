// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 AURA/LSST.
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


// Class header
#include "loader/ServerUdpBase.h"

// System headers
#include <iostream>
#include <unistd.h>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.ServerUdpBase");
}

namespace lsst {
namespace qserv {
namespace loader {

std::atomic<uint64_t> ServerUdpBase::_msgIdSeq{1};


ServerUdpBase::ServerUdpBase(boost::asio::io_service& io_service, std::string const& host, int port)
    : _ioService(io_service),
      _socket(io_service, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), port)),
      _hostName(host), _port(port) {
    _receivePrepare(); // Prime the server for an incoming message.
}


void ServerUdpBase::_receiveCallback(boost::system::error_code const& error, size_t bytesRecvd) {
    _data->advanceWriteCursor(bytesRecvd); // _data needs to know the valid portion of the buffer.
    if (!error && bytesRecvd > 0) {
        LOGS(_log, LOG_LVL_INFO, "rCb received(" << bytesRecvd << "):" <<
                                 ", code=" << error << ", from endpoint=" << _senderEndpoint);

        _sendData = parseMsg(_data, _senderEndpoint);
        if (_sendData != nullptr) {
            _sendResponse();
        } else {
            _receivePrepare();
        }
    } else {
        LOGS(_log, LOG_LVL_ERROR, "ServerUdpBase::_receiveCallback got empty message, ignoring");
        _receivePrepare();
    }

}


void ServerUdpBase::_sendResponse() {
    _socket.async_send_to(boost::asio::buffer(_sendData->getReadCursor(), _sendData->getBytesLeftToRead()),
                          _senderEndpoint,
                          [this](boost::system::error_code const& ec, std::size_t bytes_transferred) {
                              _sendCallback(ec, bytes_transferred);
                          }
    );
}


void ServerUdpBase::sendBufferTo(std::string const& hostName, int port, BufferUdp& sendBuf) {
    using namespace boost::asio;
    LOGS(_log, LOG_LVL_DEBUG, "ServerUdpBase::sendBufferTo hostName=" << hostName << " port=" << port);
    try {
        ip::udp::endpoint dest = resolve(hostName, port);
        _socket.send_to(buffer(sendBuf.getReadCursor(), sendBuf.getBytesLeftToRead()), dest);
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "ServerUdpBase::sendBufferTo boost system_error=" << e.what() <<
                " host=" << hostName << " port=" << port << " buf=" << sendBuf);
        throw;
    }
}


/// This function, and its derived children, should return quickly. Handing 'data' off to another thread
/// for handling.
BufferUdp::Ptr ServerUdpBase::parseMsg(BufferUdp::Ptr const& data,
                                       boost::asio::ip::udp::endpoint const& senderEndpoint) {
    // echo server, so send back what we got
    BufferUdp::Ptr sendData = data;
    LOGS(_log, LOG_LVL_INFO, "pM dump(" << sendData->dumpStr() << ") from endpoint " << senderEndpoint);
    return sendData;
}


void ServerUdpBase::_sendCallback(const boost::system::error_code& error, size_t bytes_sent) {
    LOGS(_log, LOG_LVL_INFO, " _sendCallback bytes_sent=" << bytes_sent);
    _receivePrepare();
}

void ServerUdpBase::_receivePrepare() {
    _data = std::make_shared<BufferUdp>(); // New buffer for next response, the old buffer
                                           // may still be in use elsewhere.
    _socket.async_receive_from(boost::asio::buffer(_data->getWriteCursor(),
        _data->getAvailableWriteLength()), _senderEndpoint,
        [this](boost::system::error_code const& ec, std::size_t bytes_transferred) {
            _receiveCallback(ec, bytes_transferred);
        }
    );
}


boost::asio::ip::udp::endpoint ServerUdpBase::resolve(std::string const& hostName, int port) {
    std::lock_guard<std::mutex> lg(_resolveMtx);
    using namespace boost::asio;
    // Resolver returns an iterator. This uses the first item only.
    // Failure to resolve anything throws a boost::system::error.
    ip::udp::endpoint dest =
            *_resolver.resolve(ip::udp::v4(), hostName, std::to_string(port)).begin();
    return dest;
}


}}} // namespace lsst::qserrv::loader
