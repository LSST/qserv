/*
 * LSST Data Management System
 * Copyright 2015 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */
#ifndef LSST_QSERV_CZAR_SUBMITRESULT_H
#define LSST_QSERV_CZAR_SUBMITRESULT_H

// System headers
#include <string>

// Qserv headers
#include "global/intTypes.h"

namespace lsst::qserv::czar {

/// @addtogroup czar

/**
 *  @ingroup czar
 *  @brief Structure used for returning result from Czar::submitQuery and from subsequent
 *   calls to Czar::getQueryInfo
 */
struct SubmitResult {
    // Populated by methods Czar::submitQuery and Czar::getQueryInfo
    std::string errorMessage;  ///< empty if there is no error
    std::string resultTable;   ///< Result table name
    std::string messageTable;  ///< Message table name
    std::string resultQuery;   ///< The query to execute to get results
    QueryId queryId = 0;       ///< The unique identifier of the user query

    // Populated by Czar::getQueryInfo only for queries which are still in flight
    std::string status;       ///< 'EXECUTING','COMPLETED','FAILED','ABORTED'
    int totalChunks = 0;      ///< The total number of chunks required by the query
    int completedChunks = 0;  ///< The number of chubnks that have been processed so far
    int queryBeginEpoch = 0;  ///< Seconds since UNIX Epoch
    int lastUpdateEpoch = 0;  ///< Seconds since UNIX Epoch
};

}  // namespace lsst::qserv::czar

#endif  // LSST_QSERV_CZAR_SUBMITRESULT_H
