// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 AURA/LSST.
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
 * @author John Gates, SLAC
 */

// System headers
#include <atomic>
#include <cstddef>
#include <errno.h>
#include <mutex>
#include <string>
#include <stdlib.h>
#include <thread>
#include <unistd.h>

// Third party headers
#include "XrdSsi/XrdSsiErrInfo.hh"
#include "XrdSsi/XrdSsiResponder.hh"
#include "XrdSsi/XrdSsiStream.hh"

// LSST headers
#include "lsst/log/Log.h"
#include "proto/ProtoHeaderWrap.h"
#include "proto/worker.pb.h"
#include "util/StringHash.h"
#include "util/threadSafe.h"

// Qserv headers
#include "qdisp/Executive.h"
#include "qdisp/QueryRequest.h"
#include "qdisp/XrdSsiMocks.h"

using namespace std;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.XrdSsiMock");

lsst::qserv::util::FlagNotify<bool> _go(true);

std::atomic<int> canCount(0);
std::atomic<int> finCount(0);
std::atomic<int> reqCount(0);
std::atomic<int> totCount(0);

bool _aOK = true;

enum RespType {RESP_BADREQ, RESP_DATA, RESP_ERROR, RESP_ERRNR,
               RESP_STREAM, RESP_STRERR};

class Agent : public XrdSsiResponder, public XrdSsiStream {
public:

    void Finished(XrdSsiRequest&        rqstR,
                  XrdSsiRespInfo const& rInfo,
                  bool cancel) override {
        const char *how = (cancel ? " cancelled" : "");
        LOGS(_log, LOG_LVL_DEBUG, "Finished: " << _rNum
                                  << " rName=" << _rName << how);
        _rrMutex.lock();
        UnBindRequest();
        if (cancel) canCount++;
        finCount++;
        _isFIN = true;
        if (_active) {
           _rrMutex.unlock();
        } else {
           _rrMutex.unlock();
           delete this;
        }
    }

    void Reply(RespType rType) {
        _go.wait(true);

        // We may have been cancelled before being able to reply
        //
        if (_isCancelled(true)) return; // we are locked now

        // Do requested reply
        //
        switch(rType) {
             case RESP_DATA:
                  _ReplyData();
                  break;
             case RESP_ERRNR:
                  _reqP->doNotRetry();
                  // Fallthrough   [[fallthrough]];
             case RESP_ERROR:
                  _ReplyError();
                  break;
             case RESP_STRERR:
                  _noData = true;
                  _reqP->doNotRetry();  // Kill retries on stream errors
                  _ReplyStream();
                  break;
             default:
                  _reqP->doNotRetry();
                  _ReplyError("Bad mock request!", 13);
                  break;
        }
        _isCancelled(false);
    }

    bool SetBuff(XrdSsiErrInfo& eRef, char* buff, int  blen) override {

        // We may have been cancelled while waiting
        //
        if (_isCancelled(true)) return false;
        std::thread (&Agent::_StrmResp, this, &eRef, buff, blen).detach();
        _rrMutex.unlock();
        return true;
    }

    Agent(lsst::qserv::qdisp::QueryRequest* rP,
          std::string const& rname, int rnum) :
         XrdSsiStream(XrdSsiStream::isPassive),
         _reqP(rP), _rName(rname), _rNum(rnum), _noData(true),
         _isFIN(false), _active(true) {

        // Initialize a null message we will return as a response
        //
        lsst::qserv::proto::ProtoHeader* ph =
                                         new lsst::qserv::proto::ProtoHeader;
        ph->set_protocol(2);
        ph->set_size(0);
        ph->set_md5(std::string("d41d8cd98f00b204e9800998ecf8427"));
        ph->set_wname("localhost");
        ph->set_largeresult(false);
        std::string pHdrString;
        ph->SerializeToString(&pHdrString);
        _msgBuf = lsst::qserv::proto::ProtoHeaderWrap::wrap(pHdrString);
        _bOff = 0;
        _bLen = _msgBuf.size();
    }

    ~Agent() {}

private:

    bool _isCancelled(bool activate) {
        if (activate) _rrMutex.lock();
        if (_isFIN) {
           _rrMutex.unlock();
           delete this;
           return true;
        }
        _active = activate;
        if (!activate) _rrMutex.unlock();
        return false;
    }

    void _ReplyData() {
        _rspBuf = "MockResponse";
        SetResponse(_rspBuf.data(), _rspBuf.size());
    }

    void _ReplyError(const char* eMsg="Mock Request Ignored!", int eNum=17) {
        SetErrResponse(eMsg, eNum);
    }

    void _ReplyStream() {SetResponse(this);}

    void _StrmResp(XrdSsiErrInfo* eP, char* buff, int blen) {
        std::cerr<<"Stream: cleint asks for " <<blen <<" bytes, have "
                 <<_bLen <<'\n' <<std::flush;
        bool last;

        // Check for cancellation while we were waiting
        //
        if (_isCancelled(true)) return;

        // Either reply with an error or actual data
        //
        if (_noData) {
            blen = -17;
            last = true;
            eP->Set("Mock stream error!", 17);
        } else {
            if (_bLen <= blen) {
                memcpy(buff, _msgBuf.data()+_bOff, _bLen);
                blen = _bLen; _bLen = 0;
                last = true;
            } else {
                memcpy(buff, _msgBuf.data()+_bOff,  blen);
                _bOff += blen; _bLen -= blen;
                last = false;
            }
        }
        _reqP->ProcessResponseData(*eP, buff, blen, last);
        _isCancelled(false);
    }

    std::recursive_mutex _rrMutex;
    lsst::qserv::qdisp::QueryRequest* _reqP;
    std::string _rName;
    std::string _rspBuf;
    std::string _msgBuf;
    int         _bOff;
    int         _bLen;
    int         _rNum;
    bool        _noData;
    bool        _isFIN;
    bool        _active;
};
}

namespace lsst {
namespace qserv {
namespace qdisp {

std::string XrdSsiServiceMock::_myRName;

int  XrdSsiServiceMock::getCount() {return totCount;}

int  XrdSsiServiceMock::getCanCount() {return canCount;}

int  XrdSsiServiceMock::getFinCount() {return finCount;}

int  XrdSsiServiceMock::getReqCount() {return reqCount;}

bool XrdSsiServiceMock::isAOK() {return _aOK;}

void XrdSsiServiceMock::Reset() {
    canCount = 0;
    finCount = 0;
    reqCount = 0;
}

void XrdSsiServiceMock::setGo(bool go) {_go.exchangeNotify(go);}

void XrdSsiServiceMock::ProcessRequest(XrdSsiRequest  &reqRef,
                                       XrdSsiResource &resRef) {
    static struct {const char *cmd; RespType rType;} reqTab[] = {
           {"respdata",   RESP_DATA},
           {"resperror",  RESP_ERROR},
           {"resperrnr",  RESP_ERRNR},
           {"respstream", RESP_STREAM},
           {"respstrerr", RESP_STRERR},
           {0, RESP_BADREQ}
    };

    int reqNum = totCount++;

    // Check if we should verify the resource name
    //
    if (_myRName.size() && _myRName != resRef.rName) {
       LOGS_DEBUG("Expected rname " <<_myRName <<" got " <<resRef.rName
                  <<" from req #" <<reqNum);
       _aOK = false;
    }

    // Get the query request object for this request and process it.
    //
    QueryRequest * r = dynamic_cast<QueryRequest *>(&reqRef);
    if (r) {
        Agent* aP = new Agent(r, resRef.rName, reqNum);
        RespType doResp;
        aP->BindRequest(reqRef);

        // Get the request data and setup to handle request. Make sure the
        // request string is null terminated (it should be).
        //
        std::string reqStr;
        int reqLen;
        const char *reqData = r->GetRequest(reqLen);
        if (reqData != nullptr) reqStr.assign(reqData, reqLen);
        reqData = reqStr.c_str();

        // Convert request to response type
        //
        int i = 0;
        while(reqTab[i].cmd && strcmp(reqTab[i].cmd, reqData)) i++;
        if (reqTab[i].cmd) {
           doResp = reqTab[i].rType;
        } else {
           LOGS_DEBUG("Unknown request '" <<reqData <<"' from req #" <<reqNum);
           _aOK = false;
           doResp = RESP_BADREQ;
        }

        // Release the request buffer (typically a no-op)
        //
        if (reqLen != 0) r->ReleaseRequestBuffer();

        // Schedule a response
        //
        reqCount++;
        std::thread (&Agent::Reply, aP, doResp).detach();
    }
}

}}} // namespace
