// -*- LSST-C++ -*-

// Class header
#include "util/InstanceCount.h"

// System Headers

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {  // File-scope helpers

LOG_LOGGER _log = LOG_GET("lsst.qserv.util.InstanceCount");

}  // namespace

namespace lsst::qserv::util {

std::map<std::string, int> InstanceCount::_instances;
std::recursive_mutex InstanceCount::_mx;

InstanceCount::InstanceCount(std::string const& className) : _className{className} { _increment("con"); }

InstanceCount::InstanceCount(InstanceCount const& other) : _className{other._className} { _increment("cpy"); }

InstanceCount::InstanceCount(InstanceCount&& origin) : _className{origin._className} { _increment("mov"); }

void InstanceCount::_increment(std::string const& source) {
    std::lock_guard<std::recursive_mutex> lg(_mx);
    std::pair<std::string const, int> entry(_className, 0);
    auto ret = _instances.insert(entry);
    auto iter = ret.first;
    iter->second += 1;
    LOGS(_log, LOG_LVL_TRACE, "InstanceCount " << source << " " << iter->first << "=" << iter->second);
}

uint16_t instanceDestructLogLimiter = 0;

InstanceCount::~InstanceCount() {
    std::lock_guard<std::recursive_mutex> lg(_mx);
    ++instanceDestructLogLimiter;
    auto iter = _instances.find(_className);
    if (iter != _instances.end()) {
        iter->second -= 1;
        LOGS(_log, LOG_LVL_TRACE, "~InstanceCount " << iter->first << "=" << iter->second << " : " << *this);
        if (instanceDestructLogLimiter % 1000 == 0) {
            LOGS(_log, LOG_LVL_DEBUG, "~InstanceCount brief " << *this);
        }
        if (iter->second == 0) {
            _instances.erase(_className);
        }
    } else {
        LOGS(_log, LOG_LVL_ERROR, "~InstanceCount " << _className << " was not found! : " << *this);
    }
}

int InstanceCount::getCount() {
    std::lock_guard<std::recursive_mutex> lg(_mx);
    auto iter = _instances.find(_className);
    if (iter == _instances.end()) {
        return 0;
    }
    return iter->second;
}

std::ostream& operator<<(std::ostream& os, InstanceCount const& instanceCount) {
    std::lock_guard<std::recursive_mutex> lg(instanceCount._mx);
    for (auto const& entry : instanceCount._instances) {
        if (entry.second != 0) {
            os << entry.first << "=" << entry.second << " ";
        }
    }
    return os;
}

}  // namespace lsst::qserv::util
