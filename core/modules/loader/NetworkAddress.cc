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
#include "loader/NetworkAddress.h"

// System headers
#include <iostream>

// Third-party headers

// Qserv headers
#include "loader/LoaderMsg.h"
#include "loader/LoaderMsg.h"
#include "proto/ProtoImporter.h"
#include "proto/loader.pb.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.NetworkAddress");
}

namespace lsst {
namespace qserv {
namespace loader {


NetworkAddress::UPtr NetworkAddress::create(BufferUdp::Ptr const& bufData, int& tcpPort, std::string const& note) {

    StringElement::Ptr data = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(*bufData));

    if (data == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "NetworkAddress::create data==nullptr " + note);
        return nullptr;
    }

    auto addr = data->protoParse<proto::LdrNetAddress>();
    if (addr == nullptr) {
        LOGS(_log, LOG_LVL_WARN, "NetworkAddress::create STATUS_PARSE_ERR in " + note);
    }

    UPtr netAddr(new NetworkAddress(addr->ip(), addr->udpport()));
    tcpPort = addr->tcpport();
    return netAddr;
}


std::ostream& operator<<(std::ostream& os, NetworkAddress const& adr) {
    os << "ip(" << adr.ip << ":" << adr.port << ")";
    return os;
}

}}} // namespace lsst::qserv::loader


