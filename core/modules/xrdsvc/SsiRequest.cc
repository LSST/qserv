// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 LSST Corporation.
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
#include <xrdsvc/SsiRequest.h>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <string>

// Third-party headers
#include <google/protobuf/arena.h>
#include "XrdSsi/XrdSsiRequest.hh"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "global/LogContext.h"
#include "global/ResourceUnit.h"
#include "proto/FrameBuffer.h"
#include "proto/worker.pb.h"
#include "qdisp/UberJob.h"
#include "qmeta/types.h"
#include "util/Timer.h"
#include "wbase/MsgProcessor.h"
#include "wbase/SendChannelShared.h"
#include "wbase/Task.h"
#include "wpublish/AddChunkGroupCommand.h"
#include "wpublish/ChunkListCommand.h"
#include "wpublish/GetChunkListCommand.h"
#include "wpublish/GetStatusCommand.h"
#include "wpublish/RemoveChunkGroupCommand.h"
#include "wpublish/ResourceMonitor.h"
#include "wpublish/SetChunkListCommand.h"
#include "wpublish/TestEchoCommand.h"
#include "xrdsvc/ChannelStream.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.xrdsvc.SsiRequest");
}

using namespace std;

namespace lsst {
namespace qserv {
namespace xrdsvc {

shared_ptr<wpublish::ResourceMonitor> SsiRequest::_resourceMonitor(new wpublish::ResourceMonitor());

SsiRequest::~SsiRequest () {
    LOGS(_log, LOG_LVL_DEBUG, "~SsiRequest()");
    UnBindRequest();
}

void SsiRequest::reportError (string const& errStr) {
    LOGS(_log, LOG_LVL_WARN, errStr);
    replyError(errStr, EINVAL);
    ReleaseRequestBuffer();
}


set<string> resourceNames; // &&& delete when done

// Step 4
/// Called by XrdSsi to actually process a request.
void SsiRequest::execute(XrdSsiRequest& req) {
    util::Timer t;

    LOGS(_log, LOG_LVL_DEBUG, "Execute request, resource=" << _resourceName);

    char *reqData = nullptr;
    int reqSize;
    t.start();
    reqData = req.GetRequest(reqSize);
    t.stop();
    LOGS(_log, LOG_LVL_DEBUG, "GetRequest took " << t.getElapsed() << " seconds");

    // We bind this object to the request now. This allows us to respond at any
    // time (much simpler). Though the manual forgot to say that all pending
    // events will be reflected on a different thread the moment we bind the
    // request; the fact allows us to use a mutex to serialize the order of
    // initialization and possible early cancellation. We protect this code
    // with a mutex gaurd which will be released upon exit.
    //
    lock_guard<mutex> lock(_finMutex);
    BindRequest(req);

    ResourceUnit ru(_resourceName);

    // Make sure the requested resource belongs to this worker
    if (!(*_validator)(ru)) {
        reportError("WARNING: request to the unowned resource detected:" + _resourceName);
        return;
    }

    // Process the request
    switch (ru.unitType()) {
        case ResourceUnit::DBCHUNK: {

            // Increment the counter of the database/chunk resources in use
            auto resourceLock =
                    make_shared<wpublish::ResourceMonitorLock>(*(_resourceMonitor.get()), _resourceName);

            { /// &&& delete block when done
                resourceNames.insert(_resourceName);
                LOGS(_log, LOG_LVL_WARN, "&&& resourceName=" << _resourceName);
                string rstr = "&&& resources:\n";
                for (auto const& rN:resourceNames) {
                    rstr += rN+ '\n';
                }
                LOGS(_log, LOG_LVL_WARN, "&&&" << rstr);
            }

            // reqData has the entire request, so we can unpack it without waiting for
            // more data.
            LOGS(_log, LOG_LVL_DEBUG, "Decoding TaskMsg of size " << reqSize);
            //&&&auto taskMsg = make_shared<proto::TaskMsg>();
            auto gArena = make_shared<google::protobuf::Arena>();
            proto::TaskMsg* taskMsg = google::protobuf::Arena::CreateMessage<proto::TaskMsg>(gArena.get());

            if (!taskMsg->ParseFromArray(reqData, reqSize) ||
                !taskMsg->IsInitialized()) {
                reportError("Failed to decode TaskMsg on resource db=" + ru.db() +
                            " chunkId=" + to_string(ru.chunk()));
                return;
            }

            QSERV_LOGCONTEXT_QUERY_JOB(taskMsg->queryid(), taskMsg->jobid());

            if (!taskMsg->has_db() || !taskMsg->has_chunkid()
                || (ru.db()    != taskMsg->db())
                || (ru.chunk() != taskMsg->chunkid())) {
                reportError("Mismatched db/chunk in TaskMsg on resource db=" + ru.db() +
                        " chunkId=" + to_string(ru.chunk()));
                return;
            }

            // Now that the request is decoded (successfully or not), release the
            // xrootd request buffer. To avoid data races, this must happen before
            // the task is handed off to another thread for processing, as there is a
            // reference to this SsiRequest inside the reply channel for the task,
            // and after the call to BindRequest.
            auto sendChannelBase = make_shared<wbase::SendChannel>(shared_from_this());
            auto sendChannel = wbase::SendChannelShared::create(sendChannelBase, _transmitMgr);
            auto tasks = wbase::Task::createTasks(*taskMsg, sendChannel, gArena, resourceLock);

            ReleaseRequestBuffer();
            t.start();
            _processor->processTasks(tasks); // Queues tasks to be run later.
            t.stop();
            LOGS(_log, LOG_LVL_DEBUG, "Enqueued TaskMsg for " << ru << " in "
                                      << t.getElapsed() << " seconds");
            break;
        }
        case ResourceUnit::WORKER: {

            LOGS(_log, LOG_LVL_DEBUG, "Parsing WorkerCommand for resource=" << _resourceName);

            /// Pick off UberJobs
            //&&& add a timer to check that ParseFromArray fails quickly when not an UberJobMsg
            //&&& TODO:UJ UberJob breaks _resourceMonitor, it may be possible for _handleUberJob
            //&&&      to fix this, but only when the chunk resource name is known.
            {
                auto gArena = make_shared<google::protobuf::Arena>(); // TODO:UJ this arena should be used for parsing all WorkerCommands
                proto::UberJobMsg* uberJobMsg = google::protobuf::Arena::CreateMessage<proto::UberJobMsg>(gArena.get());
                if (uberJobMsg->ParseFromArray(reqData, reqSize) && uberJobMsg->IsInitialized()) {
                    _handleUberJob(uberJobMsg, gArena);
                    ReleaseRequestBuffer();
                    return;
                }
            }

            wbase::WorkerCommand::Ptr const command = parseWorkerCommand(reqData, reqSize);
            if (not command) return;

            // The buffer must be released before submitting commands for
            // further processing.
            ReleaseRequestBuffer();
            _processor->processCommand(command);    // Queues the command to be run later.

            LOGS(_log, LOG_LVL_DEBUG, "Enqueued WorkerCommand for resource=" << _resourceName);

            break;
        }
        default:
            reportError("Unexpected unit type '" + to_string(ru.unitType()) +
                        "', resource name: " + _resourceName);
            break;
    }

    // Note that upon exit the _finMutex will be unlocked allowing Finished()
    // to actually do something once everything is actually setup.
}


uint64_t SsiRequest::getSeq() const {
    if (_stream == nullptr) return 0;
    return _stream->getSeq();
}



wbase::WorkerCommand::Ptr SsiRequest::parseWorkerCommand(char const* reqData, int reqSize) {

    wbase::SendChannel::Ptr const sendChannel =
        make_shared<wbase::SendChannel>(shared_from_this());

    wbase::WorkerCommand::Ptr command;

    try {

        // reqData has the entire request, so we can unpack it without waiting for
        // more data.
        proto::FrameBufferView view(reqData, reqSize);

        proto::WorkerCommandH header;
        view.parse(header);

        LOGS(_log, LOG_LVL_DEBUG, "WorkerCommandH: command=" <<
             proto::WorkerCommandH_Command_Name(header.command()) <<
             " resource=" << _resourceName);

        switch (header.command()) {
            case proto::WorkerCommandH::TEST_ECHO: {

                proto::WorkerCommandTestEchoM echo;
                view.parse(echo);

                command = make_shared<wpublish::TestEchoCommand>(
                                sendChannel,
                                echo.value());
                break;
            }
            case proto::WorkerCommandH::ADD_CHUNK_GROUP:
            case proto::WorkerCommandH::REMOVE_CHUNK_GROUP: {

                proto::WorkerCommandChunkGroupM group;
                view.parse(group);

                vector<string> dbs;
                for (int i = 0, num = group.dbs_size(); i < num; ++i)
                    dbs.push_back(group.dbs(i));

                int  const chunk = group.chunk();
                bool const force = group.force();

                if (header.command() == proto::WorkerCommandH::ADD_CHUNK_GROUP)
                    command = make_shared<wpublish::AddChunkGroupCommand>(
                                    sendChannel,
                                    _chunkInventory,
                                    _mySqlConfig,
                                    chunk,
                                    dbs);
                else
                    command = make_shared<wpublish::RemoveChunkGroupCommand>(
                                    sendChannel,
                                    _chunkInventory,
                                    _resourceMonitor,
                                    _mySqlConfig,
                                    chunk,
                                    dbs,
                                    force);
                break;

            }
            case proto::WorkerCommandH::UPDATE_CHUNK_LIST: {

                proto::WorkerCommandUpdateChunkListM message;
                view.parse(message);

                if (message.rebuild())
                    command = make_shared<wpublish::RebuildChunkListCommand>(
                                    sendChannel,
                                    _chunkInventory,
                                    _mySqlConfig,
                                    message.reload());
                else
                    command = make_shared<wpublish::ReloadChunkListCommand>(
                                    sendChannel,
                                    _chunkInventory,
                                    _mySqlConfig);
                break;
            }
            case proto::WorkerCommandH::GET_CHUNK_LIST: {

                command = make_shared<wpublish::GetChunkListCommand>(
                                    sendChannel,
                                    _chunkInventory,
                                    _resourceMonitor);
                break;
            }
            case proto::WorkerCommandH::SET_CHUNK_LIST: {

                proto::WorkerCommandSetChunkListM message;
                view.parse(message);

                vector<wpublish::SetChunkListCommand::Chunk> chunks;
                for (int i = 0, num = message.chunks_size(); i < num; ++i) {
                    chunks.push_back(
                        wpublish::SetChunkListCommand::Chunk{
                            message.chunks(i).db(),
                            message.chunks(i).chunk()
                        }
                    );
                }
                vector<string> databases;
                for (int i = 0, num = message.databases_size(); i < num; ++i) {
                    databases.push_back(message.databases(i));
                }
                bool const force = message.force();

                command = make_shared<wpublish::SetChunkListCommand>(
                                    sendChannel,
                                    _chunkInventory,
                                    _resourceMonitor,
                                    _mySqlConfig,
                                    chunks,
                                    databases,
                                    force);
                break;
            }
            case proto::WorkerCommandH::GET_STATUS: {
                command = make_shared<wpublish::GetStatusCommand>(
                                sendChannel,
                                _processor,
                                _resourceMonitor);
                break;
            }
            default:
                reportError("Unsupported command " +
                            proto::WorkerCommandH_Command_Name(header.command()) +
                            " found in WorkerCommandH on worker resource=" + _resourceName);
                break;
        }

    } catch (proto::FrameBufferError const& ex) {
        reportError("Failed to decode a worker management command, error: " +
                    string(ex.what()));
    }
    return command;
}

/// Called by SSI to free resources.
void SsiRequest::Finished(XrdSsiRequest& req, XrdSsiRespInfo const& rinfo, bool cancel) { // Step 8
    // This call is sync (blocking).
    // client finished retrieving response, or cancelled.
    // release response resources (e.g. buf)
    // But first we must make sure that request setup completed (i.e execute()) by
    // locking _finMutex.
    {
        lock_guard<mutex> finLock(_finMutex);
        // Clean up _stream if it exists and don't add anything new to it either.
        _reqFinished = true;
        if (_stream != nullptr) {
            _stream->clearMsgs();
        }
    }

    auto keepAlive = freeSelfKeepAlive();

    // No buffers allocated, so don't need to free.
    // We can release/unlink the file now
    const char* type = "";
    switch(rinfo.rType) {
    case XrdSsiRespInfo::isNone: type = "type=isNone"; break;
    case XrdSsiRespInfo::isData: type = "type=isData"; break;
    case XrdSsiRespInfo::isError: type = "type=isError"; break;
    case XrdSsiRespInfo::isFile: type = "type=isFile"; break;
    case XrdSsiRespInfo::isStream: type = "type=isStream"; break;
    case XrdSsiRespInfo::isHandle: type = "type=isHandle"; break;
    }

    // Decrement the counter of the database/chunk resources in use
    ResourceUnit ru(_resourceName);

    // We can't do much other than close the file.
    // It should work (on linux) to unlink the file after we open it, though.
    // With the optimizer on '-Og', there was a double free for a SsiRequest.
    // The likely cause could be keepAlive being optimized out for being unused.
    // The problem has not reoccurred since adding keepAlive to the following
    // comment, but having code depend on a comment line is ugly in its own way.
    LOGS(_log, LOG_LVL_DEBUG, "RequestFinished " << type << " " << keepAlive.use_count());
}

bool SsiRequest::reply(char const* buf, int bufLen) {
    Status s = SetResponse(buf, bufLen);
    if (s != XrdSsiResponder::wasPosted) {
        LOGS(_log, LOG_LVL_ERROR, "DANGER: Couldn't post response of length=" << bufLen);
        return false;
    }
    return true;
}

bool SsiRequest::replyError(string const& msg, int code) {
    Status s = SetErrResponse(msg.c_str(), code);
    if (s != XrdSsiResponder::wasPosted) {
        LOGS(_log, LOG_LVL_ERROR, "DANGER: Couldn't post error response " << msg);
        return false;
    }
    return true;
}


bool SsiRequest::replyFile(int fd, long long fSize) {
    util::Timer t;
    t.start();
    Status s = SetResponse(fSize, fd);
    if (s == XrdSsiResponder::wasPosted) {
        LOGS(_log, LOG_LVL_DEBUG, "file posted ok");
    } else {
        if (s == XrdSsiResponder::notActive) {
            LOGS(_log, LOG_LVL_ERROR, "DANGER: Couldn't post response file of length="
                 << fSize << ", responder not active.");
        } else {
            LOGS(_log, LOG_LVL_ERROR, "DANGER: Couldn't post response file of length=" << fSize);
        }
        replyError("Internal error posting response file", 1);
        t.stop();
        return false; // call must handle everything else.
    }
    t.stop();
    LOGS(_log, LOG_LVL_DEBUG, "replyFile took " << t.getElapsed() << " seconds");
    return true;
}


bool SsiRequest::replyStream(StreamBuffer::Ptr const& sBuf, bool last) {
    LOGS(_log, LOG_LVL_INFO, "replyStream, checking stream size=" << sBuf->getSize() << " last=" << last); // &&& debug

    // Normally, XrdSsi would call Recycle() when it is done with sBuf, but if this function
    // returns false, then it must call Recycle(). Otherwise, the scheduler will likely
    // wedge waiting for the buffer to be released.
    lock_guard<mutex> finLock(_finMutex);
    if (_reqFinished) {
        // Finished() was called, give up.
        LOGS(_log, LOG_LVL_ERROR, "replyStream called after reqFinished.");
        sBuf->Recycle();
        return false;
    }
    // Create a stream if needed.
    if (!_stream) {
        _stream = make_shared<ChannelStream>();
        if (SetResponse(_stream.get()) != XrdSsiResponder::Status::wasPosted) {
            LOGS(_log, LOG_LVL_WARN, "SetResponse stream failed, calling Recycle for sBuf");
            // SetResponse return value indicates XrdSsi wont call Recycle().
            sBuf->Recycle();
            return false;
        }
    } else if (_stream->closed()) {
        // XrdSsi isn't going to call Recycle if we wind up here.
        LOGS(_log, LOG_LVL_ERROR, "Logic error SsiRequest::replyStream called with stream closed.");
        sBuf->Recycle();
        return false;
    }
    LOGS(_log, LOG_LVL_WARN, "&&& SsiRequest::replyStream _stream seq=" << getSeq());
    // XrdSsi or Finished() will call Recycle().
    _stream->append(sBuf, last);
    return true;
}


bool SsiRequest::sendMetadata(const char *buf, int blen) {
    Status stat = SetMetadata(buf, blen);
    switch (stat) {
    case XrdSsiResponder::wasPosted:
        return true;
    case XrdSsiResponder::notActive:
        LOGS(_log, LOG_LVL_ERROR, "failed to setMetadata notActive");
        break;
    case XrdSsiResponder::notPosted:
        LOGS(_log, LOG_LVL_ERROR, "failed to setMetadata notPosted blen=" << blen);
        break;
    default:
        LOGS(_log, LOG_LVL_ERROR, "failed to setMetadata unkown state blen=" << blen);
    }
    return false;
}


SsiRequest::Ptr SsiRequest::freeSelfKeepAlive() {
    Ptr keepAlive = std::move(_selfKeepAlive);
    return keepAlive;
}


void SsiRequest::_handleUberJob(proto::UberJobMsg* uberJobMsg,
                                shared_ptr<google::protobuf::Arena> const& gArena) {

    // TODO:UJ if this is slow, it can be moved into a separate thread.
    //         Check the purpose of _finMutex, as it is locked before this is called.
    qmeta::CzarId czarId = uberJobMsg->czarid();
    QueryId qId = uberJobMsg->queryid();
    uint32_t uberJobId = uberJobMsg->uberjobid();
    uint32_t magicNumber = uberJobMsg->magicnumber();

    if (magicNumber != qdisp::UberJob::getMagicNumber()) {
        throw Bug("UberJob incorrect magic number");
    }
    LOGS(_log, LOG_LVL_INFO, "&&& _handleUberJob qId=" << qId << " czarId=" << czarId);

    std::vector<int> jobIds;

    int tSize = uberJobMsg->taskmsgs_size();
    if (tSize == 0) {
        return;
    }

    auto sendChannelBase = make_shared<wbase::SendChannel>(shared_from_this());
    auto sendChannel = wbase::SendChannelShared::create(sendChannelBase, _transmitMgr);

    // Make a Task for each TaskMsg in the UberJobMsg
    vector<wbase::Task::Ptr> tasks;
    LOGS(_log, LOG_LVL_INFO, "&&& SsiRequest::_hUJ  tSize=" << tSize);
    for (int j=0; j < tSize; ++j) {
        //LOGS(_log, LOG_LVL_INFO, "&&& SsiRequest::_hUJ j=" << j);
        proto::TaskMsg const& taskMsg = uberJobMsg->taskmsgs(j);

        if (!taskMsg.has_db() || !taskMsg.has_chunkid())  {
            reportError("Missing db/chunk in TaskMsg on resource db=" + taskMsg.db() +
                    " chunkId=" + to_string(taskMsg.chunkid()));
            return;
        }
        int jobId = taskMsg.jobid();
        jobIds.push_back(jobId);
        string db = taskMsg.db();
        int chunkId = taskMsg.chunkid();
        string resourcePath = ResourceUnit::makePath(chunkId, db);
        //LOGS(_log, LOG_LVL_INFO, "&&& SsiRequest::_hUJ resourcePath=" << resourcePath);
        ResourceUnit ru(resourcePath);
        if (ru.db() != db || ru.chunk() != chunkId) {
            throw Bug("resource path didn't match ru");
        }
        auto resourceLock = std::make_shared<wpublish::ResourceMonitorLock>(*(_resourceMonitor.get()), resourcePath);

        // If the query uses subchunks, the taskMsg will return multiple Tasks. Otherwise, one task.
        //LOGS(_log, LOG_LVL_INFO, "&&& SsiRequest::_hUJ creating tasks");
        auto nTasks = wbase::Task::createTasks(taskMsg, sendChannel, gArena, resourceLock);
        // Move nTasks into tasks
        tasks.insert(tasks.end(), std::make_move_iterator(nTasks.begin()),
                     std::make_move_iterator(nTasks.end()));
    }

    // make a log message showing which jobs in the uberjob
    string qIdStr = to_string(qId);
    string logMsg = "uberJob={QID," + qIdStr + "#" + to_string(uberJobId);
    logMsg += "} channel=" + to_string(sendChannel->getId());
    logMsg += " taskC=" + to_string(sendChannel->getTaskCount());
    logMsg += " lastC=" + to_string(sendChannel->getLastCount());
    logMsg += " jobs=(";
    bool firstMsg = true;
    for (auto const& jobId:jobIds) {
        if (!firstMsg) {
            logMsg += ",";
            firstMsg = false;
        }
        logMsg += "{QID," + qIdStr + "#" + to_string(jobId) + "}";
    }
    logMsg += ")";
    LOGS(_log, LOG_LVL_INFO, logMsg);

    _processor->processTasks(tasks); // Queues tasks to be run later.

}


}}} // namespace
