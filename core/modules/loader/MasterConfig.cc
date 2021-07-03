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
#include "loader/MasterConfig.h"

// System headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "util/ConfigStore.h"
#include "util/ConfigStoreError.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.loader.Config");
}


namespace lsst {
namespace qserv {
namespace loader {


MasterConfig::MasterConfig(util::ConfigStore const& configStore) {
    try {
        setFromConfig(configStore);
    } catch (util::ConfigStoreError const& e) {
        throw ConfigErr(ERR_LOC, std::string("MasterConfig ") + e.what());
    }
}


std::ostream& MasterConfig::dump(std::ostream &os) const {
    os << "(MasterConfig(" << header << "):";
    ConfigBase::dump(os);
    os << ")";
    return os;
}


}}} // namespace lsst::qserv::loader


