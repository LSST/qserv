// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2016 LSST Corporation.
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
#ifndef LSST_QSERV_QANA_SCANTABLEPLUGIN_H
#define LSST_QSERV_QANA_SCANTABLEPLUGIN_H

// Class header
#include "qana/QueryPlugin.h"

// Qserv headers
#include "protojson/ScanTableInfo.h"

namespace lsst::qserv::qana {

/// ScanTablePlugin is a query plugin that detects the "scan tables"
/// of a query. A scan table is a partitioned table that must be
/// scanned in order to answer the query. If the number of chunks
/// involved is less than a threshold number (2, currently), then the
/// scan table annotation is removed--the query is no longer
/// considered a "scanning" query because it involves a small piece of
/// the data set.
class ScanTablePlugin : public QueryPlugin {
public:
    // Types
    typedef std::shared_ptr<ScanTablePlugin> Ptr;

    explicit ScanTablePlugin(int interactiveChunkLimit) : _interactiveChunkLimit(interactiveChunkLimit) {}
    virtual ~ScanTablePlugin() {}

    void prepare() override {}

    void applyLogical(query::SelectStmt& stmt, query::QueryContext&) override;
    void applyFinal(query::QueryContext& context) override;

    /// Return the name of the plugin class for logging.
    std::string name() const override { return "ScanTablePlugin"; }

private:
    protojson::ScanInfo::Ptr _findScanTables(query::SelectStmt& stmt, query::QueryContext& context);
    protojson::ScanInfo::Ptr _scanInfo;
    int _interactiveChunkLimit;
};

}  // namespace lsst::qserv::qana

#endif  // LSST_QSERV_QANA_SCANTABLEPLUGIN_H
