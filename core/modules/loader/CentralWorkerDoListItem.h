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

#ifndef LSST_QSERV_LOADER_CENTRALWORKERDOLISTITEM_H
#define LSST_QSERV_LOADER_CENTRALWORKERDOLISTITEM_H

// Qserv headers
#include "loader/CentralWorker.h"

namespace lsst {
namespace qserv {
namespace loader {

/// This class exists to regularly call the CentralWorker::_monitor() function, which
/// does things like monitor TCP connections and control shifting with the right neighbor.
class CentralWorkerDoListItem : public DoListItem {
public:
    CentralWorkerDoListItem() = delete;
    explicit CentralWorkerDoListItem(CentralWorker* centralWorker) : _centralWorker(centralWorker) {
        setTimeOut(std::chrono::seconds(4));  // TODO: DM-17453 set via config
    }

    util::CommandTracked::Ptr createCommand() override {
        struct CWMonitorCmd : public util::CommandTracked {
            CWMonitorCmd(CentralWorker* centralW) : centralWorker(centralW) {}
            void action(util::CmdData*) override {
                centralWorker->_monitor();
            }
            CentralWorker* centralWorker;
        };
        util::CommandTracked::Ptr cmd(std::make_shared<CWMonitorCmd>(_centralWorker));
        return cmd;
    }

private:
    CentralWorker* _centralWorker;
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_CENTRAL_WORKER_DO_LIST_ITEM_H





