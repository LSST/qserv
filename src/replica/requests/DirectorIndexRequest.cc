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

// Class header
#include "replica/requests/DirectorIndexRequest.h"

// System headers
#include <functional>
#include <iostream>
#include <stdexcept>

// Third party headers
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/filesystem.hpp"

// Qserv headers
#include "replica/config/Configuration.h"
#include "replica/contr/Controller.h"
#include "replica/requests/Messenger.h"
#include "replica/services/DatabaseServices.h"
#include "replica/services/ServiceProvider.h"
#include "replica/util/ProtocolBuffer.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;
namespace fs = boost::filesystem;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DirectorIndexRequest");
bool const disposeRequired = true;
}  // namespace

namespace lsst::qserv::replica {

ostream& operator<<(ostream& os, DirectorIndexRequestInfo const& info) {
    os << "DirectorIndexRequestInfo {error:'" << info.error << "',"
       << "fileName:'" << info.fileName << "',fileSizeBytes:" << info.fileSizeBytes << "}";
    return os;
}

DirectorIndexRequest::Ptr DirectorIndexRequest::createAndStart(
        std::shared_ptr<Controller> const& controller, string const& workerName, string const& database,
        string const& directorTable, unsigned int chunk, bool hasTransactions, TransactionId transactionId,
        CallbackType const& onFinish, int priority, bool keepTracking, string const& jobId,
        unsigned int requestExpirationIvalSec) {
    auto ptr = DirectorIndexRequest::Ptr(
            new DirectorIndexRequest(controller, workerName, database, directorTable, chunk, hasTransactions,
                                     transactionId, onFinish, priority, keepTracking));
    ptr->start(jobId, requestExpirationIvalSec);
    return ptr;
}

DirectorIndexRequest::DirectorIndexRequest(std::shared_ptr<Controller> const& controller,
                                           string const& workerName, string const& database,
                                           string const& directorTable, unsigned int chunk,
                                           bool hasTransactions, TransactionId transactionId,
                                           CallbackType const& onFinish, int priority, bool keepTracking)
        : RequestMessenger(controller, "INDEX", workerName, priority, keepTracking, ::disposeRequired),
          _database(database),
          _directorTable(directorTable),
          _chunk(chunk),
          _hasTransactions(hasTransactions),
          _transactionId(transactionId),
          _onFinish(onFinish) {
    controller->serviceProvider()->config()->assertDatabaseIsValid(database);
    _responseData.fileName =
            controller->serviceProvider()->config()->get<string>("database", "qserv-master-tmp-dir") + "/" +
            database + "_" + directorTable + "_" + to_string(chunk) +
            (hasTransactions ? "_p" + to_string(transactionId) : "");
}

DirectorIndexRequest::~DirectorIndexRequest() {
    // The file may be still open in case of any failure.
    if (_file.is_open()) {
        _file.close();
    }
    // Make the best attempt to get rid of the temporary file. Ignore any errors
    // for now. Just report them.
    boost::system::error_code ec;
    fs::remove(fs::path(_responseData.fileName), ec);
    if (ec.value() != 0) {
        LOGS(_log, LOG_LVL_WARN,
             context() << "::" << __func__ << "  "
                       << "failed to remove the temporary file '" << _responseData.fileName);
    }
}

DirectorIndexRequestInfo const& DirectorIndexRequest::responseData() const { return _responseData; }

void DirectorIndexRequest::startImpl(replica::Lock const& lock) { _sendInitialRequest(lock); }

void DirectorIndexRequest::_sendInitialRequest(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << " "
                   << " worker: " << workerName() << " database: " << database()
                   << " directorTable: " << directorTable() << " chunk: " << chunk() << " hasTransactions: "
                   << (hasTransactions() ? "true" : "false") << " transactionId: " << transactionId());

    // Serialize the Request message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::QUEUED);
    hdr.set_queued_type(ProtocolQueuedRequestType::INDEX);
    hdr.set_timeout(requestExpirationIvalSec());
    hdr.set_priority(priority());
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestDirectorIndex message;
    message.set_database(database());
    message.set_director_table(directorTable());
    message.set_chunk(chunk());
    message.set_has_transactions(hasTransactions());
    message.set_transaction_id(transactionId());
    message.set_offset(_responseData.fileSizeBytes);  // Begin or continue reading the input stream
                                                      // at a point where the previous request ended.
    buffer()->serialize(message);

    _send(lock);
}

void DirectorIndexRequest::awaken(boost::system::error_code const& ec) {
    string const context_ = context() + string(__func__) + " ";
    LOGS(_log, LOG_LVL_DEBUG, context_);
    if (isAborted(ec)) return;
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context_);
    if (state() == State::FINISHED) return;
    _sendStatusRequest(lock);
}

void DirectorIndexRequest::_sendStatusRequest(replica::Lock const& lock) {
    // Serialize the Status message header and the request itself into
    // the network buffer.
    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_TRACK);
    hdr.set_instance_id(controller()->serviceProvider()->instanceId());
    buffer()->serialize(hdr);

    ProtocolRequestTrack message;
    message.set_id(id());
    message.set_queued_type(ProtocolQueuedRequestType::INDEX);
    buffer()->serialize(message);

    _send(lock);
}

void DirectorIndexRequest::_send(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    controller()->serviceProvider()->messenger()->send<ProtocolResponseDirectorIndex>(
            workerName(), id(), priority(), buffer(),
            [self = shared_from_base<DirectorIndexRequest>()](string const& id, bool success,
                                                              ProtocolResponseDirectorIndex const& response) {
                self->_analyze(success, response);
            });
}

void DirectorIndexRequest::_analyze(bool success, ProtocolResponseDirectorIndex const& message) {
    string const context_ = context() + string(__func__) + " success=" + bool2str(success) + " ";
    LOGS(_log, LOG_LVL_DEBUG, context_);

    // This method is called on behalf of an asynchronous callback fired
    // upon a completion of the request within method send() - the only
    // client of analyze(). So, we should take care of proper locking and watch
    // for possible state transition which might occur while the async I/O was
    // still in a progress.
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context_);
    if (state() == State::FINISHED) return;
    if (!success) {
        finish(lock, CLIENT_ERROR);
        return;
    }

    // Always use  the latest status reported by the remote server
    setExtendedServerStatus(lock, message.status_ext());

    // Performance counters are updated from either of two sources,
    // depending on the availability of the 'target' performance counters
    // filled in by the 'STATUS' queries. If the later is not available
    // then fallback to the one of the current request.
    if (message.has_target_performance()) {
        mutablePerformance().update(message.target_performance());
    } else {
        mutablePerformance().update(message.performance());
    }

    // Always extract the MySQL error regardless of the completion status
    // reported by the worker service.
    _responseData.error = message.error();

    // Extract target request type-specific parameters from the response
    if (message.has_request()) {
        _targetRequestParams = DirectorIndexRequestParams(message.request());
    }
    switch (message.status()) {
        case ProtocolStatus::SUCCESS: {
            try {
                _writeInfoFile(lock, message.data());
                _responseData.fileSizeBytes += message.data().size();
                if (_responseData.fileSizeBytes >= message.total_bytes()) {
                    _file.close();
                    finish(lock, SUCCESS);
                } else {
                    // Before requesting the next chunk of data, make sure the previous
                    // worker request gets garbage collected. This operation is meant to
                    // prevent memory build up at the worker. Note that workers do automaic
                    // garbage collection of request based on the default value of the request
                    // expiration parameter in the configuration: (controller, request-timeout-sec).
                    // Normally this parameter would be set to some very high number. In this case,
                    // the memory build up at workers due to intermediate data stored in the worker
                    // request may be happening faster than the automatic garbage collection.
                    // Also note the elevated priority level for the request disposal operations.
                    // This will guarantee (in most cases) that such requests will be fast-track delivered
                    // to (and processed by) the worker.
                    dispose(lock, PRIORITY_VERY_HIGH,
                            [self = shared_from_base<DirectorIndexRequest>()](auto id, auto success,
                                                                              auto message) {
                                self->_disposed(success, message);
                            });
                }
            } catch (exception const& ex) {
                _responseData.error = ex.what();
                LOGS(_log, LOG_LVL_ERROR, context_ << _responseData.error);
                finish(lock, CLIENT_ERROR);
            }
            break;
        }
        case ProtocolStatus::CREATED:
            keepTrackingOrFinish(lock, SERVER_CREATED);
            break;
        case ProtocolStatus::QUEUED:
            keepTrackingOrFinish(lock, SERVER_QUEUED);
            break;
        case ProtocolStatus::IN_PROGRESS:
            keepTrackingOrFinish(lock, SERVER_IN_PROGRESS);
            break;
        case ProtocolStatus::IS_CANCELLING:
            keepTrackingOrFinish(lock, SERVER_IS_CANCELLING);
            break;
        case ProtocolStatus::BAD:
            finish(lock, SERVER_BAD);
            break;
        case ProtocolStatus::FAILED:
            finish(lock, SERVER_ERROR);
            break;
        case ProtocolStatus::CANCELLED:
            finish(lock, SERVER_CANCELLED);
            break;
        default:
            throw logic_error(context_ + "unknown status '" + ProtocolStatus_Name(message.status()) +
                              "' received from server");
    }
}

void DirectorIndexRequest::_disposed(bool success, ProtocolResponseDispose const& message) {
    string const context_ = context() + string(__func__) + " success=" + bool2str(success) + " ";
    LOGS(_log, LOG_LVL_DEBUG, context_);
    if (state() == State::FINISHED) return;
    replica::Lock lock(_mtx, context_);
    if (state() == State::FINISHED) return;
    if (!success) {
        finish(lock, CLIENT_ERROR);
        return;
    }
    // Continue reading the stream.
    _sendInitialRequest(lock);
}

void DirectorIndexRequest::_writeInfoFile(replica::Lock const& lock, string const& data) {
    if (!_file.is_open()) {
        _file.open(_responseData.fileName, ios::binary | ios::out | ios::trunc);
        if (!_file.good()) {
            throw runtime_error(context() + string(__func__) +
                                " failed to open/create/append file: " + _responseData.fileName);
        }
    }
    _file << data;
}

void DirectorIndexRequest::notify(replica::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<DirectorIndexRequest>(lock, _onFinish);
}

void DirectorIndexRequest::savePersistentState(replica::Lock const& lock) {
    controller()->serviceProvider()->databaseServices()->saveState(*this, performance(lock));
}

list<pair<string, string>> DirectorIndexRequest::extendedPersistentState() const {
    list<pair<string, string>> result;
    result.emplace_back("database", database());
    result.emplace_back("director_table", directorTable());
    result.emplace_back("chunk", to_string(chunk()));
    result.emplace_back("has_transactions", bool2str(hasTransactions()));
    result.emplace_back("transaction_id", to_string(transactionId()));
    return result;
}

}  // namespace lsst::qserv::replica
