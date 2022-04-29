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

#ifndef LSST_QSERV_QHTTP_REQUEST_H
#define LSST_QSERV_QHTTP_REQUEST_H

// System headers
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Third-party headers
#include "boost/asio.hpp"

// Local headers
#include "util/CIUtils.h"

namespace lsst { namespace qserv { namespace qhttp {

class Server;

class Request : public std::enable_shared_from_this<Request> {
public:
    using Ptr = std::shared_ptr<Request>;

    //----- The local address on which this request was accepted

    boost::asio::ip::tcp::endpoint localAddr;

    //----- The remote address from which this request was initiated

    boost::asio::ip::tcp::endpoint remoteAddr;

    //----- Elements of the HTTP header for this request

    std::string method;   // HTTP header method
    std::string target;   // HTTP header target
    std::string version;  // HTTP header version

    //----- Parsed query elements and headers for this request.  Note that parsed HTTP headers and
    //      URL parameters are stored in simple std::maps, so repeated headers or parameters are not
    //      supported (last parsed for any given header or parameter wins).  Headers are stored in a
    //      case-insensitive map, in accordance with HTTP standards.

    std::string path;                                    // path portion of URL
    std::unordered_map<std::string, std::string> query;  // parsed URL query parameters
    std::unordered_map<std::string, std::string, util::ci_hash, util::ci_pred> header;  // parsed HTTP headers
    std::unordered_map<std::string, std::string> params;  // captured URL path elements

    //----- Body content for this request

    std::istream content;                               // unparsed body
    std::unordered_map<std::string, std::string> body;  // parsed body, if x-www-form-urlencoded

private:
    friend class Server;

    Request(Request const&) = delete;
    Request& operator=(Request const&) = delete;

    explicit Request(std::shared_ptr<Server> const server,
                     std::shared_ptr<boost::asio::ip::tcp::socket> const socket);

    bool _parseHeader();
    bool _parseUri();
    bool _parseBody();

    std::string _percentDecode(std::string const& encoded, bool exceptPathDelimeters, bool& hasNULs);

    std::shared_ptr<Server> const _server;
    std::shared_ptr<boost::asio::ip::tcp::socket> const _socket;
    boost::asio::streambuf _requestbuf;
};

}}}  // namespace lsst::qserv::qhttp

#endif  // LSST_QSERV_QHTTP_REQUEST_H
