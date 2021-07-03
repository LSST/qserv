// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2019 AURA/LSST.
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
#include <iostream>
#include <unistd.h>

// Third-party headers
#include "boost/lexical_cast.hpp"

// qserv headers
#include "loader/CentralClient.h"
#include "loader/ClientConfig.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.appClient");
}

using namespace lsst::qserv::loader;
using  boost::asio::ip::udp;

typedef std::list<KeyInfoData::Ptr> KeyInfoDataList;

/// @return true if the list is empty.
bool keyInsertListClean(KeyInfoDataList& kList, int& successCount, int& failedCount) {
    for(auto iter=kList.begin(); iter != kList.end();) {
        if (*iter == nullptr || (*iter)->isFinished()) {
            KeyInfoData::Ptr const& kPtr = *iter;
            if (kPtr->success) {
                ++successCount;
            } else {
                ++failedCount;
                LOGS(_log, LOG_LVL_WARN, "insert failed " << *kPtr);
            }
            iter = kList.erase(iter);
        } else {
            ++iter;
        }
    }
    return kList.empty();
}


/// Get a repeatable value for the chunk and subchunk numbers. It's arbitrary for
/// the test as there just needs to be some check that what was written in for
/// the key is the same as what was read
int calcChunkFrom(uint64_t j) {
    return j % 10000;
}
int calcSubchunkFrom(uint64_t j) {
    return j % 100;
}


KeyInfoData::Ptr clientAdd(CentralClient& central, uint64_t j) {
    CompositeKey cKey(j);
    int chunk = calcChunkFrom(j);
    int subchunk = calcSubchunkFrom(j);
    return central.keyInsertReq(cKey, chunk, subchunk);
}


/// @return true if the list is empty.
bool keyLookupListClean(KeyInfoDataList& kList, int& successCount, int& failedCount) {
    for(auto iter=kList.begin(); iter != kList.end();) {
        if (*iter == nullptr || (*iter)->isFinished()) {
            KeyInfoData::Ptr const& kPtr = *iter;
            if (kPtr->success) {
                // check the values
                uint64_t j = kPtr->key.kInt;
                // expected chunk and subchunk values.
                int expChunk = calcChunkFrom(j);
                int expSubchunk = calcSubchunkFrom(j);
                if (kPtr->chunk == expChunk && kPtr->subchunk == expSubchunk) {
                    ++successCount;
                } else {
                    ++failedCount;
                    LOGS(_log, LOG_LVL_WARN, "lookup failed, bad values, expected c=" << expChunk <<
                         " sc=" << expSubchunk << " found=" << *kPtr);
                }
            } else {
                ++failedCount;
                LOGS(_log, LOG_LVL_WARN, "lookup failed " << *kPtr);
            }
            iter = kList.erase(iter);
        } else {
            ++iter;
        }
    }
    return kList.empty();
}


KeyInfoData::Ptr clientAddLookup(CentralClient& central, uint64_t j) {
    CompositeKey cKey(j);
    return central.keyLookupReq(cKey);
}


int main(int argc, char* argv[]) {
    std::string cCfgFile("core/modules/loader/config/client1.cnf");
    if (argc < 3) {
        LOGS(_log, LOG_LVL_ERROR, "usage: appClientNum <startingNumber> <endingNumber> <optional config file name>");
        return 1;
    }
    uint64_t numStart = boost::lexical_cast<uint64_t>(argv[1]);
    uint64_t numEnd   = boost::lexical_cast<uint64_t>(argv[2]);
    if (argc > 3) {
        cCfgFile = argv[3];
    }
    LOGS(_log, LOG_LVL_INFO, "start=" << numStart << " end=" << numEnd << " cCfg=" << cCfgFile);
    if (numEnd == 0) {
        LOGS(_log, LOG_LVL_ERROR, "end cannot equal 0");
        return 1;
    }


    std::string const ourHost = boost::asio::ip::host_name();
    boost::asio::io_service ioService;

    ClientConfig cCfg(cCfgFile);
    CentralClient cClient(ioService, ourHost, cCfg);
    try {
        cClient.start();
    } catch (boost::system::system_error const& e) {
        LOGS(_log, LOG_LVL_ERROR, "cWorker.start() failed e=" << e.what());
        return 1;
    }

    cClient.runServer();

    KeyInfoDataList kList;
    int successCount = 0;
    int failedCount = 0;
    int totalKeyCount = 0;

    TimeOut::TimePoint insertBegin = TimeOut::Clock::now();

    if (numEnd >= numStart) {
        totalKeyCount = (numEnd - numStart) + 1;
        for (uint64_t j=numStart; j<=numEnd; ++j) {
            kList.push_back(clientAdd(cClient, j));
            // occasionally trim the list
            if (j%10000 == 0) keyInsertListClean(kList, successCount, failedCount);
        }
    } else {
        totalKeyCount = (numStart - numEnd) + 1;
        for (uint64_t j=numStart; j>=numEnd; --j) {
            kList.push_back(clientAdd(cClient, j));
            // occasionally trim the list
            if (j%10000 == 0) keyInsertListClean(kList, successCount, failedCount);
        }
    }

    int count = 0;
    // If all the requests are done, the list should be empty.
    // Wait up to 1 second per 1000 keys. (System does a bit better than 1000keys per second.)
    int waitForKeysCount = totalKeyCount/1000;
    int maxWaitCount = 16; // minimum wait to allow for 3 or 4 retries.
    if (waitForKeysCount > maxWaitCount) maxWaitCount = waitForKeysCount;
    while (!keyInsertListClean(kList, successCount, failedCount) && count < waitForKeysCount) {
        LOGS(_log, LOG_LVL_INFO, "waiting for inserts to finish count=" << count);
        sleep(1);
        ++count;
    }


    if (!kList.empty()) {
        LOGS(_log, LOG_LVL_WARN, "kList not empty, size=" << kList.size());
        std::stringstream ss;
        for (auto kPtr:kList) {
            ss << "elem=" << *kPtr << "\n";
        }
        LOGS(_log, LOG_LVL_WARN, ss.str());
    }

    if (!kList.empty() || failedCount > 0) {
        LOGS(_log, LOG_LVL_ERROR, "FAILED to insert all elements. success=" << successCount <<
                " failed=" << failedCount << " size=" << kList.size());
        return 1;
    }

    LOGS(_log, LOG_LVL_INFO, "inserted all elements. success=" << successCount <<
            " failed=" << failedCount << " size=" << kList.size());

    TimeOut::TimePoint insertEnd = TimeOut::Clock::now();

    // Lookup answers
    auto nStart = numStart;
    auto nEnd = numEnd;
    if (nEnd < nStart) {
        nStart = numEnd;
        nEnd = numStart;
    }
    successCount = 0;
    failedCount = 0;
    for (uint64_t j=nStart; j<=nEnd; ++j) {
        kList.push_back(clientAddLookup(cClient, j));
        // occasionally trim the list
        if (j%10000 == 0) keyLookupListClean(kList, successCount, failedCount);
    }

    count = 0;
    // If all the requests are done, the list should be empty.
    // About 1 second per 1000 keys)
    while (!keyLookupListClean(kList, successCount, failedCount) && count < waitForKeysCount) {
        LOGS(_log, LOG_LVL_INFO, "waiting for lookups to finish count=" << count);
        sleep(1);
        ++count;
    }

    if (!kList.empty()) {
        LOGS(_log, LOG_LVL_WARN, "kList not empty, size=" << kList.size());
        std::stringstream ss;
        for (auto kPtr:kList) {
            ss << "elem=" << *kPtr << "\n";
        }
        LOGS(_log, LOG_LVL_WARN, ss.str());
    }

    if (!kList.empty() || failedCount > 0) {
        LOGS(_log, LOG_LVL_ERROR, "FAILED to lookup all elements. success=" << successCount <<
                " failed=" << failedCount << " size=" << kList.size());
        return 1;
    }

    LOGS(_log, LOG_LVL_INFO, "lookup all elements. success=" << successCount <<
            " failed=" << failedCount << " size=" << kList.size());

    TimeOut::TimePoint lookupEnd = TimeOut::Clock::now();

    LOGS(_log, LOG_LVL_INFO, "inserts seconds=" <<
         std::chrono::duration_cast<std::chrono::seconds>(insertEnd - insertBegin).count());
    LOGS(_log, LOG_LVL_INFO, "lookups seconds=" <<
         std::chrono::duration_cast<std::chrono::seconds>(lookupEnd - insertEnd).count());
    ioService.stop();
    LOGS(_log, LOG_LVL_INFO, "client DONE");
    return 0;
}


