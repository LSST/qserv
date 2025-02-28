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
#ifndef LSST_QSERV_QMETA_JOBSTATUS_H
#define LSST_QSERV_QMETA_JOBSTATUS_H

// System headers
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <time.h>

// qserv headers
#include "global/constants.h"

namespace lsst::qserv::qmeta {

/** Monitor execution of a chunk query.
 *
 *  JobStatus instances receive timestamped reports of execution State. This
 *  allows a manager object to receive updates on status without exposing its
 *  existence to a delegate class.
 *
 *  TODO: The JobStatus class could be extended to
 *  save all received reports to provide a timeline of state changes.
 *
 *  @see qdisp::JobDescription
 */
class JobStatus {
public:
    typedef std::shared_ptr<JobStatus> Ptr;
    using Clock = std::chrono::system_clock;
    using TimeType = std::chrono::time_point<Clock>;
    JobStatus() {}

    // TODO: these shouldn't be exposed, and so shouldn't be user-level error
    // codes, but maybe we can be clever and avoid an ugly remap/translation
    // with msgCode.h. 1201-1289 (inclusive) are free and MSG_FINALIZED==2000
    enum State {
        UNKNOWN = 0,
        REQUEST = 1203,
        RESPONSE_READY,
        RESPONSE_DATA,
        RESPONSE_DATA_NACK,
        RESPONSE_DONE,
        CANCEL,
        RESPONSE_ERROR,  // Errors must be between CANCEL and COMPLETE
        RESULT_ERROR,
        MERGE_ERROR,
        RETRY_ERROR,
        COMPLETE = 2000
    };

    /// Return time as milliseconds since the epoch.
    static TimeType getNow();

    /// Return a representation of the time as a string.
    static std::string timeToString(TimeType const& inTime);

    static uint64_t timeToInt(TimeType inTime);

    /** Report a state transition by updating JobStatus::Info attributes
     *  with its input parameters values
     *
     *  Useful for logging and error reporting
     *
     *  @param idMsg string for message containing job id and other log information.
     *  @param s state value
     *  @param code code value, default to 0
     *  @param desc message, default to ""
     *
     * TODO: Save past state history:
     *  - resourceUnit should be extracted from Info (beware of mutex)
     *  - Info should be put in a vector
     */
    void updateInfo(std::string const& idMsg, State s, std::string const& source, int code = 0,
                    std::string const& desc = "", MessageSeverity severity = MSG_INFO);

    /// Same as updateInfo() except existing error states are not overwritten.
    /// @see updateInfo()
    /// @return Negative values indicate the status was changed, zero and positive values
    void updateInfoNoErrorOverwrite(std::string const& idMsg, State s, std::string const& source,
                                    int code = 0, std::string const& desc = "",
                                    MessageSeverity severity = MSG_INFO);

    struct Info {
        Info();
        // More detailed debugging may store a vector of states, appending
        // with each invocation of report().
        State state;                          ///< Actual state
        TimeType stateTime;                   ///< Last modified timestamp
        int stateCode;                        ///< Code associated with state (e.g. xrd or mysql error code)
        std::string stateDesc;                ///< Textual description
        std::string source = "";              ///< Source of the current state.
        MessageSeverity severity = MSG_INFO;  ///< Severity of the message.

        uint64_t timeInt() const;     ///< Get time in millisec since the epoch
        std::string timeStr() const;  ///< Get string representation of time.
    };

    Info getInfo() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _info;
    }

    State getState() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _info.state;
    }

    static std::string stateStr(JobStatus::State const& state);

    friend std::ostream& operator<<(std::ostream& os, JobStatus const& es);

private:
    /// @see updateInfo()
    /// note: _mutex must be held before calling.
    void _updateInfo(std::string const& idMsg, JobStatus::State s, std::string const& source, int code,
                     std::string const& desc, MessageSeverity severity);

    Info _info;
    mutable std::mutex _mutex;  ///< Mutex to guard concurrent updates
};
std::ostream& operator<<(std::ostream& os, JobStatus const& es);
std::ostream& operator<<(std::ostream& os, JobStatus::Info const& inf);
std::ostream& operator<<(std::ostream& os, JobStatus::State const& state);

}  // namespace lsst::qserv::qmeta

#endif  // LSST_QSERV_META_JOBSTATUS_H
