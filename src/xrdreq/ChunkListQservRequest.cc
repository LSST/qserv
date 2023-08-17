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
 */

// Class header
#include "xrdreq/ChunkListQservRequest.h"

// System headers
#include <string>

// Qserv headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdreq.ChunkListQservRequest");
}  // namespace

namespace lsst::qserv::xrdreq {

ChunkListQservRequest::ChunkListQservRequest(bool rebuild, bool reload, CallbackType onFinish)
        : _rebuild(rebuild), _reload(reload), _onFinish(onFinish) {
    LOGS(_log, LOG_LVL_TRACE, "ChunkListQservRequest  ** CONSTRUCTED **");
}

ChunkListQservRequest::~ChunkListQservRequest() {
    LOGS(_log, LOG_LVL_TRACE, "ChunkListQservRequest  ** DELETED **");
}

void ChunkListQservRequest::onRequest(proto::FrameBuffer& buf) {
    proto::WorkerCommandH header;
    header.set_command(proto::WorkerCommandH::UPDATE_CHUNK_LIST);
    buf.serialize(header);

    proto::WorkerCommandUpdateChunkListM message;
    message.set_rebuild(_rebuild);
    message.set_reload(_reload);
    buf.serialize(message);
}

void ChunkListQservRequest::onResponse(proto::FrameBufferView& view) {
    string const context = "ChunkListQservRequest  ";

    proto::WorkerCommandUpdateChunkListR reply;
    view.parse(reply);

    LOGS(_log, LOG_LVL_TRACE,
         context << "** SERVICE REPLY **  status: "
                 << proto::WorkerCommandStatus_Code_Name(reply.status().code()));

    ChunkCollection added;
    ChunkCollection removed;

    if (reply.status().code() == proto::WorkerCommandStatus::SUCCESS) {
        int const numAdded = reply.added_size();
        for (int i = 0; i < numAdded; i++) {
            proto::WorkerCommandChunk const& chunkEntry = reply.added(i);
            Chunk chunk{chunkEntry.chunk(), chunkEntry.db()};
            added.push_back(chunk);
        }
        LOGS(_log, LOG_LVL_TRACE, context << "total chunks added: " << numAdded);

        int const numRemoved = reply.removed_size();
        for (int i = 0; i < numRemoved; i++) {
            proto::WorkerCommandChunk const& chunkEntry = reply.removed(i);
            Chunk chunk{chunkEntry.chunk(), chunkEntry.db()};
            removed.push_back(chunk);
        }
        LOGS(_log, LOG_LVL_TRACE, context << "total chunks removed: " << numRemoved);
    }
    if (nullptr != _onFinish) {
        // Clearing the stored callback after finishing the up-stream notification
        // has two purposes:
        //
        // 1. it guaranties (exactly) one time notification
        // 2. it breaks the up-stream dependency on a caller object if a shared
        //    pointer to the object was mentioned as the lambda-function's closure
        auto onFinish = move(_onFinish);
        _onFinish = nullptr;
        onFinish(reply.status().code(), reply.status().error(), added, removed);
    }
}

void ChunkListQservRequest::onError(string const& error) {
    if (nullptr != _onFinish) {
        // Clearing the stored callback after finishing the up-stream notification
        // has two purposes:
        //
        // 1. it guaranties (exactly) one time notification
        // 2. it breaks the up-stream dependency on a caller object if a shared
        //    pointer to the object was mentioned as the lambda-function's closure
        auto onFinish = move(_onFinish);
        _onFinish = nullptr;
        onFinish(proto::WorkerCommandStatus::ERROR, error, ChunkCollection(), ChunkCollection());
    }
}

ReloadChunkListQservRequest::Ptr ReloadChunkListQservRequest::create(
        ChunkListQservRequest::CallbackType onFinish) {
    ReloadChunkListQservRequest::Ptr ptr(new ReloadChunkListQservRequest(onFinish));
    ptr->setRefToSelf4keepAlive(ptr);
    return ptr;
}

ReloadChunkListQservRequest::ReloadChunkListQservRequest(ChunkListQservRequest::CallbackType onFinish)
        : ChunkListQservRequest(false, true, onFinish) {}

RebuildChunkListQservRequest::Ptr RebuildChunkListQservRequest::create(
        bool reload, ChunkListQservRequest::CallbackType onFinish) {
    RebuildChunkListQservRequest::Ptr ptr(new RebuildChunkListQservRequest(reload, onFinish));
    ptr->setRefToSelf4keepAlive(ptr);
    return ptr;
}

RebuildChunkListQservRequest::RebuildChunkListQservRequest(bool reload,
                                                           ChunkListQservRequest::CallbackType onFinish)
        : ChunkListQservRequest(true, reload, onFinish) {}

}  // namespace lsst::qserv::xrdreq
