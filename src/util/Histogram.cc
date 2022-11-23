// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013 LSST Corporation.
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
// Generic timer class

#include <cstdio>
#include <float.h>
#include <set>

#include "Histogram.h"
// qserv headers
#include "util/Bug.h"

// LSST headers
#include "lsst/log/Log.h"

using namespace std;

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.util.Histogram");
}

namespace lsst::qserv::util {

Histogram::Histogram(string const& label, std::vector<double> const& bucketVals) : _label(label) {
    // sort vector and remove duplicates.
    std::set<double> valSet;
    for (auto&& v : bucketVals) {
        valSet.insert(v);
    }
    for (auto&& v : valSet) {
        _buckets.emplace_back(v);
    }
}

string Histogram::addEntry(double val, string const& note) {
    auto now = CLOCK::now();
    return addEntry(now, val, note);
}

string Histogram::addEntry(TIMEPOINT stamp, double val, string const& note) {
    std::lock_guard<std::mutex> lock(_mtx);
    return _addEntry(stamp, val, note);
}

string Histogram::_addEntry(TIMEPOINT stamp, double val, string const& note) {
    _changeCountsBy(val, 1);
    _total += val;
    ++_totalCount;

    if (note.empty()) {
        return note;
    }
    return _getString(note);
}

void Histogram::_changeCountsBy(double val, int incr) {
    bool found = false;
    for (auto& bkt : _buckets) {
        if (val <= bkt.getMaxVal()) {
            bkt.count += incr;
            found = true;
            break;
        }
    }
    if (not found) {
        _overMaxCount += incr;
    }
}

double Histogram::getAvg() const {
    std::lock_guard<std::mutex> lock(_mtx);
    return _getAvg();
}

double Histogram::_getAvg() const {
    double avg = (_totalCount) ? _total / _totalCount : 0.0;
    return avg;
}

int Histogram::getBucketCount(size_t index) const {
    std::lock_guard<std::mutex> lock(_mtx);
    if (index > _buckets.size()) {
        LOGS(_log, LOG_LVL_ERROR, "Histogram::getBucketCount out of range index=" << index);
        return 0;
    }
    if (index == _buckets.size()) {
        return _overMaxCount;
    }
    return _buckets[index].count;
}

double Histogram::getBucketMaxVal(size_t index) const {
    std::lock_guard<std::mutex> lock(_mtx);
    if (index > _buckets.size()) {
        string eMsg = string("Histogram::getBucketCount out of range index=") + to_string(index);
        LOGS(_log, LOG_LVL_ERROR, eMsg);
        return std::numeric_limits<double>::max();
    }
    if (index == _buckets.size()) {
        return std::numeric_limits<double>::max();
    }
    return _buckets[index].getMaxVal();
}

string Histogram::getString(std::string const& note) {
    std::lock_guard<std::mutex> lock(_mtx);
    return _getString(note);
}

/// _mtx must be locked before calling this function.
///
string Histogram::_getString(std::string const& note) {
    stringstream os;

    os << _label << " " << note << " size=" << _totalCount << " total=" << _total << " avg=" << _getAvg()
       << " ";
    double maxB = -DBL_MAX;
    for (auto& bkt : _buckets) {
        os << " <" << bkt.getMaxVal() << "=" << bkt.count;
        maxB = bkt.getMaxVal();
    }
    os << " >" << maxB << "=" << _overMaxCount;
    return os.str();
}

nlohmann::json Histogram::getJson() const {
    std::lock_guard<std::mutex> lock(_mtx);
    nlohmann::json rJson = {
            {"HistogramId", _label}, {"avg", _getAvg()}, {"totalCount", _totalCount}, {"total", _total}};
    for (size_t j = 0; j < _buckets.size(); ++j) {
        auto const& bk = _buckets[j];
        rJson["buckets"][j] = {{"index", j}, {"maxVal", bk.getMaxVal()}, {"count", bk.count}};
    }

    auto sz = _buckets.size();
    rJson["buckets"][sz] = {
            {"index", sz}, {"maxVal", std::numeric_limits<double>::max()}, {"count", _overMaxCount}};
    return rJson;
}

std::string Histogram::getJsonStr() const {
    nlohmann::json rJson = getJson();
    stringstream os;
    os << rJson;
    return os.str();
}

HistogramRolling::HistogramRolling(string const& label, std::vector<double> const& bucketVals,
                                   std::chrono::milliseconds maxAgeMillis, size_t maxSize)
        : Histogram(label, bucketVals), _maxSize(maxSize), _maxAge(maxAgeMillis) {}

std::string HistogramRolling::addEntry(double val, std::string const& note) {
    auto now = CLOCK::now();
    return addEntry(now, val, note);
}

std::string HistogramRolling::addEntry(TIMEPOINT stamp, double val, std::string const& note) {
    std::lock_guard<std::mutex> lock(_mtx);

    string str = Histogram::_addEntry(stamp, val, note);

    if (_maxSize > 0) {
        _entries.emplace(stamp, val);
    }

    // remove old values.
    _checkEntries();

    return str;
}

size_t HistogramRolling::getSize() {
    std::lock_guard<std::mutex> lock(_mtx);
    return _entries.size();
}

void HistogramRolling::checkEntries() {
    std::lock_guard<std::mutex> lock(_mtx);
    _checkEntries();
}

void HistogramRolling::_checkEntries() {
    auto now = CLOCK::now();
    auto originalSize = _entries.size();
    while (true && !_entries.empty()) {
        Entry& head = _entries.front();
        auto age = now - head.stamp;
        if (age.count() > _maxAge.count() || _entries.size() > _maxSize) {
            auto val = head.val;
            _total -= val;
            _changeCountsBy(val, -1);
            _entries.pop();
            --_totalCount;
        } else {
            break;
        }
    }
    if (_entries.size() == 0 && originalSize != 0) {
        // clear values.
        for (auto& bkt : _buckets) {
            bkt.count = 0;
        }
        _overMaxCount = 0;
        _total = 0.0;
        _totalCount = 0;
    }
}

void HistogramRolling::setMaxSize(size_t maxSize) {
    std::lock_guard<std::mutex> lock(_mtx);
    _maxSize = maxSize;
    _checkEntries();
}

void HistogramRolling::setMaxAge(std::chrono::milliseconds maxAge) {
    std::lock_guard<std::mutex> lock(_mtx);
    _maxAge = maxAge;
    _checkEntries();
}

}  // namespace lsst::qserv::util
