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
#ifndef LSST_QSERV_LOADER_SERVERTCPBASE_H
#define LSST_QSERV_LOADER_SERVERTCPBASE_H

// system headers
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <cstdlib>
#include <iostream>
#include <set>

// Qserv headers
#include "loader/BufferUdp.h"


namespace lsst {
namespace qserv {
namespace loader {

typedef boost::asio::ip::tcp AsioTcp;

class ServerTcpBase;

class TcpBaseConnection : public std::enable_shared_from_this<TcpBaseConnection> {
public:
    typedef std::shared_ptr<TcpBaseConnection> Ptr;

    static Ptr create(boost::asio::io_context& io_context, ServerTcpBase* tcpBase) {
        return Ptr(new TcpBaseConnection(io_context, tcpBase));
    }

    ~TcpBaseConnection() { shutdown(); }

    AsioTcp::socket& socket() {
        return _socket;
    }

    void start();
    void shutdown();

private:
    TcpBaseConnection(boost::asio::io_context& io_context, ServerTcpBase* tcpBase) :
        _socket(io_context), _serverTcpBase(tcpBase) {}

    void _readKind(const boost::system::error_code&, size_t /*bytes_transferred*/);
    void _recvKind(const boost::system::error_code&, size_t bytesTrans);

    /// Free the connection and cancel shifts from this server.
    void _freeConnect();

    AsioTcp::socket _socket;
    ServerTcpBase* _serverTcpBase; // _serverTcpBase controls this class' lifetime.
    BufferUdp _buf{1000000};

    /// Handle the series of messages where another worker is claiming to be our left neighbor.
    void _handleImYourLNeighbor(uint32_t bytes);
    void _handleImYourLNeighbor1(boost::system::error_code const& ec, size_t bytesTrans);
    void _handleImYourLNeighbor2(boost::system::error_code const& ec, size_t bytesTrans);

    /// Handle the series of messages for shifting to our right neighbor.
    void _handleShiftToRight(uint32_t bytes);
    void _handleShiftToRight1(boost::system::error_code const& ec, size_t bytesTrans);

    /// Handle the series of messages for shifting from our right neighbor.
    void _handleShiftFromRight(uint32_t bytesInMsg);
    void _handleShiftFromRight1(boost::system::error_code const& ec, size_t bytesTrans);

    /// Handle TCP functionality test messages.
    void _handleTest();
    void _handleTest2(boost::system::error_code const& ec, size_t bytesTrans);
    void _handleTest2b(boost::system::error_code const& ec, size_t bytesTrans);
    void _handleTest2c(boost::system::error_code const& ec, size_t bytesTrans);
};


class CentralWorker;


class ServerTcpBase {
public:
    typedef std::shared_ptr<ServerTcpBase> Ptr;
    ServerTcpBase(boost::asio::io_context& io_context, int port) :
        _io_context(io_context),
        _acceptor(io_context, AsioTcp::endpoint(AsioTcp::v4(), port)), _port(port) {
        _startAccept();
    }

    ServerTcpBase(boost::asio::io_context& io_context, int port, CentralWorker* cw) :
        _io_context(io_context),
        _acceptor(io_context, AsioTcp::endpoint(AsioTcp::v4(), port)), _port(port),
        _centralWorker(cw){
        _startAccept();
    }


    ~ServerTcpBase() {
        _io_context.stop();
        for (std::thread& t : _threads) {
            t.join();
        }
        // The server is expected to live until program termination.
        // If a connection is doing something, and this is called, what happens?
        // Check _connections empty before deleting?
        for (auto&& conn:_connections) {
            conn->shutdown();
        }
        _connections.clear();
    }

    void runThread() {
        auto func = [this]() {
            _io_context.run();
        };
        _threads.push_back(std::thread(func));
    }

    bool testConnect();

    void freeConnection(TcpBaseConnection::Ptr const& conn) {
        _connections.erase(conn);
    }

    uint32_t getOurName();

    CentralWorker* getCentralWorker() const { return _centralWorker; }

    static bool writeData(AsioTcp::socket& socket, BufferUdp& data);

private:
    void _startAccept();

    boost::asio::io_context& _io_context;
    AsioTcp::acceptor _acceptor;
    int _port;
    std::vector<std::thread> _threads;
    std::set<TcpBaseConnection::Ptr> _connections;

    CentralWorker* _centralWorker{nullptr}; // not too thrilled with this
};




}}} // namespace lsst:qserv:loader

#endif // LSST_QSERV_LOADER_SERVERTCPBASE_H
