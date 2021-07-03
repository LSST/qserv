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
#include "loader/DoList.h"

// System headers
#include <iostream>

// Qserv headers
#include "loader/Central.h"
#include "loader/LoaderMsg.h"
#include "proto/loader.pb.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.DoList");
}

namespace lsst {
namespace qserv {
namespace loader {


void DoList::checkList() {
    LOGS(_log, LOG_LVL_DEBUG, "DoList::checkList");
    std::lock_guard<std::mutex> lock(_listMtx);
    {
        std::lock_guard<std::mutex> lockAddList(_addListMtx);
        // Move all the items in _addList to _list. _addList is emptied
        _list.splice(_list.end(), _addList);
    }
    for (auto iter = _list.begin(); iter != _list.end(); ++iter){
        DoListItem::Ptr const& item = *iter;
        auto cmd = item->runIfNeeded(TimeOut::Clock::now());
        if (cmd != nullptr) {
            LOGS(_log, LOG_LVL_DEBUG, "queuing command");
            _central.queueCmd(cmd);
        } else {
            if (item->shouldRemoveFromList()) {
                LOGS(_log, LOG_LVL_INFO, "removing item");
                item->setAddedToList(false);
                iter = _list.erase(iter);
            }
        }
    }
}


void DoList::runItemNow(DoListItem::Ptr const& item) {
    auto cmd = item->runIfNeeded(TimeOut::Clock::now());
    if (cmd != nullptr) {
        LOGS(_log, LOG_LVL_INFO, "DoList::addAndRunItemNow queuing command");
        _central.queueCmd(cmd);
    }
}


}}} // namespace lsst:qserv::loader


