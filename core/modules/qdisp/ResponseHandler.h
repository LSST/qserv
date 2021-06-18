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
#ifndef LSST_QSERV_QDISP_RESPONSEHANDLER_H
#define LSST_QSERV_QDISP_RESPONSEHANDLER_H

// System headers
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Qserv headers
#include "util/Error.h"

namespace lsst {
namespace qserv {
namespace qdisp {

class JobBase;




/// ResponseHandler is an interface that handles result bytes. Tasks are
/// submitted to an Executive instance naming a resource unit (what resource is
/// required), a request string (task payload), and a handler for returning bytes.
/// The ResponseHandler implements logic to process incoming results
/// and buffers that are sized to the number of bytes expected in the next
/// segment of results.
class ResponseHandler {
public:
    typedef util::Error Error;
    using BufPtr = std::shared_ptr<std::vector<char>>;

    typedef std::shared_ptr<ResponseHandler> Ptr;
    ResponseHandler() {}
    void setJobQuery(std::shared_ptr<JobBase> const& jobBase) { _jobBase = jobBase; }
    virtual ~ResponseHandler() {}

    /// Flush the retrieved buffer where bLen bytes were set. If last==true,
    /// then no more buffer() and flush() calls should occur.
    /// @return true if successful (no error)
    virtual bool flush(int bLen, BufPtr const& bufPtr, bool& last, bool& largeResult,
                       int& nextBufSize) = 0;

    /// Signal an unrecoverable error condition. No further calls are expected.
    virtual void errorFlush(std::string const& msg, int code) = 0;

    /// @return true if the receiver has completed its duties.
    virtual bool finished() const = 0;
    virtual bool reset() = 0; ///< Reset the state that a request can be retried.

    /// Print a string representation of the receiver to an ostream
    virtual std::ostream& print(std::ostream& os) const = 0;

    /// @return an error code and description
    virtual Error getError() const = 0;

    /// Do anything that needs to be done if this job gets cancelled.
    virtual void processCancel() {};

    /// Scrub the results from jobId-attempt from the result table.
    virtual void prepScrubResults(int jobId, int attempt) = 0;

    std::weak_ptr<JobBase> getJobBase() { return _jobBase; }

private:
    std::weak_ptr<JobBase> _jobBase;
};

inline std::ostream& operator<<(std::ostream& os, ResponseHandler const& r) {
    return r.print(os);
}


}}} // namespace lsst::qserv::qdisp

#endif // LSST_QSERV_QDISP_RESPONSEHANDLER_H
