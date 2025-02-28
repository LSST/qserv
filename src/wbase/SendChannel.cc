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

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.wbase.SendChannel");
}

using namespace std;

namespace lsst::qserv::wbase {

/// NopChannel is a NOP implementation of SendChannel for development and
/// debugging code without an XrdSsi channel.
class NopChannel : public SendChannel {
public:
    NopChannel() {}

    bool send(char const* buf, int bufLen) override {
        cout << "NopChannel send(" << (void*)buf << ", " << bufLen << ");\n";
        return !isDead();
    }
};

SendChannel::Ptr SendChannel::newNopChannel() { return std::shared_ptr<NopChannel>(new NopChannel()); }

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

private:
    string& _dest;
};

SendChannel::Ptr SendChannel::newStringChannel(string& d) {
    return std::shared_ptr<StringChannel>(new StringChannel(d));
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
    return _dead;
}

}  // namespace lsst::qserv::wbase
