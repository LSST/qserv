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
#include <string>
#include <tuple>
#include <unistd.h>

// Third-party headers
#include "boost/asio.hpp"

// Boost unit test header
#define BOOST_TEST_MODULE Czar_1
#include <boost/test/unit_test.hpp>

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "czar/CzarChunkMap.h"

namespace test = boost::test_tools;
using namespace lsst::qserv;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.czar.testCzar");
}

using namespace std;

BOOST_AUTO_TEST_SUITE(Suite)

BOOST_AUTO_TEST_CASE(CzarChunkMap) {
    // Each chunk only occurs on one worker
    string test1 = R"(
    {
      "ce1c1b79-e6fb-11ee-a46b-0242c0a80308":
           {"qcase01":
              {"Object":[[1234567890,0],[6630,1460],[6800,6068],[6968,1000],[6971,2716],[7140,4556],[7310,2144],[7648,1568]],  
               "Source":[[1234567890,0],[6630,37084],[6800,163888],[6968,33044],[6971,67016],[7140,145300],[7310,83872],[7648,30096]]
              },
            "qcase02":
              {"Object":[[1234567890,0],[7310,0]],
               "Source":[[1234567890,0],[7310,0]]
              },
            "qcase03":
              {"RefDeepSrcMatch":[[1234567890,0],[7165,76356]],
               "RefObject":[[1234567890,0],[7165,119616]],
               "RunDeepForcedSource":[[1234567890,0],[7165,130617531]],
               "RunDeepSource":[[1234567890,0],[7165,578396]]
              }
           },
      "ddc3f1b9-e6fb-11ee-a46b-0242c0a80304":
           {"qcase01":
              {"Object":[[1234567890,0],[6631,1612],[6801,4752],[6970,5780],[7138,3212],[7308,2144],[7478,4608]],
               "Source":[[1234567890,0],[6631,45724],[6801,123940],[6970,151660],[7138,97252],[7308,56784],[7478,99304]]
              },
            "qcase02":
              {"Object":[[1234567890,0],[7480,1055000]],
               "Source":[[1234567890,0],[7480,2259419]]
              },
            "qcase03":
              {"RefDeepSrcMatch":[[1234567890,0],[6995,7728]],
               "RefObject":[[1234567890,0],[6995,10920]],
               "RunDeepForcedSource":[[1234567890,0],[6995,11708834]],
               "RunDeepSource":[[1234567890,0],[6995,58604]]
              }
           }
    }
    )";

    /// 3 workers, each containing all chunks.
    string test2 = R"(
    {
      "ce1c1b79-e6fb-11ee-a46b-0242c0a80308":
           {"qcase01":
              {"Object":[[1234567890,0],[6631,1612],[6801,4752],[6970,5780],[7138,3212],[7308,2144],[7478,4608],
                         [6630,1460],[6800,6068],[6968,1000],[6971,2716],[7140,4556],[7310,2144],[7648,1568]],
               "Source":[[1234567890,0],[6631,45724],[6801,123940],[6970,151660],[7138,97252],[7308,56784],[7478,99304],
                         [6630,37084],[6800,163888],[6968,33044],[6971,67016],[7140,145300],[7310,83872],[7648,30096]]
              },
            "qcase02":
              {"Object":[[1234567890,0],[7480,1055000],[7310,0]],
               "Source":[[1234567890,0],[7480,2259419],[7310,0]]
              },
            "qcase03":
              {"RefDeepSrcMatch":[[1234567890,0],[6995,7728],[7165,76356]],
               "RefObject":[[1234567890,0],[6995,10920],[7165,119616]],
               "RunDeepForcedSource":[[1234567890,0],[6995,11708834],[7165,130617531]],
               "RunDeepSource":[[1234567890,0],[6995,58604],[7165,578396]]
              }
           },
      "brnd1b79-e6fb-11ee-a46b-0242c0a80308":
           {"qcase01":
              {"Object":[[1234567890,0],[6631,1612],[6801,4752],[6970,5780],[7138,3212],[7308,2144],[7478,4608],
                         [6630,1460],[6800,6068],[6968,1000],[6971,2716],[7140,4556],[7310,2144],[7648,1568]],
               "Source":[[1234567890,0],[6631,45724],[6801,123940],[6970,151660],[7138,97252],[7308,56784],[7478,99304],
                         [6630,37084],[6800,163888],[6968,33044],[6971,67016],[7140,145300],[7310,83872],[7648,30096]]
              },
            "qcase02":
              {"Object":[[1234567890,0],[7480,1055000],[7310,0]],
               "Source":[[1234567890,0],[7480,2259419],[7310,0]]
              },
            "qcase03":
              {"RefDeepSrcMatch":[[1234567890,0],[6995,7728],[7165,76356]],
               "RefObject":[[1234567890,0],[6995,10920],[7165,119616]],
               "RunDeepForcedSource":[[1234567890,0],[6995,11708834],[7165,130617531]],
               "RunDeepSource":[[1234567890,0],[6995,58604],[7165,578396]]
              }
           },
      "ddc3f1b9-e6fb-11ee-a46b-0242c0a80304":
           {"qcase01":
              {"Object":[[1234567890,0],[6631,1612],[6801,4752],[6970,5780],[7138,3212],[7308,2144],[7478,4608],
                         [6630,1460],[6800,6068],[6968,1000],[6971,2716],[7140,4556],[7310,2144],[7648,1568]],
               "Source":[[1234567890,0],[6631,45724],[6801,123940],[6970,151660],[7138,97252],[7308,56784],[7478,99304],
                         [6630,37084],[6800,163888],[6968,33044],[6971,67016],[7140,145300],[7310,83872],[7648,30096]]
              },
            "qcase02":
              {"Object":[[1234567890,0],[7480,1055000],[7310,0]],
               "Source":[[1234567890,0],[7480,2259419],[7310,0]]
              },
            "qcase03":
              {"RefDeepSrcMatch":[[1234567890,0],[6995,7728],[7165,76356]],
               "RefObject":[[1234567890,0],[6995,10920],[7165,119616]],
               "RunDeepForcedSource":[[1234567890,0],[6995,11708834],[7165,130617531]],
               "RunDeepSource":[[1234567890,0],[6995,58604],[7165,578396]]
              }
           }
    }
    )";

    auto jsTest1 = nlohmann::json::parse(test1);
    auto [chunkMapPtr, wcMapPtr] = czar::CzarChunkMap::makeNewMaps(jsTest1);
    czar::CzarChunkMap::verify(*chunkMapPtr, *wcMapPtr);  // Throws on failure.
    LOGS(_log, LOG_LVL_DEBUG, "CzarChunkMap test 1 passed");

    auto jsTest2 = nlohmann::json::parse(test2);
    tie(chunkMapPtr, wcMapPtr) = czar::CzarChunkMap::makeNewMaps(jsTest2);
    czar::CzarChunkMap::verify(*chunkMapPtr, *wcMapPtr);  // Throws on failure.
    LOGS(_log, LOG_LVL_DEBUG, "CzarChunkMap test 2 passed");
}

BOOST_AUTO_TEST_SUITE_END()
