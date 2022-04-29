/*
 * LSST Data Management System
 * Copyright 2017 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */

#ifndef LSST_QSERV_QHTTP_AJAXENDPOINT_H
#define LSST_QSERV_QHTTP_AJAXENDPOINT_H

// System headers
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Local headers
#include "qhttp/Response.h"

namespace lsst { namespace qserv { namespace qhttp {

class Server;

class AjaxEndpoint {
public:
    using Ptr = std::shared_ptr<AjaxEndpoint>;

    //----- AjaxEndpoint is a specialized Handler to handle the common AJAX programming technique.
    //      add() installs an instance on the specified Server which will accumulate incoming GET requests
    //      to URL's which match a specified pattern, but leave their responses pending.  When update()
    //      is subsequently called and provided with a JSON payload, that payload is returned as the body
    //      response body of all currently pending requests, with Content-Type set automatically to
    //      application/json, and the pending request list is cleared.  Note that the update() method is
    //      thread-safe.  Note also that the Server::addAjaxEndpoint() convenience method would typically be
    //      called in preference to calling the add() method here directly.

    static Ptr add(Server& server, std::string const& path);
    void update(std::string const& json);  // thread-safe

private:
    AjaxEndpoint(std::shared_ptr<Server> const server);

    std::shared_ptr<Server> const _server;

    std::vector<Response::Ptr> _pendingResponses;
    std::mutex _pendingResponsesMutex;
};

}}}  // namespace lsst::qserv::qhttp

#endif  // LSST_QSERV_QHTTP_AJAXENDPOINT_H
