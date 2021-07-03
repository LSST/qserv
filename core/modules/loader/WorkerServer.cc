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
#include "loader/WorkerServer.h"

// System headers
#include <iostream>

// Third-party headers

// Qserv headers
#include "loader/CentralWorker.h"
#include "loader/LoaderMsg.h"
#include "proto/loader.pb.h"
#include "proto/ProtoImporter.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.WorkerServer");
}

namespace lsst {
namespace qserv {
namespace loader {

BufferUdp::Ptr WorkerServer::parseMsg(BufferUdp::Ptr const& data,
                                      boost::asio::ip::udp::endpoint const& senderEndpoint) {
    std::string const funcName("WorkerServer::parseMsg");
    BufferUdp::Ptr sendData; /// nullptr for empty response.
    LoaderMsg inMsg;
    inMsg.parseFromData(*data);
    LOGS(_log, LOG_LVL_INFO, funcName << " sender " << senderEndpoint <<
                             " kind=" << inMsg.msgKind->element <<
                             " data length=" << data->getAvailableWriteLength());
    switch (inMsg.msgKind->element) {
        case LoaderMsg::MAST_INFO:
            // TODO handle a message with information about the master
            break;
        case LoaderMsg::MAST_WORKER_LIST:
            _centralWorker->getWorkerList()->workerListReceive(data);
            break;
        case LoaderMsg::MSG_RECEIVED:
            _msgRecieved(inMsg, data, senderEndpoint);
            sendData.reset(); // never send a response back for one of these, infinite loop.
            break;
        case LoaderMsg::MAST_WORKER_INFO:
            _centralWorker->workerInfoReceive(data);
            break;
        case LoaderMsg::KEY_INSERT_REQ:
            _centralWorker->workerKeyInsertReq(inMsg, data);
            break;
        case LoaderMsg::KEY_LOOKUP_REQ:
            _centralWorker->workerKeyInfoReq(inMsg, data);
            break;
        case LoaderMsg::WORKER_KEYS_INFO_REQ:
            _centralWorker->workerWorkerKeysInfoReq(inMsg, data);
            break;
        case LoaderMsg::WORKER_RIGHT_NEIGHBOR:
            _centralWorker->workerWorkerSetRightNeighbor(inMsg, data);
            break;
        case LoaderMsg::WORKER_LEFT_NEIGHBOR:
            _centralWorker->workerWorkerSetLeftNeighbor(inMsg, data);
            break;

        // Following not expected by worker
        case LoaderMsg::KEY_LOOKUP:
        case LoaderMsg::MAST_INFO_REQ:
        case LoaderMsg::MAST_WORKER_LIST_REQ:
        case LoaderMsg::MAST_WORKER_INFO_REQ:
        case LoaderMsg::MAST_WORKER_ADD_REQ:
            // Response for known but unexpected message.
            sendData = prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, "unexpected Msg Kind");
            // Fallthrough
        default:
            sendData = prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, "unknownMsgKind");
    }

    return sendData;
}


BufferUdp::Ptr WorkerServer::prepareReplyMsg(boost::asio::ip::udp::endpoint const& senderEndpoint,
                                              LoaderMsg const& inMsg,
                                              int status, std::string const& msgTxt) {

    if (status != LoaderMsg::STATUS_SUCCESS) {
        LOGS(_log,LOG_LVL_WARN, "Error response Original from " << senderEndpoint <<
                " msg=" << msgTxt << " inMsg=" << inMsg.getStringVal());
    }

    LoaderMsg outMsg(LoaderMsg::MSG_RECEIVED, inMsg.msgId->element, getOurHostName(), getOurPort());

    // create the proto buffer
    proto::LdrMsgReceived protoBuf;
    protoBuf.set_originalid(inMsg.msgId->element);
    protoBuf.set_originalkind(inMsg.msgKind->element);
    protoBuf.set_status(LoaderMsg::STATUS_PARSE_ERR);
    protoBuf.set_errmsg(msgTxt);
    protoBuf.set_dataentries(0);

    StringElement respBuf;
    protoBuf.SerializeToString(&(respBuf.element));

    auto sendData = std::make_shared<BufferUdp>(1000); // this message should be fairly small.
    outMsg.appendToData(*sendData);
    respBuf.appendToData(*sendData);
    return sendData;
}


void WorkerServer::_msgRecieved(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                boost::asio::ip::udp::endpoint const& senderEndpoint) {
    bool success = true;
    // This is only really expected for parsing errors. Most responses to
    // requests come in as normal messages.
    StringElement::Ptr seData = std::dynamic_pointer_cast<StringElement>(MsgElement::retrieve(*data));
    if (seData == nullptr) {
        success = false;
    }

    std::unique_ptr<proto::LdrMsgReceived> protoBuf;
    if (success) {
        protoBuf = seData->protoParse<proto::LdrMsgReceived>();
        if (protoBuf == nullptr) { success = false; }
    }

    std::stringstream os;
    int status = LoaderMsg::STATUS_PARSE_ERR;

    if (success) {
        auto originalId   = protoBuf->originalid();
        auto originalKind = protoBuf->originalkind();
        status            = protoBuf->status();
        auto errMsg       = protoBuf->errmsg();
        os << " sender=" << senderEndpoint <<
                " id=" << originalId << " kind=" << originalKind << " status=" << status <<
                " msg=" << errMsg;
    } else {
        os << " Failed to parse MsgRecieved! sender=" << senderEndpoint;
    }

    if (status != LoaderMsg::STATUS_SUCCESS) {
        ++_errCount;
        LOGS(_log, LOG_LVL_WARN, "MsgRecieved Message sent by this server caused error at its target" <<
              " errCount=" << _errCount << os.str());
    } else {
        // There shouldn't be many of these, unless there's a need to time things.
        LOGS(_log, LOG_LVL_INFO, "MsgRecieved " << os.str());
    }
}

}}} // namespace lsst:qserv::loader





