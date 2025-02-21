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

#ifndef LSST_QSERV_WBASE_UBERJOBDATA_H
#define LSST_QSERV_WBASE_UBERJOBDATA_H

// System headers
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

// Third-party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "global/intTypes.h"
#include "http/Method.h"
#include "qmeta/types.h"
#include "util/QdispPool.h"
#include "wbase/SendChannel.h"

namespace lsst::qserv {

namespace protojson {
class ScanInfo;
}

namespace util {
class MultiError;
}

namespace wcontrol {
class Foreman;
}
}  // namespace lsst::qserv

namespace lsst::qserv::wbase {

class FileChannelShared;
class Task;

/// This class tracks all Tasks associates with the UberJob on the worker
/// and reports status to the czar.
class UberJobData : public std::enable_shared_from_this<UberJobData> {
public:
    using Ptr = std::shared_ptr<UberJobData>;

    UberJobData() = delete;
    UberJobData(UberJobData const&) = delete;

    static Ptr create(UberJobId uberJobId, std::string const& czarName, qmeta::CzarId czarId,
                      std::string const& czarHost, int czarPort, uint64_t queryId, int rowLimit,
                      uint64_t maxTableSizeBytes, std::shared_ptr<protojson::ScanInfo> const& scanInfo,
                      bool scanInteractive, std::string const& workerId,
                      std::shared_ptr<wcontrol::Foreman> const& foreman, std::string const& authKey,
                      uint16_t resultsHttpPort = 8080) {
        return Ptr(new UberJobData(uberJobId, czarName, czarId, czarHost, czarPort, queryId, rowLimit,
                                   maxTableSizeBytes, scanInfo, scanInteractive, workerId, foreman, authKey,
                                   resultsHttpPort));
    }
    /// Set file channel for this UberJob
    void setFileChannelShared(std::shared_ptr<FileChannelShared> const& fileChannelShared);

    bool getScanInteractive() const { return _scanInteractive; }
    std::shared_ptr<protojson::ScanInfo> getScanInfo() const { return _scanInfo; }

    UberJobId getUberJobId() const { return _uberJobId; }
    qmeta::CzarId getCzarId() const { return _czarId; }
    std::string getCzarHost() const { return _czarHost; }
    int getCzarPort() const { return _czarPort; }
    uint64_t getQueryId() const { return _queryId; }
    std::string getWorkerId() const { return _workerId; }
    uint64_t getMaxTableSizeBytes() const { return _maxTableSizeBytes; }

    /// Add the tasks defined in the UberJob to this UberJobData object.
    void addTasks(std::vector<std::shared_ptr<wbase::Task>> const& tasks) {
        std::lock_guard<std::mutex> tLg(_ujTasksMtx);
        _ujTasks.insert(_ujTasks.end(), tasks.begin(), tasks.end());
    }

    /// Let the czar know the result is ready.
    void responseFileReady(std::string const& httpFileUrl, uint64_t rowCount, uint64_t fileSize,
                           uint64_t headerCount);  // TODO:UJ remove headerCount

    /// Let the Czar know there's been a problem.
    bool responseError(util::MultiError& multiErr, int chunkId, bool cancelled);

    std::string const& getIdStr() const { return _idStr; }
    std::string cName(std::string const& funcName) { return "UberJobData::" + funcName + " " + getIdStr(); }

    bool getCancelled() const { return _cancelled; }

    /// Cancel all Tasks in this UberJob.
    void cancelAllTasks();

    /// Returns the LIMIT of rows for the query enforceable at the worker, where values <= 0 indicate
    /// that there is no limit to the number of rows sent back by the worker.
    /// Workers can only safely limit rows for queries that have the LIMIT clause without other related
    /// clauses like ORDER BY.
    int getRowLimit() const { return _rowLimit; }

    std::string resultFilePath() const;
    std::string resultFileHttpUrl() const;

private:
    UberJobData(UberJobId uberJobId, std::string const& czarName, qmeta::CzarId czarId, std::string czarHost,
                int czarPort, uint64_t queryId, int rowLimit, uint64_t maxTableSizeBytes,
                std::shared_ptr<protojson::ScanInfo> const& scanInfo, bool scanInteractive,
                std::string const& workerId, std::shared_ptr<wcontrol::Foreman> const& foreman,
                std::string const& authKey, uint16_t resultsHttpPort);

    /// Return the name of the file that will contain the results of the query.
    std::string _resultFileName() const;

    /// Queue the response to be sent to the originating czar.
    void _queueUJResponse(http::Method method_, std::vector<std::string> const& headers_,
                          std::string const& url_, std::string const& requestContext_,
                          std::string const& requestStr_);

    UberJobId const _uberJobId;
    std::string const _czarName;
    qmeta::CzarId const _czarId;
    std::string const _czarHost;
    int const _czarPort;
    QueryId const _queryId;
    int const _rowLimit;  ///< If > 0, only read this many rows before return the results.
    uint64_t const _maxTableSizeBytes;
    std::string const _workerId;
    std::string const _authKey;
    uint16_t const _resultsHttpPort;  ///<  = 8080

    std::shared_ptr<wcontrol::Foreman> const _foreman;

    std::vector<std::shared_ptr<wbase::Task>> _ujTasks;
    std::shared_ptr<FileChannelShared> _fileChannelShared;

    std::mutex _ujTasksMtx;  ///< Protects _ujTasks.

    /// True if this an interactive (aka high priority) user query.
    std::atomic<bool> _scanInteractive;

    /// Pointer to scan rating and table information.
    std::shared_ptr<protojson::ScanInfo> _scanInfo;

    std::string const _idStr;

    std::atomic<bool> _cancelled{false};  ///< Set to true if this was cancelled.
};

/// This class puts the information about a locally finished UberJob into a command
/// so it can be put on a queue and sent to the originating czar. The information
/// being transmitted is usually the url for the result file or an error message.
class UJTransmitCmd : public util::PriorityCommand {
public:
    using Ptr = std::shared_ptr<UJTransmitCmd>;

    UJTransmitCmd() = delete;
    ~UJTransmitCmd() override = default;

    std::string cName(const char* funcN) const;

    static Ptr create(std::shared_ptr<wcontrol::Foreman> const& foreman_, UberJobData::Ptr const& ujData_,
                      http::Method method_, std::vector<std::string> const& headers_, std::string const& url_,
                      std::string const& requestContext_, std::string const& requestStr_) {
        auto ptr = Ptr(
                new UJTransmitCmd(foreman_, ujData_, method_, headers_, url_, requestContext_, requestStr_));
        ptr->_selfPtr = ptr;
        return ptr;
    }

    /// Send the UberJob file to the czar, this is the function that will be run when
    /// the queue reaches this command. If this message is not received by the czar,
    /// it will notify WCzarInfo and possibly send WorkerCzarComIssue.
    void action(util::CmdData* data) override;

    /// Reset the self pointer so this object can be killed.
    void kill();

    /// This function makes a duplicate of the required information for transmition to the czar
    /// in a new object and then increments the attempt count, so it is not a true copy.
    /// Priority commands cannot be resent as there's information in them about which queue
    /// to modify, so a fresh object is needed to re-send. The message and target czar remain
    /// unchanged except for the atttempt count.
    Ptr duplicate();

private:
    UJTransmitCmd(std::shared_ptr<wcontrol::Foreman> const& foreman_, UberJobData::Ptr const& ujData_,
                  http::Method method_, std::vector<std::string> const& headers_, std::string const& url_,
                  std::string const& requestContext_, std::string const& requestStr_)
            : PriorityCommand(),
              _foreman(foreman_),
              _ujData(ujData_),
              _czarId(ujData_->getCzarId()),
              _queryId(ujData_->getQueryId()),
              _uberJobId(ujData_->getUberJobId()),
              _method(method_),
              _headers(headers_),
              _url(url_),
              _requestContext(requestContext_),
              _requestStr(requestStr_) {}

    Ptr _selfPtr;  ///< So this object can put itself back on the queue and keep itself alive.
    std::shared_ptr<wcontrol::Foreman> const _foreman;
    std::weak_ptr<UberJobData> const _ujData;
    CzarIdType const _czarId;
    QueryId const _queryId;
    UberJobId const _uberJobId;
    http::Method const _method;
    std::vector<std::string> const _headers;
    std::string const _url;
    std::string const _requestContext;
    std::string const _requestStr;
    int _attemptCount = 0;  ///< How many attempts have been made to transmit this.
};

}  // namespace lsst::qserv::wbase

#endif  // LSST_QSERV_WBASE_UBERJOBDATA_H
