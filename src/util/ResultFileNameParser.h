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

#ifndef LSST_QSERV_UTIL_RESULTFILENAMEPARSER_H
#define LSST_QSERV_UTIL_RESULTFILENAMEPARSER_H

// System headers
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

// Third-party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "global/intTypes.h"
#include "qmeta/types.h"

// Forward declarations

namespace boost::filesystem {
class path;
}  // namespace boost::filesystem

// This header declarations

namespace lsst::qserv::util {

/**
 * Utility class ResultFileNameParser parses the file path, extracts attributes from
 * the file name, validates attribute values to ensure they're in the valid range,
 * and and stored them in the corresponding data members. Parsing is done in the class's
 * constructors. Two forms of the construction are provided for convenience of
 * the client applications.
 *
 * The file path is required to have the following format:
 * @code
 *   [<folder>/]<czar-id>-<query-id>-<job-id>-<chunk-id>-<attemptcount>[.<ext>]
 * @code
 */
class ResultFileNameParser {
public:
    /// The file extention including the '.' prefix.
    static std::string const fileExt;

    qmeta::CzarId czarId = 0;
    QueryId queryId = 0;
    std::uint32_t jobId = 0;
    std::uint32_t chunkId = 0;

    ResultFileNameParser() = default;
    ResultFileNameParser(ResultFileNameParser const&) = default;
    ResultFileNameParser& operator=(ResultFileNameParser const&) = default;

    /// @param filePath The file to be evaluated.
    /// @throw std::invalid_argument If the file path did not match expectations.
    explicit ResultFileNameParser(boost::filesystem::path const& filePath);

    /// @param filePath The file to be evaluated.
    /// @throw std::invalid_argument If the file path did not match expectations.
    explicit ResultFileNameParser(std::string const& filePath);

    /// @return The JSON object (dictionary) encapsulating values of the attributes.
    nlohmann::json toJson() const;

    bool operator==(ResultFileNameParser const& rhs) const;
    bool operator!=(ResultFileNameParser const& rhs) const { return operator==(rhs); }

    friend std::ostream& operator<<(std::ostream& os, ResultFileNameParser const& parser);

private:
    static std::string _context(std::string const& func);
    void _parse();

    template <typename T>
    void _validateAndStoreAttr(std::size_t attrIndex, std::string const& attrName, T& attr) {
        std::uint64_t const& attrValue = _taskAttributes[attrIndex];
        T const minVal = std::numeric_limits<T>::min();
        T const maxVal = std::numeric_limits<T>::max();
        if ((attrValue >= minVal) && (attrValue <= maxVal)) {
            attr = static_cast<T>(attrValue);
            return;
        }
        throw std::invalid_argument(_context(__func__) + " failed for attribute=" + attrName + ", value=" +
                                    std::to_string(attrValue) + ", allowed range=[" + std::to_string(minVal) +
                                    "," + std::to_string(maxVal) + "], file=" + _fileName);
    }

    std::string const _fileName;
    std::vector<std::uint64_t> _taskAttributes;
};

}  // namespace lsst::qserv::util

#endif  // LSST_QSERV_UTIL_RESULTFILENAMEPARSER_H
