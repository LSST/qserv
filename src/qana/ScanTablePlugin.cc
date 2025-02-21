// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2013-2016 AURA/LSST.
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
/**
 * @file
 *
 * @brief ScanTablePlugin implementation
 *
 * @author Daniel L. Wang, SLAC
 */

// Class header
#include "qana/ScanTablePlugin.h"

// System headers
#include <algorithm>

// Third-party headers

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "czar/Czar.h"
#include "global/stringTypes.h"
#include "query/ColumnRef.h"
#include "query/FromList.h"
#include "query/QueryContext.h"
#include "query/SelectList.h"
#include "query/SelectStmt.h"
#include "query/WhereClause.h"

namespace {
LOG_LOGGER _log = LOG_GET("lsst.qserv.qana.ScanTablePlugin");
}

namespace lsst::qserv::qana {

////////////////////////////////////////////////////////////////////////
// ScanTablePlugin implementation
////////////////////////////////////////////////////////////////////////
void ScanTablePlugin::applyLogical(query::SelectStmt& stmt, query::QueryContext& context) {
    _scanInfo = _findScanTables(stmt, context);
    context.scanInfo = _scanInfo;
}

void ScanTablePlugin::applyFinal(query::QueryContext& context) {
    int const scanThreshold = _interactiveChunkLimit;
    if (context.chunkCount < scanThreshold) {
        context.scanInfo->infoTables.clear();
        context.scanInfo->scanRating = 0;
        LOGS(_log, LOG_LVL_INFO, "ScanInfo Squash full table scan tables: <" << scanThreshold << " chunks.");
    }
}

struct getPartitioned : public query::TableRef::FuncC {
    getPartitioned(StringPairVector& sVector_) : sList(sVector_) {}
    virtual void operator()(query::TableRef const& tRef) {
        StringPair entry(tRef.getDb(), tRef.getTable());
        if (found.end() != found.find(entry)) return;
        sList.push_back(entry);
        found.insert(entry);
    }
    std::set<StringPair> found;
    StringPairVector& sList;
};

// helper
StringPairVector filterPartitioned(query::TableRefList const& tList) {
    StringPairVector vector;
    getPartitioned gp(vector);
    for (query::TableRefList::const_iterator i = tList.begin(), e = tList.end(); i != e; ++i) {
        (**i).apply(gp);
    }
    return vector;
}

protojson::ScanInfo::Ptr ScanTablePlugin::_findScanTables(query::SelectStmt& stmt,
                                                          query::QueryContext& context) {
    // Might be better as a separate plugin

    // All tables of a query are scan tables if the statement both:
    // a. has non-trivial spatial scope (all chunks? >1 chunk?)
    // b. requires column reading

    // a. means that the there is a spatial scope specification in the
    // WHERE clause or none at all (everything matches). However, an
    // objectId specification counts as a trivial spatial scope,
    // because it evaluates to a specific set of subchunks. We limit
    // the objectId specification, but the limit can be large--each
    // concrete objectId incurs at most the cost of one subchunk.

    // b. means that columns are needed to process the query.
    // In the SelectList, count(*) does not need columns, but *
    // does. So do ra_PS and iFlux_SG*10
    // In the WhereClause, this means that we have expressions that
    // require columns to evaluate.

    // When there is no WHERE clause that requires column reading,
    // the presence of a small-valued LIMIT should be enough to
    // de-classify a query as a scanning query.

    bool hasSelectColumnRef = false;  // Requires row-reading for
                                      // results
    bool hasWhereColumnRef = false;   // Makes count(*) non-trivial
    bool hasSecondaryKey = false;     // Using secondaryKey to restrict
                                      // coverage, e.g., via objectId=123
                                      // or objectId IN (123,133) ?

    if (stmt.hasWhereClause()) {
        query::WhereClause& wc = stmt.getWhereClause();

        // Look for column refs
        std::shared_ptr<query::ColumnRef::Vector const> crl = wc.getColumnRefs();
        if (crl) {
            hasWhereColumnRef = !crl->empty();
#if 0
            // FIXME: Detect secondary key reference by Qserv
            // restrictor detection, not by WHERE clause.
            // The qserv restrictor must be a condition on the
            // secondary key--spatial selects can still be part of
            // scans if they involve >k chunks.
            std::shared_ptr<AndTerm> aterm = wc.getRootAndTerm();
            if (aterm) {
                // Look for secondary key matches
                typedef BoolTerm::PtrList PtrList;
                for(PtrList::iterator i = aterm->iterBegin();
                    i != aterm->iterEnd(); ++i) {
                    if (testIfSecondary(**i)) {
                        hasSecondaryKey = true;
                        break;
                    }
                }
            }
#endif
        }
    }
    query::SelectList& sList = stmt.getSelectList();
    std::shared_ptr<query::ValueExprPtrVector> sVexpr = sList.getValueExprList();

    if (sVexpr) {
        query::ColumnRef::Vector cList;  // For each expr, get column refs.

        typedef query::ValueExprPtrVector::const_iterator Iter;
        for (Iter i = sVexpr->begin(), e = sVexpr->end(); i != e; ++i) {
            (*i)->findColumnRefs(cList);
        }
        // Resolve column refs, see if they include partitioned
        // tables.
        typedef query::ColumnRef::Vector::const_iterator ColIter;
        for (ColIter i = cList.begin(), e = cList.end(); i != e; ++i) {
            // FIXME: Need to resolve and see if it's a partitioned table.
            hasSelectColumnRef = true;
        }
    }

    auto czar = czar::Czar::getCzar();
    bool queryDistributionTestVer = (czar == nullptr) ? 0 : czar->getQueryDistributionTestVer();

    StringPairVector scanTables;
    // Even trivial queries need to use full table scans to avoid crippling the czar
    // with heaps of high priority, but very simple queries. Unless there are
    // factors that greatly restrict how many chunks need to be read, it needs to be
    // a table scan.
    // For system testing, it is useful to see how the system handles large numbers
    // trivial queries as the amount of work done by the workers is minimal. This
    // highlights the cost of czar-worker communications.
    if (!queryDistributionTestVer || hasSelectColumnRef) {
        if (hasSecondaryKey) {
            LOGS(_log, LOG_LVL_TRACE, "**** Not a scan ****");
            // Not a scan? Leave scanTables alone
        } else {
            LOGS(_log, LOG_LVL_TRACE, "**** SCAN (column ref, non-spatial-idx)****");
            // Scan tables = all partitioned tables
            scanTables = filterPartitioned(stmt.getFromList().getTableRefList());
        }
    } else if (hasWhereColumnRef) {
        // No column ref in SELECT, still a scan for non-trivial WHERE
        // count(*): still a scan with a non-trivial where.
        LOGS(_log, LOG_LVL_TRACE, "**** SCAN (filter) ****");
        scanTables = filterPartitioned(stmt.getFromList().getTableRefList());
    }

    // Ask css if any of the tables should be locked in memory and their scan rating.
    // Use this information to determine scanPriority.
    auto scanInfo = protojson::ScanInfo::create();
    for (auto& pair : scanTables) {
        protojson::ScanTableInfo info(pair.first, pair.second);
        css::ScanTableParams const params = context.css->getScanTableParams(info.db, info.table);
        info.lockInMemory = params.lockInMem;
        info.scanRating = params.scanRating;
        scanInfo->infoTables.push_back(info);
        scanInfo->scanRating = std::max(scanInfo->scanRating, info.scanRating);
        scanInfo->scanRating = std::min(scanInfo->scanRating, static_cast<int>(protojson::ScanInfo::SLOWEST));
        LOGS(_log, LOG_LVL_INFO,
             "ScanInfo " << info.db << "." << info.table << " lockInMemory=" << info.lockInMemory
                         << " rating=" << info.scanRating);
    }

    return scanInfo;
}

}  // namespace lsst::qserv::qana
