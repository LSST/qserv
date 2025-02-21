/*
 * LSST Data Management System
 * Copyright 2017 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */

// Class header
#include "qhttp/Server.h"

// System headers
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <ratio>
#include <string>

// Third-party headers
#include "boost/asio.hpp"
#include "boost/asio/steady_timer.hpp"
#include "boost/regex.hpp"

// Local headers
#include "lsst/log/Log.h"
#include "qhttp/AjaxEndpoint.h"
#include "qhttp/LogHelpers.h"
#include "qhttp/StaticContent.h"
#include "qhttp/Status.h"

namespace asio = boost::asio;
namespace errc = boost::system::errc;
namespace ip = boost::asio::ip;
namespace chrono = std::chrono;

using namespace std::literals;

#define DEFAULT_REQUEST_TIMEOUT 5min
#define DEFAULT_MAX_RESPONSE_BUF_SIZE 2097152

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qhttp");
}

using namespace std;

namespace lsst::qserv::qhttp {

Server::Ptr Server::create(asio::io_service& io_service, unsigned short port, int backlog,
                           std::size_t const maxResponseBufSize) {
    return std::shared_ptr<Server>(new Server(io_service, port, backlog, maxResponseBufSize));
}

unsigned short Server::getPort() { return _acceptor.local_endpoint().port(); }

Server::Server(asio::io_service& io_service, unsigned short port, int backlog,
               std::size_t const maxResponseBufSize)
        : _io_service(io_service),
          _backlog(backlog),
          _maxResponseBufSize(std::max(maxResponseBufSize, (std::size_t)DEFAULT_MAX_RESPONSE_BUF_SIZE)),
          _acceptorEndpoint(ip::tcp::v4(), port),
          _acceptor(io_service),
          _requestTimeout(DEFAULT_REQUEST_TIMEOUT) {}

Server::~Server() { stop(); }

void Server::addHandler(std::string const& method, std::string const& pattern, Handler handler,
                        bool readEntireBody) {
    auto& handlers = _pathHandlersByMethod[method];
    handlers.resize(handlers.size() + 1);
    auto& phandler = handlers.back();
    phandler.path.parse(pattern);
    phandler.handler = handler;
    phandler.readEntireBody = readEntireBody;
}

void Server::addHandlers(std::initializer_list<HandlerSpec> handlers) {
    for (auto& handler : handlers) {
        addHandler(handler.method, handler.pattern, handler.handler, handler.readEntireBody);
    }
}

void Server::addStaticContent(std::string const& pattern, std::string const& rootDirectory) {
    StaticContent::add(*this, pattern, rootDirectory);
}

AjaxEndpoint::Ptr Server::addAjaxEndpoint(const std::string& pattern) {
    return AjaxEndpoint::add(*this, pattern);
}

void Server::setRequestTimeout(chrono::milliseconds const& timeout) { _requestTimeout = timeout; }

void Server::setMaxResponseBufSize(std::size_t const maxResponseBufSize) {
    _maxResponseBufSize = std::max(maxResponseBufSize, (std::size_t)DEFAULT_MAX_RESPONSE_BUF_SIZE);
}

void Server::_accept() {
    auto socket = std::make_shared<ip::tcp::socket>(_io_service);
    {
        std::lock_guard<std::mutex> lock(_activeSocketsMutex);
        auto removed = std::remove_if(_activeSockets.begin(), _activeSockets.end(),
                                      [](auto& weakSocket) { return weakSocket.expired(); });
        auto numExpired = _activeSockets.end() - removed;
        if (numExpired != 0) {
            LOGLS_TRACE(_log, logger(this) << "purging tracking for " << numExpired << " expired socket(s)");
            _activeSockets.erase(removed, _activeSockets.end());
        }
        _activeSockets.push_back(socket);
        LOGLS_TRACE(_log, logger(this) << "tracking new socket");
    }

    auto self = shared_from_this();
    _acceptor.async_accept(*socket, [self, socket](boost::system::error_code const& ec) {
        if (!self->_acceptor.is_open() || ec == asio::error::operation_aborted) {
            LOGLS_DEBUG(_log, logger(self) << "accept chain exiting");
            return;
        }
        try {
            if (!ec) {
                LOGLS_INFO(_log, logger(self)
                                         << logger(socket) << "connect from " << socket->remote_endpoint());
                boost::system::error_code ignore;
                socket->set_option(ip::tcp::no_delay(true), ignore);
                self->_readRequest(socket);
            } else {
                LOGLS_ERROR(_log, logger(self) << "accept failed: " << ec.message());
            }
        } catch (boost::system::system_error const& bEx) {
            LOGS(_log, LOG_LVL_ERROR, "qhttp::Server::_accept lambda threw " << bEx.what());
        }
        self->_accept();  // start accept again for the next incoming connection
    });
}

void Server::start() {
    LOGLS_DEBUG(_log, logger(this) << "starting");

    try {
        _acceptor.open(_acceptorEndpoint.protocol());
        _acceptor.set_option(ip::tcp::acceptor::reuse_address(true));
        _acceptor.bind(_acceptorEndpoint);
        _acceptorEndpoint.port(_acceptor.local_endpoint().port());  // preserve assigned port
        _acceptor.listen(_backlog);
    }

    catch (boost::system::system_error const& e) {
        LOGLS_ERROR(_log, logger(this) << "acceptor " << e.what());
        throw e;
    }

    LOGLS_INFO(_log, logger(this) << "listening at " << _acceptor.local_endpoint());
    _accept();
}

void Server::stop() {
    LOGLS_DEBUG(_log, logger(this) << "shutting down");
    boost::system::error_code ignore;
    _acceptor.close(ignore);
    std::lock_guard<std::mutex> lock(_activeSocketsMutex);
    LOGLS_DEBUG(_log, logger(this) << "purging tracking for " << _activeSockets.size() << " socket(s)");
    for (auto& weakSocket : _activeSockets) {
        auto socket = weakSocket.lock();
        if (socket) {
            LOGLS_DEBUG(_log, logger(this) << logger(socket) << "closing");
            socket->lowest_layer().shutdown(ip::tcp::socket::shutdown_both, ignore);
            socket->lowest_layer().close(ignore);
        }
    }
    _activeSockets.clear();
}

std::shared_ptr<asio::steady_timer> Server::_startTimer(std::shared_ptr<ip::tcp::socket> socket) {
    auto timer = std::make_shared<asio::steady_timer>(_io_service);
    timer->expires_from_now(_requestTimeout);
    timer->async_wait([self = shared_from_this(), socket](boost::system::error_code const& ec) {
        if (!ec) {
            LOGLS_WARN(_log, logger(self) << logger(socket) << "read timed out, closing");
            boost::system::error_code ignore;
            socket->lowest_layer().shutdown(ip::tcp::socket::shutdown_both, ignore);
            socket->lowest_layer().close(ignore);
        } else if (ec == asio::error::operation_aborted) {
            LOGLS_TRACE(_log, logger(self) << logger(socket) << "read timeout timer canceled");
        } else {
            LOGLS_ERROR(_log, logger(self) << logger(socket) << "read timeout timer: " << ec.message());
        }
    });
    return timer;
}

void Server::_readRequest(std::shared_ptr<ip::tcp::socket> socket) {
    auto self = shared_from_this();

    // Set up a timer and handler to timeout requests that take too long to arrive.
    auto timer = _startTimer(socket);

    // Create Response object for this request. Completion handler will log total request + response
    // time, then either turn-around or close the client socket as appropriate.
    auto startTime = chrono::steady_clock::now();
    auto reuseSocket = std::make_shared<bool>(false);
    auto const response = std::shared_ptr<Response>(new Response(
            self, socket,
            [self, socket, startTime, reuseSocket](boost::system::error_code const& ec, std::size_t sent) {
                chrono::duration<double, std::milli> elapsed = chrono::steady_clock::now() - startTime;
                string logStr;
                if (LOG_CHECK_LVL(_log, LOG_LVL_INFO)) {
                    logStr = string("request duration ") + to_string(elapsed.count()) + "ms";
                }
                if (!ec && *reuseSocket) {
                    LOGLS_INFO(_log, logger(self) << logger(socket) << logStr << " lingering");
                    self->_readRequest(socket);
                } else {
                    LOGLS_INFO(_log, logger(self) << logger(socket) << logStr << " closing");
                    boost::system::error_code ignore;
                    socket->lowest_layer().shutdown(ip::tcp::socket::shutdown_both, ignore);
                    socket->lowest_layer().close(ignore);
                }
            },
            _maxResponseBufSize));

    // Create Request object for this request, and initiate header read.

    auto const request = std::shared_ptr<Request>(new Request(self, response, socket));
    asio::async_read_until(
            *socket, request->_requestbuf, "\r\n\r\n",
            [self, socket, reuseSocket, request, response, timer](boost::system::error_code const& ec,
                                                                  size_t bytesRead) {
                if (ec == asio::error::operation_aborted) {
                    LOGLS_ERROR(_log, logger(self) << logger(socket) << "header read canceled");
                } else if (ec) {
                    LOGLS_ERROR(_log, logger(self)
                                              << logger(socket) << "header read failed: " << ec.message());
                }
                timer->cancel();
                if (ec) return;
                if (!(request->_parseHeader(bytesRead) && request->_parseUri())) {
                    response->sendStatus(STATUS_BAD_REQ);
                    return;
                }
                if (request->version == "HTTP/1.1") {
                    *reuseSocket = true;
                }
                auto const pathHandler = self->_findPathHandler(request);
                if (pathHandler == nullptr) {
                    LOGLS_DEBUG(_log, logger(self) << logger(socket) << "no handler found");
                    response->sendStatus(STATUS_NOT_FOUND);
                    return;
                }
                if (pathHandler->readEntireBody) {
                    try {
                        request->readEntireBodyAsync([self, pathHandler](auto request, auto response,
                                                                         bool success, size_t bytesRead) {
                            if (success) {
                                self->_dispatchRequest(pathHandler, request, response);
                            } else {
                                response->sendStatus(STATUS_BAD_REQ);
                            }
                        });
                    } catch (...) {
                        response->sendStatus(STATUS_INTERNAL_SERVER_ERR);
                    }
                } else {
                    self->_dispatchRequest(pathHandler, request, response);
                }
            });
}

std::shared_ptr<Server::PathHandler> Server::_findPathHandler(Request::Ptr request) {
    auto pathHandlersIt = _pathHandlersByMethod.find(request->method);
    if (pathHandlersIt != _pathHandlersByMethod.end()) {
        boost::smatch pathMatch;
        for (auto& pathHandler : pathHandlersIt->second) {
            if (boost::regex_match(request->path, pathMatch, pathHandler.path.regex)) {
                pathHandler.path.updateParamsFromMatch(request, pathMatch);
                return std::make_shared<PathHandler>(pathHandler);
            }
        }
    }
    return nullptr;
}

void Server::_dispatchRequest(std::shared_ptr<PathHandler> pathHandler, Request::Ptr request,
                              Response::Ptr response) {
    LOGLS_DEBUG(_log, logger(this) << logger(request->_socket) << "invoking handler for "
                                   << pathHandler->path.regex);
    try {
        pathHandler->handler(request, response);
    } catch (boost::system::system_error const& e) {
        LOGLS_ERROR(_log, logger(this) << logger(request->_socket)
                                       << "exception thrown from handler: " << e.what());
        switch (e.code().value()) {
            case errc::permission_denied:
                response->sendStatus(STATUS_FORBIDDEN);
                break;
            default:
                response->sendStatus(STATUS_INTERNAL_SERVER_ERR);
                break;
        }
    } catch (std::exception const& e) {
        LOGLS_ERROR(_log, logger(this) << logger(request->_socket)
                                       << "exception thrown from handler: " << e.what());
        response->sendStatus(STATUS_INTERNAL_SERVER_ERR);
    }
}

}  // namespace lsst::qserv::qhttp
