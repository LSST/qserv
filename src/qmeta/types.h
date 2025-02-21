/*
 * LSST Data Management System
 * Copyright 2015 AURA/LSST.
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
 * see <https://www.lsstcorp.org/LegalNotices/>.
 */
#ifndef LSST_QSERV_QMETA_TYPES_H
#define LSST_QSERV_QMETA_TYPES_H

// System headers
#include <cstdint>
#include <string>

// Third-party headers

// Qserv headers
#include "global/intTypes.h"

namespace lsst::qserv::qmeta {

/*
 * typedefs for commonly used types.
 */

/// Typedef for Czar ID in query metadata.
typedef CzarIdType CzarId;  // uint32_t  TODO:UJ Replace qmeta::CzarId with global

}  // namespace lsst::qserv::qmeta

#endif  // LSST_QSERV_QMETA_TYPES_H
