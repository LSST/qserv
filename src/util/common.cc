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
// Class header
#include "util/common.h"

// Standard headers
#include <netdb.h>
#include <stdexcept>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

// Third-party headers
#include "boost/asio.hpp"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.util.common");
}

namespace lsst::qserv::util {

string get_current_host_fqdn(bool all) {
    // Get the short name of the current host first.
    boost::system::error_code ec;
    string const hostname = boost::asio::ip::host_name(ec);
    if (ec.value() != 0) {
        throw runtime_error("Registry::" + string(__func__) +
                            " boost::asio::ip::host_name failed: " + ec.category().name() + string(":") +
                            to_string(ec.value()) + "[" + ec.message() + "]");
    }

    // Get the host's FQDN(s)
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;     /* either IPV4 or IPV6 */
    hints.ai_socktype = SOCK_STREAM; /* IP */
    hints.ai_flags = AI_CANONNAME;   /* canonical name */
    hints.ai_canonname = NULL;
    struct addrinfo* info;
    while (true) {
        int const retCode = getaddrinfo(hostname.data(), "http", &hints, &info);
        if (retCode == 0) break;
        if (retCode == EAI_AGAIN) continue;
        throw runtime_error("Registry::" + string(__func__) +
                            " getaddrinfo failed: " + gai_strerror(retCode));
    }
    string fqdn;
    for (struct addrinfo* p = info; p != NULL; p = p->ai_next) {
        if (p->ai_canonname == NULL) {
            throw runtime_error("Registry::" + string(__func__) +
                                " getaddrinfo failed: ai_canonname is NULL");
        }
        if (!fqdn.empty()) {
            if (!all) break;
            fqdn += ",";
        }
        fqdn += p->ai_canonname;
    }
    freeaddrinfo(info);
    return fqdn;
}

std::string getCurrentHostFqdnBlocking() {
    while (true) {
        try {
            string result = util::get_current_host_fqdn();
            if (!result.empty()) {
                return result;
            }
            LOGS(_log, LOG_LVL_ERROR, __func__ << " Empty response for the worker hosts's FQDN.");
        } catch (std::runtime_error const& ex) {
            LOGS(_log, LOG_LVL_ERROR, __func__ << " Failed to obtain worker hosts's FQDN, ex: " << ex.what());
        }
        sleep(1);
    }
}

}  // namespace lsst::qserv::util
