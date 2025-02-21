// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2012-2015 LSST Corporation.
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

/// MessageStore.h declares:
///
/// struct QueryMessage
/// class MessageStore

/// The MessageStore classes are responsible for maintaining status and
/// error messages associated with a query.

#ifndef LSST_QSERV_QMETA_MESSAGESTORE_H
#define LSST_QSERV_QMETA_MESSAGESTORE_H

// System headers
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

// Qserv headers
#include "global/constants.h"
#include "qmeta/JobStatus.h"

namespace lsst::qserv::qmeta {

struct QueryMessage {
    QueryMessage(int chunkId_, std::string const& msgSource_, int code_, std::string description_,
                 qmeta::JobStatus::TimeType timestamp_, MessageSeverity severity_)
            : chunkId(chunkId_),
              msgSource(msgSource_),
              code(code_),
              description(description_),
              timestamp(timestamp_),
              severity(severity_) {}

    int chunkId;
    std::string msgSource;
    int code;
    std::string description;
    qmeta::JobStatus::TimeType timestamp;
    MessageSeverity severity;
};

/** Store messages issued by Qserv workers and czar
 *
 * For each SQL query, these messages are stored in a MySQL message table
 * so that mysql-proxy can retrieve it and log it or send error messages
 * to client. These messages are also stored in QMeta in the QMessages
 * table.
 *
 * `msgSource` is used to help sort the messages into appropriate
 * categories. "COMPLETE", "MULITERROR", "EXECFAIL", and "CANCEL" are
 * reserved for specific cases.
 * "MULTIERROR" - is a combined error message that is placed in the
 *      messages_# table specific to the query and is sent to the user
 *      by the proxy. This is not added to QMessages.
 * "COMPLETE" - indicates that there were no problems with a particular job.
 * "CANCEL" - indicates the job was cancelled by the system
 * "EXECFAIL" - indicates that a job has been killed by the executive
 *      because merging or a different job failed.
 */
class MessageStore {
public:
    /** Add a message to this MessageStore
     *
     * This message will be sent to proxy via message table, in order to be
     * displayed in mysql-proxy logs.
     *
     * @param chunkId chunkId related to the message, -1 if not available
     * @param code code of the message
     * @param description text of the message
     * @param severity_ message severity level, default to MSG_INFO
     * @param timestamp milliseconds since the epoch (1970). Default is 0.
     */
    void addMessage(int chunkId, std::string const& msgSource, int code, std::string const& description,
                    MessageSeverity severity_ = MessageSeverity::MSG_INFO,
                    qmeta::JobStatus::TimeType timestamp = qmeta::JobStatus::TimeType());

    /** Add an error message to this MessageStore
     *
     * This message will be sent to mysql-proxy via message table, in order
     * display an error in mysql client console. chunkId and code are set
     * to NOTSET because this message may aggregate multiple error messages
     * in multiple files. Indeed, mysql-client can only display
     * one error message per query.
     *
     * @param description text of the message
     */
    void addErrorMessage(std::string const& msgSource, std::string const& description);
    QueryMessage getMessage(int idx) const;
    int messageCount() const;
    int messageCount(int code) const;

private:
    std::mutex _storeMutex;
    std::vector<QueryMessage> _queryMessages;
};

}  // namespace lsst::qserv::qmeta

#endif  // LSST_QSERV_QMETA_MESSAGESTORE_H
