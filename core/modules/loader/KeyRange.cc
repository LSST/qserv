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
#include "KeyRange.h"

#include <iostream>

// qserv headers
#include "loader/BufferUdp.h"
#include "loader/LoaderMsg.h"
#include "proto/loader.pb.h"

// LSST headers
#include "lsst/log/Log.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.StringRange");
}

namespace lsst {
namespace qserv {
namespace loader {


std::ostream& operator<<(std::ostream& os, NeighborsInfo const& ni) {
    os << "NeighborsInfo";
    os << " neighborLeft=" << (ni.neighborLeft == nullptr) ? "nullptr" : std::to_string(ni.neighborLeft->get());
    os << " neighborRight=" << (ni.neighborRight == nullptr) ? "nullptr" : std::to_string(ni.neighborRight->get());
    os << " recentAdds=" << ni.recentAdds;
    os << " keyCount=" << ni.keyCount;
    return os;
}

std::ostream& operator<<(std::ostream& os, KeyRange const& strRange) {
    os << "valid=" << strRange._valid
       << " min=" << strRange._min
       << " max=" << strRange._maxE
       << " unlimited=" << strRange._unlimited;
    return os;
}


void KeyRange::setAllInclusiveRange() {
    _min = CompositeKey(0,"");
    _maxE = CompositeKey(CompositeKey::maxIntVal(), "");
    _unlimited = true;
    setValid();
}


bool KeyRange::setMin(CompositeKey const& val) {
    if (not _unlimited && val >= _maxE) {
        _min = decrement(_maxE);
        return false;
    }
    _min = val;
    return true;
}


bool KeyRange::setMax(CompositeKey const& val, bool unlimited) {
    _unlimited = unlimited;
    if (unlimited) {
        if (val > _maxE) { _maxE = val; }
        return true;
    }
    if (val < _min) {
        _maxE = increment(_min);
        return false;
    }
    _maxE = val;
    return true;
}


bool KeyRange::setMinMax(CompositeKey const& vMin, CompositeKey const& vMax, bool unlimited) {
    _unlimited = unlimited;
    if (!unlimited && vMin > vMax) {
        return false;
    }
    _unlimited = unlimited;
    if (_unlimited) {
        _min = vMin;
        _maxE = std::max(vMax, _min); // max is irrelevant at this point
    } else {
        _min = vMin;
        _maxE = vMax;
    }
    setValid();
    return true;
}




std::string KeyRange::incrementString(std::string const& str, char appendChar) {
    std::string output(str);
    if (output.empty()) {
        output += appendChar;
    }
    size_t pos = output.size() - 1;
    char lastChar = output[pos];
    if (lastChar < 'z') {
        ++lastChar;
        output[pos] = lastChar;
    } else {
        output += appendChar;
    }
    return output;
}


CompositeKey KeyRange::increment(CompositeKey const& key, char appendChar) {
    CompositeKey outKey(key.kInt, incrementString(key.kStr, appendChar));
    return outKey;
}


std::string KeyRange::decrementString(std::string const& str, char minChar) {
    if (str.empty()) {
        return std::string();
    }
    std::string output(str);
    size_t pos = output.size() - 1;
    char lastChar = output[pos];
    --lastChar;
    if (lastChar > minChar) {
        output[pos] = lastChar;
        return output;
    } else {
        output.erase(pos, 1);
    }
    return output;
}


CompositeKey KeyRange::decrement(CompositeKey const& key, char minChar) {
    CompositeKey outK(key);
    if (outK.kStr.empty()) {
        if (outK.kInt > 0) --outK.kInt;
        return outK;
    }
    outK.kStr = decrementString(outK.kStr, minChar);
    return outK;
}


void KeyRange::loadProtoRange(proto::WorkerRange& protoRange) {
    protoRange.set_valid(_valid);
    protoRange.set_minint(_min.kInt);
    protoRange.set_minstr(_min.kStr);
    protoRange.set_maxint(_maxE.kInt);
    protoRange.set_maxstr(_maxE.kStr);
    protoRange.set_maxunlimited(_unlimited);
}


void ProtoHelper::workerKeysInfoExtractor(BufferUdp& data, uint32_t& wId, NeighborsInfo& nInfo, KeyRange& keyRange) {
    auto funcName = "CentralWorker::_workerKeysInfoExtractor";
    LOGS(_log, LOG_LVL_DEBUG, funcName);
    auto protoItem = StringElement::protoParse<proto::WorkerKeysInfo>(data);
    if (protoItem == nullptr) {
        throw LoaderMsgErr(ERR_LOC, "protoItem nullptr");
    }

    wId = protoItem->wid();
    nInfo.keyCount = protoItem->mapsize();
    nInfo.recentAdds = protoItem->recentadds();
    proto::WorkerRange protoRange = protoItem->range();
    bool valid = protoRange.valid();
    if (valid) {
        CompositeKey minKey(protoRange.minint(), protoRange.minstr());
        CompositeKey maxKey(protoRange.maxint(), protoRange.maxstr());
        bool unlimited = protoRange.maxunlimited();
        keyRange.setMinMax(minKey, maxKey, unlimited);
    }
    proto::Neighbor protoLeftNeigh = protoItem->left();
    nInfo.neighborLeft->update(protoLeftNeigh.wid());
    proto::Neighbor protoRightNeigh = protoItem->right();
    nInfo.neighborRight->update(protoRightNeigh.wid());
}


}}} // namespace lsst::qserv::loader

