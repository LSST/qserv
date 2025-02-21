// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015-2016 LSST Corporation.
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

/**
 * @file
 *
 * @brief JobStatus implementation
 *
 * Store information message issused by a job
 * running a chunkQuery on an SSI ressource
 *
 * @author Fabrice Jammes, IN2P3
 */

// Class header
#include "qmeta/JobStatus.h"

// System headers
#include <chrono>
#include <ctime>
#include <iostream>

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qmeta.JobStatus");
}

namespace lsst::qserv::qmeta {

JobStatus::Info::Info() : state(UNKNOWN), stateCode(0) { stateTime = getNow(); }

void JobStatus::updateInfo(std::string const& idMsg, JobStatus::State s, std::string const& source, int code,
                           std::string const& desc, MessageSeverity severity) {
    std::lock_guard<std::mutex> lock(_mutex);
    _updateInfo(idMsg, s, source, code, desc, severity);
}

void JobStatus::_updateInfo(std::string const& idMsg, JobStatus::State s, std::string const& source, int code,
                            std::string const& desc, MessageSeverity severity) {
    LOGS(_log, LOG_LVL_DEBUG,
         idMsg << " Updating state to: " << s << " code=" << code << " " << desc << " src=" << source);
    _info.stateTime = getNow();
    _info.state = s;
    _info.stateCode = code;
    _info.stateDesc = desc;
    _info.source = source;
    _info.severity = severity;
}

void JobStatus::updateInfoNoErrorOverwrite(std::string const& idMsg, JobStatus::State s,
                                           std::string const& source, int code, std::string const& desc,
                                           MessageSeverity severity) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto jState = _info.state;
    if (jState != qmeta::JobStatus::CANCEL && jState != qmeta::JobStatus::RESPONSE_ERROR &&
        jState != qmeta::JobStatus::RESULT_ERROR && jState != qmeta::JobStatus::MERGE_ERROR) {
        _updateInfo(idMsg, s, source, code, desc, severity);
    }

    LOGS(_log, LOG_LVL_DEBUG,
         idMsg << " Updating state to: " << s << " code=" << code << " " << desc << " src=" << source);
    _info.stateTime = getNow();
    _info.state = s;
    _info.stateCode = code;
    _info.stateDesc = desc;
    _info.source = source;
    _info.severity = severity;
}

std::string JobStatus::stateStr(JobStatus::State const& state) {
    std::string msg("?");
    switch (state) {
        case JobStatus::UNKNOWN:
            msg = "UNKNOWN";
            break;
        case JobStatus::REQUEST:
            msg = "REQUEST";
            break;
        case JobStatus::RESPONSE_READY:
            msg = "RESPONSE_READY";
            break;
        case JobStatus::RESPONSE_ERROR:
            msg = "RESPONSE_ERROR";
            break;
        case JobStatus::RESPONSE_DATA:
            msg = "RESPONSE_DATA";
            break;
        case JobStatus::RESPONSE_DATA_NACK:
            msg = "RESPONSE_DATA_NACK";
            break;
        case JobStatus::RESPONSE_DONE:
            msg = "RESPONSE_DONE";
            break;
        case JobStatus::RESULT_ERROR:
            msg = "RESULT_ERROR";
            break;
        case JobStatus::MERGE_ERROR:
            msg = "MERGE_ERROR";
            break;
        case JobStatus::COMPLETE:
            msg = "COMPLETE (success)";
            break;
        case JobStatus::CANCEL:
            msg = "CANCEL";
            break;
        default:
            msg = "(unrecognized) state=" + std::to_string((int)state);
    }
    return msg;
}

JobStatus::TimeType JobStatus::getNow() { return std::chrono::system_clock::now(); }

std::string JobStatus::timeToString(TimeType const& inTime) {
    const int date_buf_len = sizeof "2011-10-08T07:07:09+0200";
    char date_buf[date_buf_len];
    struct tm tmpTm;
    auto locTime = std::chrono::system_clock::to_time_t(inTime);
    ::strftime(date_buf, date_buf_len, "%FT%T%z", ::localtime_r(&locTime, &tmpTm));
    return date_buf;
}

std::string JobStatus::Info::timeStr() const { return timeToString(stateTime); }

uint64_t JobStatus::timeToInt(TimeType inTime) {
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(inTime.time_since_epoch());
    uint64_t val = dur.count();
    return val;
}

uint64_t JobStatus::Info::timeInt() const { return timeToInt(stateTime); }

std::ostream& operator<<(std::ostream& os, JobStatus::State const& state) {
    os << JobStatus::stateStr(state);
    return os;
}

std::ostream& operator<<(std::ostream& os, JobStatus const& jobstatus) { return os << jobstatus.getInfo(); }

std::ostream& operator<<(std::ostream& os, JobStatus::Info const& info) {
    os << ": " << info.timeStr() << ", " << info.state << ", " << info.source << ", " << info.stateCode
       << ", " << info.stateDesc << ", " << info.severity;
    return os;
}

}  // namespace lsst::qserv::qmeta
