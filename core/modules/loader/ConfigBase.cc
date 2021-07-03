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
#include "loader/ConfigBase.h"

// System headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "util/ConfigStore.h"
#include "util/ConfigStoreError.h"

// Third-party headers
#include "boost/lexical_cast.hpp"


namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.Config");
}


namespace lsst {
namespace qserv {
namespace loader {


void ConfigElement::setFromConfig(util::ConfigStore const& cfgStore) {
    if (_required) {
        _value = cfgStore.getRequired(getFullKey());
    } else {
        _value = cfgStore.get(getFullKey(), _default);
    }
}


bool ConfigElement::verifyValueIsOfKind() {
    switch (_kind) {
        case STRING:
            return true;
        case INT:
            return isInteger();
        case FLOAT:
            return isFloat();
        default:
            return false;
    }
}


std::string ConfigElement::getFullKey() const {
    if (_header.empty()) return std::string(_key);
    return std::string(_header + "." + _key);
}


int ConfigElement::getInt() const {
    if (_kind != INT) {
        throw ConfigErr(ERR_LOC, "getInt called for non-integer " + dump());
    }
    return boost::lexical_cast<int>(_value);
}


double ConfigElement::getDouble() const {
    if (_kind != FLOAT) {
        throw ConfigErr(ERR_LOC, "getDouble called for non-float " + dump());
    }
    return boost::lexical_cast<double>(_value);
}


bool ConfigElement::isInteger() const {
    if (_kind != INT) return false;
    try {
        // lexical cast is more strict than std::stoi()
        getInt();
    } catch (boost::bad_lexical_cast const& exc) {
        return false;
    }
    return true;
}


bool ConfigElement::isFloat() const {
    if (_kind != FLOAT) return false;
    try {
        getDouble();
    } catch (boost::bad_lexical_cast const& exc) {
        return false;
    }
    return true;
}


std::string ConfigElement::kindToStr(Kind kind) {
    switch (kind) {
    case STRING:
        return "STRING";
    case INT:
        return "INT";
    case FLOAT:
        return "FLOAT";
    default:
        return "undefined Kind";
    }
}


std::ostream& ConfigElement::dump(std::ostream &os) const {
    os << "(key=" << getFullKey()
       << " val=" << _value
       << " req=" << _required
       << " kind=" << kindToStr(_kind)
       << " def=" << _default << ")";
    return os;
}


std::string ConfigElement::dump() const {
    std::ostringstream os;
    dump(os);
    return os.str();
}


std::ostream& operator<<(std::ostream &os, ConfigElement const& elem) {
    return elem.dump(os);
}


void ConfigBase::setFromConfig(util::ConfigStore const& configStore) {
    for (auto& elem:cfgList) {
        elem->setFromConfig(configStore);
        if (not elem->verifyValueIsOfKind()) {
            throw util::ConfigStoreError("Could not parse " + elem->dump());
        }
    }
}


std::ostream& ConfigBase::dump(std::ostream &os) const {
    os << "(ConfigBase: ";
    for (auto&& elem:cfgList) {
        os << *elem << " ";
    }
    os << ")";
    return os;
}


std::string ConfigBase::dump() const {
    std::stringstream os;
    dump(os);
    return os.str();
}


std::ostream& operator<<(std::ostream &os, ConfigBase const& cfg) {
    return cfg.dump(os);
}


}}} // namespace lsst::qserv::loader


