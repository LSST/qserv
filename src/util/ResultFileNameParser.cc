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
#include "util/ResultFileNameParser.h"

// Third party headers
#include "boost/filesystem.hpp"

// Qserv headers
#include "util/String.h"

using namespace std;
using namespace nlohmann;
namespace fs = boost::filesystem;

namespace lsst::qserv::util {

string const ResultFileNameParser::fileExt = ".proto";

ResultFileNameParser::ResultFileNameParser(fs::path const& filePath) : _fileName(filePath.stem().string()) {
    _parse();
}

ResultFileNameParser::ResultFileNameParser(string const& filePath)
        : _fileName(fs::path(filePath).stem().string()) {
    _parse();
}

json ResultFileNameParser::toJson() const {
    return json::object(
            {{"czar_id", czarId}, {"query_id", queryId}, {"job_id", jobId}, {"chunk_id", chunkId}});
}

bool ResultFileNameParser::operator==(ResultFileNameParser const& rhs) const {
    return (czarId == rhs.czarId) && (queryId == rhs.queryId) && (jobId == rhs.jobId) &&
           (chunkId == rhs.chunkId);
}

ostream& operator<<(ostream& os, ResultFileNameParser const& parser) {
    os << parser.toJson();
    return os;
}

string ResultFileNameParser::_context(string const& func) {
    return "FileChannelShared::ResultFileNameParser::" + func;
}

void ResultFileNameParser::_parse() {
    _taskAttributes = String::parseToVectUInt64(_fileName, "-");
    if (_taskAttributes.size() != 4) {
        throw invalid_argument(_context(__func__) + " not a valid result file name: " + _fileName);
    }
    size_t attrIndex = 0;
    _validateAndStoreAttr(attrIndex++, "czarId", czarId);
    _validateAndStoreAttr(attrIndex++, "queryId", queryId);
    _validateAndStoreAttr(attrIndex++, "jobId", jobId);
    _validateAndStoreAttr(attrIndex++, "chunkId", chunkId);
}

}  // namespace lsst::qserv::util
