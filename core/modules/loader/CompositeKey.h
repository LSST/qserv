// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2018 LSST Corporation.
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
 *
 */
#ifndef LSST_QSERV_LOADER_COMPOSITEKEY_H
#define LSST_QSERV_LOADER_COMPOSITEKEY_H

// system headers
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>


namespace lsst {
namespace qserv {
namespace loader {


/// A key consisting of an unsigned 64 bit integer and a std::string with support for comparisons.
/// The integer component is compared before the string component.
class CompositeKey {
public:
    CompositeKey(uint64_t ki, std::string const& ks) : kInt(ki), kStr(ks) {}
    explicit CompositeKey(uint64_t ki) : CompositeKey(ki, "") {}
    explicit CompositeKey(std::string const& ks) : CompositeKey(0, ks) {}
    CompositeKey(CompositeKey const& ck) : CompositeKey(ck.kInt, ck.kStr) {}
    CompositeKey() : CompositeKey(0, "") {}
    ~CompositeKey() = default;

    static uint64_t maxIntVal() { return std::numeric_limits<uint64_t>::max(); }

    CompositeKey& operator=(CompositeKey const& other) {
        if (this != &other) {
            kInt = other.kInt;
            kStr = other.kStr;
        }
        return *this;
    }

    /// Smallest possible value for a CompositeKey (0,"")
    static CompositeKey const minValue;

    bool operator<(CompositeKey const& other) const {
        return std::tie(kInt, kStr) < std::tie(other.kInt, other.kStr);
    }

    bool operator>(CompositeKey const& other) const {
        return other < *this;
    }

    bool operator==(CompositeKey const& other) const {
        return (kInt == other.kInt) && (kStr == other.kStr);
    }

    bool operator!=(CompositeKey const& other) const {
        return !(*this == other);
    }

    bool operator<=(CompositeKey const& other) const {
        return !(*this > other);
    }

    bool operator>=(CompositeKey const& other) const {
        return !(*this < other);
    }

    void dump(std::ostream& os) const;
    std::string dump() const ;

    uint64_t    kInt;
    std::string kStr;
};

std::ostream& operator<<(std::ostream& os, CompositeKey const& cKey);

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_COMPOSITEKEY_H
