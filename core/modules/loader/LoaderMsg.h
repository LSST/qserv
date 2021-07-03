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
#ifndef LSST_QSERV_LOADER_LOADERMSG_H
#define LSST_QSERV_LOADER_LOADERMSG_H


// Qserv headers
#include "loader/MsgElement.h"


#define MAX_MSG_STRING_LENGTH 5000

namespace lsst {
namespace qserv {
namespace loader {

/// Base class for loader messages.
//  These messages are meant to be short and simple UDP messages. Long messages
//  may have difficulty being transmitted successfully.
//
//  The message contains the message kind and the address of the entity sending
//  the message.
//
class LoaderMsg {
public:
    enum Kind {
        WAITING      = 0,
        MSG_RECEIVED = 100,    // Standard success/error response to received message.
        TEST,                  // Communications test.
        MAST_INFO_REQ,         // Request some information about the master
        MAST_INFO,             // Information about the master
        MAST_WORKER_LIST_REQ,  // Request a list of workers from the master.
        MAST_WORKER_LIST,      // List of all workers known by the master.
        MAST_WORKER_INFO_REQ,  // Request information for a single worker.
        MAST_WORKER_INFO,      // All the information the master has about one worker. TODO add key list information
        MAST_WORKER_ADD_REQ,   // Request the Master add the worker. MSG_RECIEVED + MAST_WORKER_INFO
        WORKER_KEYS_INFO_REQ,  // Master asking a worker for information about its key-value pairs.
        WORKER_KEYS_INFO,      // Information about number of key values, range, number of new keys.
        KEY_INSERT_REQ,        // Insert a new key with info. MSG_RECEIVED + KEY_INFO
        KEY_INSERT_COMPLETE,   // Key has been inserted and logged.
        KEY_LOOKUP_REQ,          // Request info for a single key.
        KEY_LOOKUP,              // Information about a specific key. (includes file id and row)
        WORKER_LEFT_NEIGHBOR,  // Mast assigns a left neighbor to a worker.
        WORKER_RIGHT_NEIGHBOR, // Mast assigns a right neighbor to a worker.
        IM_YOUR_L_NEIGHBOR,    // Worker message to other worker to setup being neighbors.
        IM_YOUR_R_NEIGHBOR,    // Worker message to other worker to setup being neighbors.
        NEIGHBOR_VERIFIED,     //
        SHIFT_TO_RIGHT,
        SHIFT_TO_RIGHT_RECEIVED,
        SHIFT_FROM_RIGHT,
        SHIFT_FROM_RIGHT_RECEIVED
    };

    enum Status {
        STATUS_SUCCESS = 0,
        STATUS_PARSE_ERR
    };

    LoaderMsg() = default;
    /// Contains the address of entity sending the message.
    LoaderMsg(uint16_t kind, uint64_t id, std::string const& host, uint32_t port);
    LoaderMsg(LoaderMsg const&) = delete;
    LoaderMsg& operator=(LoaderMsg const&) = delete;

    virtual ~LoaderMsg() = default;

    void parseFromData(BufferUdp& data);
    void appendToData(BufferUdp& data);

    std::string getStringVal() const;

    size_t getExpectedSize() const {
        size_t exp = sizeof(msgKind->element);
        exp += sizeof(msgId->element);
        exp += senderHost->element.size();
        exp += sizeof(senderPort->element);
        return exp;
    }

    UInt16Element::Ptr msgKind;
    UInt64Element::Ptr msgId;
    StringElement::Ptr senderHost;
    UInt32Element::Ptr senderPort;

    friend std::ostream& operator<<(std::ostream& os, LoaderMsg const& loaderMsg);
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_LOADERMSG_H
