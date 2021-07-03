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
#include "loader/LoaderMsg.h"

// System headers
#include <iostream>

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.LoaderMsg");
}

namespace lsst {
namespace qserv {
namespace loader {


LoaderMsg::LoaderMsg(uint16_t kind, uint64_t id, std::string const& host, uint32_t port) :
        msgKind(std::make_shared<UInt16Element>(kind)),
        msgId(std::make_shared<UInt64Element>(id)),
        senderHost(std::make_shared<StringElement>(host)),
        senderPort(std::make_shared<UInt32Element>(port)) {
}


void LoaderMsg::parseFromData(BufferUdp& data) {
    MsgElement::Ptr elem = MsgElement::retrieve(data);
    msgKind = std::dynamic_pointer_cast<UInt16Element>(elem);
    if (msgKind == nullptr) {
        throw LoaderMsgErr(ERR_LOC, "LoaderMsg::parseMsg wrong type for msgKind:" +
                           MsgElement::getStringVal(elem));
    }

    elem = MsgElement::retrieve(data);
    msgId = std::dynamic_pointer_cast<UInt64Element>(elem);
    if (msgId == nullptr) {
        throw LoaderMsgErr(ERR_LOC, "LoaderMsg::parseMsg wrong type for msgId:" +
                           MsgElement::getStringVal(elem));
    }

    elem = MsgElement::retrieve(data);
    senderHost = std::dynamic_pointer_cast<StringElement>(elem);
    if (senderHost == nullptr) {
        throw LoaderMsgErr(ERR_LOC, "LoaderMsg::parseMsg wrong type for senderHost:" +
                           MsgElement::getStringVal(elem));
    }

    elem = MsgElement::retrieve(data);
    senderPort = std::dynamic_pointer_cast<UInt32Element>(elem);
    if (senderPort == nullptr) {
        throw LoaderMsgErr(ERR_LOC, "LoaderMsg::parseMsg wrong type for senderPort:" +
                           MsgElement::getStringVal(elem));
    }
}


void LoaderMsg::appendToData(BufferUdp& data) {
    bool success = true;
    if (msgKind == nullptr || msgId == nullptr || senderHost == nullptr || senderPort == nullptr) {
        success = false;
    } else {
        success |= msgKind->appendToData(data);
        success |= msgId->appendToData(data);
        success |= senderHost->appendToData(data);
        success |= senderPort->appendToData(data);
    }

    if (not success) {
        std::string str("LoaderMsg::serialize nullptr");
        str += " msgKind=" + MsgElement::getStringVal(msgKind);
        str += " msgId=" + MsgElement::getStringVal(msgId);
        str += " senderHost=" + MsgElement::getStringVal(senderHost);
        str += " senderPort=" + MsgElement::getStringVal(senderPort);
        throw LoaderMsgErr(ERR_LOC, str);
    }
}


std::string LoaderMsg::getStringVal() const {
    std::string str("LMsg(");
    str += msgKind->getStringVal() + " " + msgId->getStringVal() + " ";
    str += senderHost->getStringVal() + ":" + senderPort->getStringVal() + ")";
    return str;
}

std::ostream& operator<<(std::ostream& os, LoaderMsg const& loaderMsg) {
    os << loaderMsg.getStringVal();
    return os;
}


}}} // namespace lsst::qserv::loader
