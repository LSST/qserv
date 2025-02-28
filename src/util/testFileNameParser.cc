// -*- LSST-C++ -*-
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

// System headers
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "qmeta/types.h"
#include "util/ResultFileNameParser.h"

// Boost unit test header
#define BOOST_TEST_MODULE ResultFileNameParser
#include <boost/test/unit_test.hpp>

// Third party headers
#include "boost/filesystem.hpp"

namespace fs = boost::filesystem;
namespace qmeta = lsst::qserv::qmeta;
namespace test = boost::test_tools;
namespace util = lsst::qserv::util;

BOOST_AUTO_TEST_SUITE(Suite)

BOOST_AUTO_TEST_CASE(ResultFileNameParserTest) {
    LOGS_INFO("ResultFileNameParserTest");

    util::ResultFileNameParser fileExpected;
    fileExpected.czarId = 1;
    fileExpected.queryId = 2;
    fileExpected.jobId = 3;
    fileExpected.chunkId = 4;
    std::string const fileNameNoExt =
            std::to_string(fileExpected.czarId) + "-" + std::to_string(fileExpected.queryId) + "-" +
            std::to_string(fileExpected.jobId) + "-" + std::to_string(fileExpected.chunkId);

    std::string const fileName = fileNameNoExt + util::ResultFileNameParser::fileExt;

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file(fileNameNoExt);
        BOOST_CHECK_EQUAL(file, fileExpected);
        BOOST_CHECK_EQUAL(file.czarId, fileExpected.czarId);
        BOOST_CHECK_EQUAL(file.queryId, fileExpected.queryId);
        BOOST_CHECK_EQUAL(file.jobId, fileExpected.jobId);
        BOOST_CHECK_EQUAL(file.chunkId, fileExpected.chunkId);
    });

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file(fileName);
        BOOST_CHECK_EQUAL(file, fileExpected);
    });

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file{fs::path(fileName)};
        BOOST_CHECK_EQUAL(file, fileExpected);
    });

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file("/" + fileName);
        BOOST_CHECK_EQUAL(file, fileExpected);
    });

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file("/base/" + fileName);
        BOOST_CHECK_EQUAL(file, fileExpected);
    });

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file("base/" + fileName);
        BOOST_CHECK_EQUAL(file, fileExpected);
    });

    BOOST_CHECK_NO_THROW({
        util::ResultFileNameParser const file(fs::path("/base/") / fileName);
        BOOST_CHECK_EQUAL(file, fileExpected);
    });

    BOOST_CHECK_THROW(
            { util::ResultFileNameParser const file("1-2-3-4" + fileName); }, std::invalid_argument);

    BOOST_CHECK_THROW(
            { util::ResultFileNameParser const file("a-2-3-4-5" + fileName); }, std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
