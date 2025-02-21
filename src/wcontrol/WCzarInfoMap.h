/*
 * LSST Data Management System
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
#ifndef LSST_QSERV_WCONTROL_WCZARINFOMAP_H
#define LSST_QSERV_WCONTROL_WCZARINFOMAP_H

// System headers
#include <map>
#include <memory>
#include <mutex>
#include <queue>

// Third-party headers

// Qserv headers
#include "global/clock_defs.h"
#include "global/intTypes.h"

namespace lsst::qserv::protojson {
class CzarContactInfo;
class WorkerContactInfo;
class WorkerCzarComIssue;
}  // namespace lsst::qserv::protojson

namespace lsst::qserv::wbase {
class UJTransmitCmd;
}

namespace lsst::qserv::wcontrol {

class Foreman;

/// This class is used to send the "/workerczarcomissue" from the worker to the
/// czar and then used by the czar to handle the message; the messsage itself
/// is made with WorkerCzarComIssue.
/// The general concept is that WorkerCzarComIssue exists on both the worker
/// and the czar and messages keep them in sync.
/// This class is assuming the czardId is correct and there are no duplicate czarIds.
class WCzarInfo : public std::enable_shared_from_this<WCzarInfo> {
public:
    using Ptr = std::shared_ptr<WCzarInfo>;

    std::string cName(const char* funcN) {
        return std::string("WCzarInfo::") + funcN + " czId=" + std::to_string(czarId);
    }

    WCzarInfo() = delete;
    ~WCzarInfo() = default;

    static Ptr create(CzarIdType czarId_) { return Ptr(new WCzarInfo(czarId_)); }

    /// If there were communication issues, start a thread to send the WorkerCzarComIssue message.
    void sendWorkerCzarComIssueIfNeeded(std::shared_ptr<protojson::WorkerContactInfo> const& wInfo_,
                                        std::shared_ptr<protojson::CzarContactInfo> const& czInfo_);

    /// Called by the worker after the czar successfully replied to the original
    /// message from the worker.
    void czarMsgReceived(TIMEPOINT tm);

    bool isAlive() const { return _alive; }

    /// Check if the czar is still considered to be alive, or it timed out.
    bool checkAlive(TIMEPOINT tmMark);

    std::shared_ptr<protojson::WorkerCzarComIssue> getWorkerCzarComIssue();

    CzarIdType const czarId;

private:
    WCzarInfo(CzarIdType czarId_);

    void _sendMessage();

    std::atomic<bool> _alive{true};
    TIMEPOINT _lastTouch{CLOCK::now()};

    /// This class tracks communication problems and prepares a message
    /// to inform the czar of the problem.
    std::shared_ptr<protojson::WorkerCzarComIssue> _workerCzarComIssue;
    mutable std::mutex _wciMtx;  ///< protects all private members.

    /// true when running a thread to send a message to the czar
    /// with _sendMessage()
    std::atomic<bool> _msgThreadRunning{false};
};

/// Each worker talks to multiple czars and needs a WCzarInfo object for each czar,
/// this class keeps track of those objects.
class WCzarInfoMap {
public:
    using Ptr = std::shared_ptr<WCzarInfoMap>;

    std::string cName(const char* funcN) { return std::string("WCzarInfoMap::") + funcN; }

    ~WCzarInfoMap() = default;

    static Ptr create() { return Ptr(new WCzarInfoMap()); }

    /// Return the WCzarInfo ptr associated with czId, creating a new one if needed.
    WCzarInfo::Ptr getWCzarInfo(CzarIdType czId);

private:
    WCzarInfoMap() = default;

    std::map<CzarIdType, WCzarInfo::Ptr> _wczMap;

    mutable std::mutex _wczMapMtx;
};

}  // namespace lsst::qserv::wcontrol

#endif  // LSST_QSERV_WCONTROL_WCZARINFOMAP_H
