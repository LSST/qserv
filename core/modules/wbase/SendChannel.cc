// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2018 LSST Corporation.
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
#include "wbase/SendChannel.h"

// System headers
#include <functional>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <vector>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "proto/ProtoHeaderWrap.h"
#include "global/LogContext.h"
#include "util/common.h"
#include "util/Timer.h"
#include "xrdsvc/SsiRequest.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wbase.SendChannel");
}

using namespace std;

namespace lsst {
namespace qserv {
namespace wbase {

/// NopChannel is a NOP implementation of SendChannel for development and
/// debugging code without an XrdSsi channel.
class NopChannel : public SendChannel {
public:
    NopChannel() {}

    bool send(char const* buf, int bufLen) override {
        cout << "NopChannel send(" << (void*) buf
             << ", " << bufLen << ");\n";
        return !isDead();
    }

    bool sendError(string const& msg, int code) override {
        if (kill("NopChannel")) return false;
        cout << "NopChannel sendError(\"" << msg
             << "\", " << code << ");\n";
        return true;
    }
    bool sendFile(int fd, Size fSize) override {
        cout << "NopChannel sendFile(" << fd
             << ", " << fSize << ");\n";
        return !isDead();
    }
    bool sendStream(xrdsvc::StreamBuffer::Ptr const& sBuf, bool last) override {
        cout << "NopChannel sendStream(" << (void*) sBuf.get()
             << ", " << (last ? "true" : "false") << ");\n";
        return !isDead();
    }
};


SendChannel::Ptr SendChannel::newNopChannel() {
    return make_shared<NopChannel>();
}


/// StringChannel is an almost-trivial implementation of a SendChannel that
/// remembers what it has received.
class StringChannel : public SendChannel {
public:
    StringChannel(string& dest) : _dest(dest) {}

    bool send(char const* buf, int bufLen) override {
        if (isDead()) return false;
        _dest.append(buf, bufLen);
        return true;
    }

    bool sendError(string const& msg, int code) override {
        if (kill("StringChannel")) return false;
        ostringstream os;
        os << "(" << code << "," << msg << ")";
        _dest.append(os.str());
        return true;
    }

    bool sendFile(int fd, Size fSize) override {
        if (isDead()) return false;
        vector<char> buf(fSize);
        Size remain = fSize;
        while(remain > 0) {
            Size frag = ::read(fd, buf.data(), remain);
            if (frag < 0) {
                cout << "ERROR reading from fd during "
                     << "StringChannel::sendFile(" << "," << fSize << ")";
                return false;
            } else if (frag == 0) {
                cout << "ERROR unexpected 0==read() during "
                     << "StringChannel::sendFile(" << "," << fSize << ")";
                return false;
            }
            _dest.append(buf.data(), frag);
            remain -= frag;
        }
        release();
        return true;
    }

    bool sendStream(xrdsvc::StreamBuffer::Ptr const& sBuf, bool last) override {
        if (isDead()) return false;
        char const* buf = sBuf->data;
        size_t bufLen = sBuf->getSize();
        _dest.append(buf, bufLen);
        cout << "StringChannel sendStream(" << (void*) buf
             << ", " << bufLen << ", "
             << (last ? "true" : "false") << ");\n";
        return true;
    }

private:
    string& _dest;
};


SendChannel::Ptr SendChannel::newStringChannel(string& d) {
    return make_shared<StringChannel>(d);

}


/// This is the standard definition of SendChannel which actually does something!
/// We vector responses posted to SendChannel via the tightly bound SsiRequest
/// object as this object knows how to effect Ssi responses.
///
bool SendChannel::send(char const* buf, int bufLen) {
    if (isDead()) return false;
    if (_ssiRequest->reply(buf, bufLen)) return true;
    kill("SendChannel::send");
    return false;
}


bool SendChannel::sendError(string const& msg, int code) {
    // Kill this send channel. If it wasn't already dead, send the error.
    if (kill("SendChannel::sendError")) return false;
    if (_ssiRequest->replyError(msg.c_str(), code)) return true;
    return false;
}


bool SendChannel::sendFile(int fd, Size fSize) {
    if (!isDead()) {
        if (_ssiRequest->replyFile(fSize, fd)) return true;
    }
    kill("SendChannel::sendFile");
    release();
    return false;
}


bool SendChannel::kill(std::string const& note) {
    bool oldVal = _dead.exchange(true);
    if (!oldVal && !_destroying) {
        LOGS(_log, LOG_LVL_WARN, "SendChannel first kill call " << note);
    }
    return oldVal;
}


bool SendChannel::isDead() {
    if (_dead) return true;
    if (_ssiRequest == nullptr) return true;
    if (_ssiRequest->isFinished()) kill("SendChannel::isDead");
    return _dead;
}


bool SendChannel::sendStream(xrdsvc::StreamBuffer::Ptr const& sBuf, bool last) {
    if (isDead()) return false;
    if (_ssiRequest->replyStream(sBuf, last)) return true;
    LOGS(_log, LOG_LVL_ERROR, "_ssiRequest->replyStream failed, killing.");
    kill("SendChannel::sendStream");
    return false;
}


bool SendChannel::setMetadata(const char *buf, int blen) {
    if (isDead()) return false;
    if (_ssiRequest->sendMetadata(buf, blen)) return true;
    return false;
}


uint64_t SendChannel::getSeq() const {
    if (_ssiRequest == nullptr) return 0;
    return _ssiRequest->getSeq();
}


}}} // namespace lsst::qserv::wbase

