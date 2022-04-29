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

// Class header
#include "memman/MemFileSet.h"

// System Headers
#include <errno.h>

// Qserv Headers
#include "memman/MemFile.h"
#include "memman/Memory.h"

namespace lsst { namespace qserv { namespace memman {

/******************************************************************************/
/*                            D e s t r u c t o r                             */
/******************************************************************************/

MemFileSet::~MemFileSet() {
    // Unreference every fle in our file set. This action will also cause
    // memory to be unlocked if no one else is using the file then the file
    // object will be deleted as well.
    //
    for (auto mfP : _lockFiles) {
        mfP->release();
    }
    for (auto mfP : _flexFiles) {
        mfP->release();
    }

    // Unlock this file set if it is locked
    //
    serialize(false);
}

/******************************************************************************/
/*                                   a d d                                    */
/******************************************************************************/

int MemFileSet::add(std::string const& tabname, int chunk, bool iFile, bool mustLK) {
    std::string fPath(_memory.filePath(tabname, chunk, iFile));

    // Obtain a memory file object for this table and chunk
    //
    MemFile::MFResult mfResult = MemFile::obtain(fPath, _memory, !mustLK);
    if (mfResult.mfP == 0) return mfResult.retc;

    // Add to the appropriate file set
    //
    if (mustLK) {
        _lockFiles.push_back(mfResult.mfP);
    } else {
        _flexFiles.push_back(mfResult.mfP);
    }
    _numFiles++;
    return 0;
}

/******************************************************************************/
/*                               l o c k A l l                                */
/******************************************************************************/

int MemFileSet::lockAll(bool strict) {
    MemFile::MLResult mlResult;
    uint64_t totLocked = 0;
    double totMlockSeconds = 0.0;

    // Try to lock all of the required tables. Any failure is considered fatal.
    // The caller should delete the fileset upon return in this case.
    //
    for (auto mfP : _lockFiles) {
        mlResult = mfP->memLock();
        totLocked += mlResult.bLocked;
        totMlockSeconds += mlResult.mlockTime;
        if (mlResult.retc != 0 && strict) {
            _lockBytes += totLocked;
            _lockSeconds += totMlockSeconds;
            return mlResult.retc;
        }
    }

    // Try locking as many flexible files as we can. At some point we will
    // place unlocked flex files on a "want to lock" queue. FUTURE!!! In any
    // case we ignore all errors here as these files may remain unlocked.
    //
    for (auto mfP : _flexFiles) {
        mlResult = mfP->memLock();
        totLocked += mlResult.bLocked;
        totMlockSeconds += mlResult.mlockTime;
    }

    // We ignore optional files at this point. FUTURE!!!
    //

    // All done, update the statistics.
    //
    _lockBytes += totLocked;
    _lockSeconds += totMlockSeconds;
    return 0;
}

/******************************************************************************/
/*                                m a p A l l                                 */
/******************************************************************************/

int MemFileSet::mapAll() {
    int rc;

    // Try to map all of the required tables. Any failure is considered fatal.
    // The caller should delete the fileset upon return in this case.
    //
    for (auto mfP : _lockFiles) {
        rc = mfP->memMap();
        if (rc != 0) return rc;
    }

    // Try locking as many flexible files as we can. At some point we will
    // place unlocked flex files on a "want to lock" queue. FUTURE!!! In any
    // case we ignore all errors here as these files may remain unlocked.
    //
    for (auto mfP : _flexFiles) {
        if (mfP->memMap() != 0) break;
    }

    // We ignore optional files at this point. FUTURE!!!
    //

    // All done
    //
    return 0;
}

/******************************************************************************/
/*                                s t a t u s                                 */
/******************************************************************************/

MemMan::Status MemFileSet::status() {
    MemMan::Status myStatus(_lockBytes, _lockSeconds, _numFiles, _chunk);

    return myStatus;
}
}}}  // namespace lsst::qserv::memman
