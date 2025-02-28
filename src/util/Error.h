// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2014-2016 AURA/LSST.
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
 * @ingroup util
 *
 * @brief Store a Qserv error
 *
 * @author Fabrice Jammes, IN2P3/SLAC
 */

#ifndef LSST_QSERV_UTIL_ERROR_H_
#define LSST_QSERV_UTIL_ERROR_H_

// System headers
#include <string>

namespace lsst::qserv::util {

/**
 * List of known Qserv errors
 *
 * TODO: fix confusion between status and code see: DM-2996
 * TODO: centralize all error code (here?) see: DM-2416
 */
struct ErrorCode {
    enum errCode {
        NONE = 0,
        // Query plugin errors:
        DUPLICATE_SELECT_EXPR,
        // InfileMerger errors:
        HEADER_IMPORT,
        HEADER_OVERFLOW,
        RESULT_IMPORT,
        RESULT_MD5,
        MYSQLOPEN,
        MERGEWRITE,
        TERMINATE,
        CREATE_TABLE,
        MYSQLCONNECT,
        MYSQLEXEC,
        INTERNAL,
        CZAR_RESULT_TOO_LARGE,
        // Worker errors:
        WORKER_RESULT_TOO_LARGE
    };
};

/** @brief Store a Qserv error
 *
 * To be used with util::MultiError
 *
 */
class Error {
public:
    Error(int code = ErrorCode::NONE, std::string const& msg = "", int status = ErrorCode::NONE,
          bool logLvLErr = true);

    /** Overload output operator for current class
     *
     * @param out
     * @param error
     * @return an output stream
     */
    friend std::ostream& operator<<(std::ostream& out, Error const& error);

    int getCode() const { return _code; }

    const std::string& getMsg() const { return _msg; }

    int getStatus() const { return _status; }

    /** Check if current Object contains an actual error
     *
     *  By convention, code==util::ErrorCode::NONE
     *  means that no error has been detected
     *
     * @return true if current object doesn't contain an actual error
     */
    bool isNone() { return (_code == util::ErrorCode::NONE); }

private:
    int _code;
    std::string _msg;
    int _status;
};

}  // namespace lsst::qserv::util

#endif /* LSST_QSERV_UTIL_ERROR_H_ */
