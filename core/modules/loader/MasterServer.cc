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
#include "loader/MasterServer.h"

// System headers
#include <iostream>

// Third-party headers


// qserv headers
#include "loader/CentralMaster.h"
#include "loader/LoaderMsg.h"
#include "loader/NetworkAddress.h"
#include "proto/ProtoImporter.h"
#include "proto/loader.pb.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.MasterServer");
}

namespace lsst {
namespace qserv {
namespace loader {

BufferUdp::Ptr MasterServer::parseMsg(BufferUdp::Ptr const& data,
                                      boost::asio::ip::udp::endpoint const& senderEndpoint) {

    LOGS(_log, LOG_LVL_DEBUG, "MasterServer::parseMsg sender " << senderEndpoint <<
                              " data length=" << data->getAvailableWriteLength());
    BufferUdp::Ptr sendData; /// nullptr for empty response.
    LoaderMsg inMsg;
    try {
        inMsg.parseFromData(*data);
    } catch (LoaderMsgErr const& exc) {
        std::string errMsg("MasterServer::parseMsg inMsg garbled exception ");
        errMsg += exc.what();
        LOGS(_log, LOG_LVL_ERROR, errMsg);
        sendData = prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, errMsg);
        return sendData;
    }

    try {
        LOGS(_log, LOG_LVL_INFO, "MasterServer::parseMsg sender " << senderEndpoint <<
                " kind=" << inMsg.msgKind->element << " data length=" << data->getAvailableWriteLength());
        switch (inMsg.msgKind->element) {
            case LoaderMsg::MSG_RECEIVED:
                // TODO: locate msg id in send messages and take appropriate action
                break;
            case LoaderMsg::MAST_INFO_REQ:
                // TODO: provide performance information about the master via MAST_INFO
                break;
            case LoaderMsg::MAST_WORKER_LIST_REQ:
                sendData = workerListRequest(inMsg, data, senderEndpoint);
                break;
            case LoaderMsg::MAST_WORKER_INFO_REQ:
                // Request information about a specific worker via MAST_WORKER_INFO
                sendData = workerInfoRequest(inMsg, data, senderEndpoint);
                break;
            case LoaderMsg::MAST_WORKER_ADD_REQ:
                sendData = workerAddRequest(inMsg, data, senderEndpoint);
                break;
            case LoaderMsg::WORKER_KEYS_INFO:
                sendData = workerKeysInfo(inMsg, data, senderEndpoint);
                break;
                // following not expected by master
            case LoaderMsg::MAST_INFO:
            case LoaderMsg::MAST_WORKER_LIST:
            case LoaderMsg::MAST_WORKER_INFO:
            case LoaderMsg::KEY_INSERT_REQ:
            case LoaderMsg::KEY_LOOKUP_REQ:
            case LoaderMsg::KEY_LOOKUP:
                /// TODO add msg unexpected by master response.
                break;
            default:
                ++_errCount;
                LOGS(_log, LOG_LVL_ERROR, "unknownMsgKind errCount=" << _errCount << " inMsg=" << inMsg);
                sendData = prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR,
                                           "unknownMsgKind");
        }
    } catch (LoaderMsgErr const& exc) {
        ++_errCount;
        std::string errMsg("MasterServer::parseMsg inMsg garbled exception ");
        errMsg += exc.what();
        LOGS(_log, LOG_LVL_ERROR, errMsg);
        // Send error back to the server in inMsg
        auto reply = prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, errMsg);
        sendBufferTo(inMsg.senderHost->element, inMsg.senderPort->element, *reply);
        return nullptr;
    }

    return sendData;
}


BufferUdp::Ptr MasterServer::prepareReplyMsg(boost::asio::ip::udp::endpoint const& senderEndpoint,
                                              LoaderMsg const& inMsg, int status, std::string const& msgTxt) {

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


BufferUdp::Ptr MasterServer::workerAddRequest(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                              boost::asio::ip::udp::endpoint const& senderEndpoint) {

    /// Message contains the network address of a worker to add to our list.
    int tcpPort = 0;
    auto addReq = NetworkAddress::create(data, tcpPort, "MasterServer::workerAddRequest");
    if (addReq == nullptr) {
        return prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR,
                "STATUS_PARSE_ERR parse error workerAddRequest ");
    }

    // Once the worker has been added, its name will be sent to all other workers.
    _centralMaster->addWorker(addReq->ip, addReq->port, tcpPort);

    LOGS(_log, LOG_LVL_INFO, "Adding worker ip=" << addReq->ip <<
                             " udp=" << addReq->port << " tcp=" << tcpPort);

    return nullptr;
}


BufferUdp::Ptr MasterServer::workerListRequest(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                               boost::asio::ip::udp::endpoint const& senderEndpoint) {
    std::string funcName("MasterServer::workerListRequest");
    LOGS(_log, LOG_LVL_DEBUG, funcName);

    int tcpPort = 0; // only needed for create parameter
    auto addr = NetworkAddress::create(data, tcpPort, funcName);
    if (addr == nullptr) {
        std::string errStr("STATUS_PARSE_ERR parse error in " + funcName);
        LOGS(_log, LOG_LVL_ERROR, errStr);
        return prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, errStr);
    }

    // TODO: put this in a separate thread.
    auto workerList = _centralMaster->getWorkerList();
    workerList->sendListTo(inMsg.msgId->element, addr->ip, addr->port, getOurHostName(), getOurPort());
    LOGS(_log, LOG_LVL_INFO, funcName << " done sendListTo ");

    return nullptr;
}


BufferUdp::Ptr MasterServer::workerKeysInfo(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                            boost::asio::ip::udp::endpoint const& senderEndpoint) {

    std::string funcName("MasterServer::workerKeysInfo");
    LOGS(_log, LOG_LVL_DEBUG, funcName);

    try {
        uint32_t name;
        NeighborsInfo nInfo;
        KeyRange strRange;
        ProtoHelper::workerKeysInfoExtractor(*data, name, nInfo, strRange);
        LOGS(_log, LOG_LVL_INFO, funcName << "name=" << name << " keyCount=" << nInfo.keyCount <<
                                 " recentAdds=" << nInfo.recentAdds << " range=" << strRange);
        // TODO move to separate thread.
        _centralMaster->updateWorkerInfo(name, nInfo, strRange);
    } catch (LoaderMsgErr &msgErr) {
        LOGS(_log, LOG_LVL_ERROR, msgErr.what());
        return prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, msgErr.what());
    }
    return nullptr;
}


BufferUdp::Ptr MasterServer::workerInfoRequest(LoaderMsg const& inMsg, BufferUdp::Ptr const& data,
                                 boost::asio::ip::udp::endpoint const& senderEndpoint) {
    // TODO HIGH Wrap this up in a command and put it on a queue.
    try {
        std::string const funcName("MasterServer::workerInfoRequest");
        int tcpPort = 0;
        NetworkAddress::UPtr requestorAddr = NetworkAddress::create(data, tcpPort, funcName);
        if (requestorAddr == nullptr) {
            throw LoaderMsgErr(ERR_LOC, "requestorAddr nullptr");
        }

        auto protoItem = StringElement::protoParse<proto::WorkerListItem>(*data);
        if (protoItem == nullptr) {
            throw LoaderMsgErr(ERR_LOC, "protoItem nullptr");
        }

        auto workerId = protoItem->wid();
        LOGS(_log, LOG_LVL_INFO, funcName << " Master got wId=" << workerId);

        /// Find the worker name in the map.
        auto workerItem = _centralMaster->getWorkerWithId(protoItem->wid());
        if (workerItem == nullptr) {
            /// TODO construct message for invalid worker
            return nullptr;
        }

        /// Return worker's name, netaddress, and range in MAST_WORKER_INFO msg
        proto::WorkerListItem protoWorker;
        proto::LdrNetAddress* protoAddr = protoWorker.mutable_address();
        proto::WorkerRange* protoRange = protoWorker.mutable_range();
        protoWorker.set_wid(workerItem->getId());
        auto udp = workerItem->getUdpAddress();
        protoAddr->set_ip(udp.ip);
        protoAddr->set_udpport(udp.port);
        protoAddr->set_tcpport(workerItem->getTcpAddress().port);
        auto range = workerItem->getRangeString();
        LOGS(_log, LOG_LVL_INFO, funcName << " workerInfoRequest range = " << range);
        range.loadProtoRange(*protoRange);
        StringElement seItem(protoWorker.SerializeAsString());

        LoaderMsg masterWorkerInfoMsg(LoaderMsg::MAST_WORKER_INFO, _centralMaster->getNextMsgId(),
                                      _centralMaster->getMasterHostName(), _centralMaster->getMasterPort());

        BufferUdp sendBuf;
        masterWorkerInfoMsg.appendToData(sendBuf);
        seItem.appendToData(sendBuf);

        // Send the response to the worker that asked for it.
        try {
            _centralMaster->sendBufferTo(requestorAddr->ip, requestorAddr->port, sendBuf);
        } catch (boost::system::system_error const& e) {
            LOGS(_log, LOG_LVL_ERROR, "MasterServer::workerInfoRequest boost system_error=" << e.what() <<
                    " inMsg=" << inMsg);
        }
    } catch (LoaderMsgErr const& msgErr) {
        LOGS(_log, LOG_LVL_ERROR, msgErr.what());
        return prepareReplyMsg(senderEndpoint, inMsg, LoaderMsg::STATUS_PARSE_ERR, msgErr.what());
    }
    return nullptr;
}

}}} // namespace lsst:qserv::loader


