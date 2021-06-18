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
#ifndef LSST_QSERV_QDISP_JOBBASE_H
#define LSST_QSERV_QDISP_JOBBASE_H

// System headers
#include <memory>
#include <string>

// Qserv headers
#include "global/intTypes.h"

// This header declarations
namespace lsst {
namespace qserv {
namespace qdisp {

class JobStatus;
class QdispPool;
class ResponseHandler;
class QueryRequest;

/// Base class for JobQuery and UberJob.
/// TODO:UJ This could use a lot of cleanup.
class JobBase : public std::enable_shared_from_this<JobBase> {
public:
    using Ptr = std::shared_ptr<JobBase>;

    JobBase() = default;
    JobBase(JobBase const&) = delete;
    JobBase& operator=(JobBase const&) = delete;
    virtual ~JobBase() = default;

    virtual QueryId getQueryId() const = 0;
    virtual int getIdInt() const = 0;
    virtual std::string const& getIdStr() const = 0;
    virtual std::shared_ptr<QdispPool> getQdispPool() = 0;
    virtual std::string const& getPayload() const = 0; ///< const& in return type is essential for xrootd
    virtual std::shared_ptr<ResponseHandler> getRespHandler() = 0;
    virtual std::shared_ptr<JobStatus> getStatus() = 0;
    virtual bool getScanInteractive() const = 0;
    virtual bool isQueryCancelled() = 0;
    virtual void callMarkCompleteFunc(bool success) = 0;
    virtual void setQueryRequest(std::shared_ptr<QueryRequest> const& qr) = 0;

    virtual std::ostream& dumpOS(std::ostream &os) const;

    std::string dump() const;
    friend std::ostream& operator<<(std::ostream& os, JobBase const& jb);
};


}}} // namespace lsst::qserv::qdisp

#endif // LSST_QSERV_QDISP_JOBBASE_H
