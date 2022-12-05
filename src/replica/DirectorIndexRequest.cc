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
#include "replica/DirectorIndexRequest.h"

// System headers
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

// Third party headers
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/filesystem.hpp"

// Qserv headers
#include "replica/Controller.h"
#include "replica/DatabaseServices.h"
#include "replica/Messenger.h"
#include "replica/ProtocolBuffer.h"
#include "replica/ServiceProvider.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;
using namespace std::placeholders;
namespace fs = boost::filesystem;

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.replica.DirectorIndexRequest");

}  // namespace

namespace lsst::qserv::replica {

ostream& operator<<(ostream& os, DirectorIndexRequestInfo const& info) {
    os << "DirectorIndexRequestInfo {error:'" << info.error << "',"
       << "fileName:'" << info.fileName << "',fileSizeBytes:" << info.fileSizeBytes << "}";
    return os;
}

DirectorIndexRequest::Ptr DirectorIndexRequest::create(
        ServiceProvider::Ptr const& serviceProvider, boost::asio::io_service& io_service,
        string const& worker, string const& database, string const& directorTable, unsigned int chunk,
        bool hasTransactions, TransactionId transactionId, CallbackType const& onFinish, int priority,
        bool keepTracking, shared_ptr<Messenger> const& messenger) {
    return DirectorIndexRequest::Ptr(new DirectorIndexRequest(
            serviceProvider, io_service, worker, database, directorTable, chunk, hasTransactions,
            transactionId, onFinish, priority, keepTracking, messenger));
}

DirectorIndexRequest::DirectorIndexRequest(ServiceProvider::Ptr const& serviceProvider,
                                           boost::asio::io_service& io_service, string const& worker,
                                           string const& database, string const& directorTable,
                                           unsigned int chunk, bool hasTransactions,
                                           TransactionId transactionId, CallbackType const& onFinish,
                                           int priority, bool keepTracking,
                                           shared_ptr<Messenger> const& messenger)
        : RequestMessenger(serviceProvider, io_service, "INDEX", worker, priority, keepTracking,
                           false,  // allowDuplicate
                           true,   // disposeRequired
                           messenger),
          _database(database),
          _directorTable(directorTable),
          _chunk(chunk),
          _hasTransactions(hasTransactions),
          _transactionId(transactionId),
          _onFinish(onFinish),
          _fileName(serviceProvider->config()->get<string>("database", "qserv-master-tmp-dir") + "/" +
                    database + "_" + directorTable + "_" + to_string(chunk) +
                    (hasTransactions ? "_p" + to_string(transactionId) : "")) {
    Request::serviceProvider()->config()->assertDatabaseIsValid(database);
}

DirectorIndexRequest::~DirectorIndexRequest() {
    // Make the best attempt to get rid of the temporary file. Ignore any errors
    // for now. Just report them.
    boost::system::error_code ec;
    fs::remove(fs::path(_fileName), ec);
    if (ec.value() != 0) {
        LOGS(_log, LOG_LVL_WARN,
             context() << "::" << __func__ << "  "
                       << "failed to remove the temporary file '" << _fileName);
    }
}

DirectorIndexRequestInfo const& DirectorIndexRequest::responseData() const { return _responseData; }

void DirectorIndexRequest::startImpl(util::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG,
         context() << __func__ << " "
                   << " worker: " << worker() << " database: " << database()
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
    hdr.set_instance_id(serviceProvider()->instanceId());

    buffer()->serialize(hdr);

    ProtocolRequestDirectorIndex message;
    message.set_database(database());
    message.set_director_table(directorTable());
    message.set_chunk(chunk());
    message.set_has_transactions(hasTransactions());
    message.set_transaction_id(transactionId());

    buffer()->serialize(message);

    _send(lock);
}

void DirectorIndexRequest::awaken(boost::system::error_code const& ec) {
    string const context_ = context() + string(__func__) + " ";
    LOGS(_log, LOG_LVL_DEBUG, context_);

    if (isAborted(ec)) return;

    if (state() == State::FINISHED) return;
    util::Lock lock(_mtx, context_);
    if (state() == State::FINISHED) return;

    // Serialize the Status message header and the request itself into
    // the network buffer.

    buffer()->resize();

    ProtocolRequestHeader hdr;
    hdr.set_id(id());
    hdr.set_type(ProtocolRequestHeader::REQUEST);
    hdr.set_management_type(ProtocolManagementRequestType::REQUEST_STATUS);
    hdr.set_instance_id(serviceProvider()->instanceId());

    buffer()->serialize(hdr);

    ProtocolRequestStatus message;
    message.set_id(id());
    message.set_queued_type(ProtocolQueuedRequestType::INDEX);

    buffer()->serialize(message);

    _send(lock);
}

void DirectorIndexRequest::_send(util::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    auto self = shared_from_base<DirectorIndexRequest>();
    messenger()->send<ProtocolResponseDirectorIndex>(
            worker(), id(), priority(), buffer(),
            [self](string const& id, bool success, ProtocolResponseDirectorIndex const& response) {
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
    util::Lock lock(_mtx, context_);
    if (state() == State::FINISHED) return;

    if (not success) {
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
            } catch (exception const& ex) {
                _responseData.error = ex.what();
                LOGS(_log, LOG_LVL_ERROR, context_ << _responseData.error);
                finish(lock, CLIENT_ERROR);
                break;
            }
            _responseData.fileName = _fileName;
            _responseData.fileSizeBytes = message.data().size();
            finish(lock, SUCCESS);
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

void DirectorIndexRequest::_writeInfoFile(util::Lock const& lock, string const& data) {
    ofstream f(_fileName, ios::out | ios::trunc);
    if (!f.good()) {
        throw runtime_error(context() + string(__func__) +
                            " failed to open/create/append file: " + _fileName);
    }
    f << data;
    f.close();
}

void DirectorIndexRequest::notify(util::Lock const& lock) {
    LOGS(_log, LOG_LVL_DEBUG, context() << __func__);
    notifyDefaultImpl<DirectorIndexRequest>(lock, _onFinish);
}

void DirectorIndexRequest::savePersistentState(util::Lock const& lock) {
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
