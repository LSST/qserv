// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015 LSST Corporation.
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
#ifndef LSST_QSERV_XRDSVC_SSIREQUEST_H
#define LSST_QSERV_XRDSVC_SSIREQUEST_H

// System headers
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

// Third-party headers
#include "XrdSsi/XrdSsiResponder.hh"

// Qserv headers
#include "global/ResourceUnit.h"
#include "mysql/MySqlConfig.h"
#include "wbase/Task.h"
#include "wbase/WorkerCommand.h"
#include "wpublish/ChunkInventory.h"
#include "xrdsvc/StreamBuffer.h"


// Forward declarations
class XrdSsiService;


namespace google {
namespace protobuf {
class Arena;
}}

namespace lsst {
namespace qserv {
namespace wbase {
struct MsgProcessor;
}
namespace wcontrol {
class TransmitMgr;
}
namespace wpublish {
class ResourceMonitor;
}}}

namespace lsst {
namespace qserv {
namespace xrdsvc {

class ChannelStream;
class StreamBuffer;

/// An implementation of XrdSsiResponder that is used by SsiService to provide
/// qserv worker services. The SSI interface encourages such an approach, and
/// object lifetimes are explicitly stated in the documentation which we
/// adhere to using BindRequest() and UnBindRequest() responder methods.
class SsiRequest
    :   public XrdSsiResponder,
        public std::enable_shared_from_this<SsiRequest> {

public:

    // Smart pointer definitions

    typedef std::shared_ptr<ResourceUnit::Checker> ValidatorPtr;
    typedef std::shared_ptr<SsiRequest>            Ptr;

    /// Use factory to ensure proper construction for enable_shared_from_this.
    static SsiRequest::Ptr newSsiRequest (
            std::string const&                               rname,
            std::shared_ptr<wpublish::ChunkInventory> const& chunkInventory,
            std::shared_ptr<wbase::MsgProcessor> const&      processor,
            mysql::MySqlConfig const&                        mySqlConfig,
            std::shared_ptr<wcontrol::TransmitMgr> const&    transmitMgr) {
        auto req = SsiRequest::Ptr(new SsiRequest(rname,
                                                  chunkInventory,
                                                  processor,
                                                  mySqlConfig,
                                                  transmitMgr));
        req->_selfKeepAlive = req;
        return req;
    }

    virtual ~SsiRequest();

    void execute(XrdSsiRequest& req);

    /**
     * Implements the virtual method defined in the base class
     *
     * @see XrdSsiResponder::Finished
     */
    void Finished(XrdSsiRequest&        req,
                  XrdSsiRespInfo const& rinfo,
                  bool                  cancel=false) override;

    bool isFinished() { return _reqFinished; }

    bool reply(char const* buf, int bufLen);
    bool replyError(std::string const& msg, int code);
    bool replyFile(int fd, long long fSize);
    bool replyStream(StreamBuffer::Ptr const& sbuf, bool last);

    bool sendMetadata(const char *buf, int blen);

    uint64_t getSeq() const;

    /// Call this to allow object to die after it truly is no longer needed.
    /// i.e. It is know Finish() will not be called.
    /// NOTE: It is important that any non-static SsiRequest member
    /// function make a local copy of the returned pointer so that
    /// SsiRequest is guaranteed to live to the end of
    /// the function call.
    Ptr freeSelfKeepAlive();

private:

    /// Constructor (called by SsiService)
    SsiRequest(std::string const&                               rname,
               std::shared_ptr<wpublish::ChunkInventory> const& chunkInventory,
               std::shared_ptr<wbase::MsgProcessor> const&      processor,
               mysql::MySqlConfig const&                        mySqlConfig,
               std::shared_ptr<wcontrol::TransmitMgr> const&    transmitMgr)
        :   _chunkInventory(chunkInventory),
            _validator(_chunkInventory->newValidator()),
            _processor(processor),
            _resourceName(rname),
            _mySqlConfig(mySqlConfig),
            _transmitMgr(transmitMgr){
    }
    
    /// For internal error reporting
    void reportError (std::string const& errStr);

    /**
     * Parse a Protobuf request into the corresponding command
     * 
     * @param reqData - pointer to the Protobuf data buffer
     * @param reqSize - size of the data buffer
     * 
     * @return smart pointer to the corresponding command object or nullptr if failed
     */
    wbase::WorkerCommand::Ptr parseWorkerCommand(char const* reqData, int reqSize);

private:
    /// Convert the UberJobMsg into its component TaskMsgs and run them.
    /// uberJobMsg must be in memory held by gArena.
    void _handleUberJob(proto::UberJobMsg* uberJobMsg,
                        std::shared_ptr<google::protobuf::Arena> const& gArena);

    /// Counters of the database/chunk requests which are being used
    static std::shared_ptr<wpublish::ResourceMonitor> _resourceMonitor;

    std::shared_ptr<wpublish::ChunkInventory> _chunkInventory;

    ValidatorPtr                         _validator;    ///< validates request against what's available
    std::shared_ptr<wbase::MsgProcessor> _processor;    ///< actual msg processor

    std::mutex  _finMutex;  ///< Protects execute() from Finish(), _finished, and _stream
    std::atomic<bool> _reqFinished{false};  ///< set to true when Finished called
    std::string _resourceName; ///< chunk identifier

    std::shared_ptr<ChannelStream> _stream;
    mysql::MySqlConfig const _mySqlConfig;
    std::shared_ptr<wcontrol::TransmitMgr> _transmitMgr; ///< limits transmits to czars.

    /// Make sure this object exists until Finish() is called.
    /// Make a local copy before calling reset() within and non-static member function.
    Ptr _selfKeepAlive;
};

}}} // namespace

#endif // LSST_QSERV_XRDSVC_SSIREQUEST_H
