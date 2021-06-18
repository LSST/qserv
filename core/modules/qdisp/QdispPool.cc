// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 AURA/LSST.
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
#include "qdisp/QdispPool.h"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "util/common.h"
#include "util/InstanceCount.h" //&&&

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qdisp.QdispPool");
}

namespace lsst {
namespace qserv {
namespace qdisp {

///< @Return true if the queue could be added.
bool PriorityQueue::addPriQueue(int priority, int minRunning, int maxRunning) {
    std::lock_guard<std::mutex> lock(_mtx);
    auto q = std::make_shared<PriQ>(priority, minRunning, maxRunning);
    //std::pair<int, PriQ::Ptr> item(priority, q);
    auto item = std::make_pair(priority, q);
    auto ret = _queues.insert(item);
    if (!ret.second) {
        LOGS(_log, LOG_LVL_ERROR, "Failed addPriQueue priority=" << priority <<
                                  " minRunning=" << minRunning);
    }
    return ret.second;
}


/// The pool needs to be able to place commands in this queue for shutdown.
void PriorityQueue::queCmd(util::Command::Ptr const& cmd) {
    {
        std::lock_guard<std::mutex> lock(_mtx);
        auto iter = _queues.find(_defaultPriority);
        if (iter == _queues.end()) {
            throw Bug("PriorityQueue default priority queue not found a!");
        }
        iter->second->queCmd(cmd);
        _changed = true;
    }
    _cv.notify_one();
}


void PriorityQueue::queCmd(PriorityCommand::Ptr const& cmd, int priority) {
    {
        //util::InstanceCount ic("PQ:queCmd&&&");
        std::lock_guard<std::mutex> lock(_mtx);
        auto iter = _queues.find(priority);
        if (iter == _queues.end()) {
            // give it the default priority
            LOGS(_log, LOG_LVL_WARN, "queCmd invalid priority=" << priority <<
                                     " using default priority=" << _defaultPriority);
            iter = _queues.find(_defaultPriority);
            if (iter == _queues.end()) {
                throw Bug("PriorityQueue default priority queue not found b!");
            }
        }
        cmd->_priority = priority;
        //LOGS (_log, LOG_LVL_INFO, "&&&priQue p=" << priority << _statsStr());
        iter->second->queCmd(cmd);
        LOGS (_log, LOG_LVL_DEBUG, "priQue p=" << priority << _statsStr());
        _changed = true;
    }
    _cv.notify_one();
    _cv.notify_one(); //&&&
}


std::atomic<unsigned int> localLogLimiter(0);

util::Command::Ptr PriorityQueue::getCmd(bool wait){
    //util::InstanceCount ic("PQ:getCmd&&&");
    util::Command::Ptr ptr;
    std::unique_lock<std::mutex> uLock(_mtx);
    while (true) {
        _changed = false;
        ++localLogLimiter;
        // Log this every once in while to INFO so there's some idea of system
        // load without generating crushing amounts of log messages.
        if (true) { //&&& localLogLimiter % 500 == 0) { // &&& revert
            LOGS(_log, LOG_LVL_INFO, "priQueGet " << _statsStr());
        } else {
            LOGS(_log, LOG_LVL_DEBUG, "priQueGet " << _statsStr());
        }

        /// Make sure minimum number of jobs running per priority.
        if (!_shuttingDown) {
            // If shutting down, this could prevent all jobs from completing.
            // Goes from highest to lowest priority queue
            for (auto const& elem : _queues) {
                PriQ::Ptr const& que = elem.second;
                if (que->running < que->getMinRunning()) {
                    ptr = que->getCmd(false); // no wait
                    if (ptr != nullptr) {
                        _changed = true;
                        _cv.notify_one();
                        return ptr;
                    }
                }
            }
        }

        // Since all the minimums are met, just run the first command found.
        for (auto const& elem : _queues) {
            PriQ::Ptr const& que = elem.second;
            // If this queue has no running threads, or
            if (que->running < que->getMaxRunning()) {
                ptr = que->getCmd(false); // no wait
                if (ptr != nullptr) {
                    _changed = true;
                    _cv.notify_one();
                    _cv.notify_one(); //&&&
                    return ptr;
                }
            }
        }

        // If nothing was found, wait or return nullptr.
        if (wait) {
            //util::InstanceCount icWait("PQ:getCmd&&&WAIT");
            LOGS (_log, LOG_LVL_DEBUG, "getCmd wait " << _statsStr());
            _cv.wait(uLock, [this](){ return _changed; });
        } else {
            return ptr;
        }
    }
}


void PriorityQueue::prepareShutdown() {
    std::lock_guard<std::mutex> lock(_mtx);
    _shuttingDown = true;
}


void PriorityQueue::_incrDecrRunningCount(util::Command::Ptr const& cmd, int incrDecr) {
    std::lock_guard<std::mutex> lock(_mtx);
    PriorityCommand::Ptr priCmd = std::dynamic_pointer_cast<PriorityCommand>(cmd);
    if (priCmd != nullptr) {
        int priority = priCmd->_priority;
        auto iter = _queues.find(priority);
        if (iter != _queues.end()) {
            iter->second->running += incrDecr;
            return;
        }
    } else if (cmd != nullptr) {
        // Non-PriorityCommands go on the default queue.
        auto iter = _queues.find(_defaultPriority);
        if (iter != _queues.end()) {
            iter->second->running += incrDecr;
        }
    }
}


void PriorityQueue::commandStart(util::Command::Ptr const& cmd) {
    // Increase running count by 1
    _incrDecrRunningCount(cmd, 1);
}


void PriorityQueue::commandFinish(util::Command::Ptr const& cmd) {
    // Reduce running count by 1
    _incrDecrRunningCount(cmd, -1);
}


std::vector<PriorityQueue::PriQ::Stats> PriorityQueue::stats() const {
    std::lock_guard<std::mutex> const lock(_mtx);
    return _stats();
}


std::vector<PriorityQueue::PriQ::Stats> PriorityQueue::_stats() const {
    std::vector<PriorityQueue::PriQ::Stats> result;
    for (auto const& elem: _queues)   {
        PriQ::Ptr const& queue = elem.second;
        result.push_back(queue->stats());
    }
    return result;
}


std::string PriorityQueue::_statsStr() const {
    std::stringstream os;
    for (auto const& queueStats: _stats()) {
        os << "(pr=" << queueStats.priority
           << ":sz=" << queueStats.size
           << ":r="  << queueStats.running << ")";
    }
    return os.str();
}


QdispPool::QdispPool(int poolSize, int largestPriority, std::vector<int> const& maxRunSizes,
                      std::vector<int> const& minRunningSizes) {
    std::stringstream os;
    os << "poolSize(max " << maxPoolSize() << ")=" << poolSize
       << " maxPriority(1 to " << defaultPriority() - 2 << ")=" << largestPriority << " maxRunSizes=" << util::prettyCharList(maxRunSizes)
       << " minRunningSizes=" << util::prettyCharList(minRunningSizes);
    if (poolSize < 1 || poolSize > maxPoolSize()
        || largestPriority < 0
        || maxRunSizes.size() <static_cast<size_t>(largestPriority) + 1
        || largestPriority > defaultPriority() - 2) {
        LOGS(_log, LOG_LVL_ERROR, "QdispPool invalid paramater " << os.str());
        throw std::invalid_argument(os.str());
    }

    LOGS(_log, LOG_LVL_INFO, "QdispPool creating " << os.str());
    _prQueue = std::make_shared<PriorityQueue>(defaultPriority(), 1, 1); // default (lowest) priority.
    for (unsigned int pri = 0; pri <= static_cast<unsigned int>(largestPriority); ++pri) {
        size_t const minRun = minRunningSizes.size() > pri ? minRunningSizes[pri] : 1;
        size_t const maxRun = maxRunSizes.size() > pri ? maxRunSizes[pri] : 1;
        LOGS(_log, LOG_LVL_INFO, "creating priQ pri=" << pri << " min=" << minRun << " max=" << maxRun);
        _prQueue->addPriQueue(pri, minRun, maxRun);
    }
    // This pool does not kick threads out when they take time (but little CPU) to process,
    // so maxPoolThreads is just slightly larger than poolSize.
    _pool = util::ThreadPool::newThreadPool(poolSize, _prQueue);
}


QdispPool::QdispPool(bool unitTest) {
    if (not unitTest) {
        std::string msg("QdispPool::QdispPool(bool unitTest) "
                        "This constructor is only meant for use with unit tests.");
        LOGS(_log, LOG_LVL_ERROR,
             "QdispPool::QdispPool(bool unitTest) This constructor is only meant for use with unit tests.");
        throw std::invalid_argument(msg);
    } else {
        _prQueue = std::make_shared<PriorityQueue>(100, 1, 1); // default (lowest) priority.
        unsigned int poolSize = 50;
        _pool = util::ThreadPool::newThreadPool(poolSize, _prQueue);
        _prQueue->addPriQueue(0, 1, 3);  // Highest priority - interactive queries
        _prQueue->addPriQueue(1, 1, 3);  // Outgoing shared scan queries.
        _prQueue->addPriQueue(2, 1, 3); // FAST queries (Object table)
        _prQueue->addPriQueue(3, 1, 3); // MEDIUM queries (Source table)
        _prQueue->addPriQueue(4, 1, 3); // SLOW queries (Object Extra table)
        _prQueue->addPriQueue(5, 1, 3); // FAST large results
        _prQueue->addPriQueue(6, 1, 3); // MEDIUM large results
        _prQueue->addPriQueue(7, 1, 3);  // Everything else (slow things)
    }
}

}}} // namespace lsst:qserv::qdisp
