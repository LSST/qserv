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
#ifndef LSST_QSERV_LOADER_DOLIST_H
#define LSST_QSERV_LOADER_DOLIST_H


// Qserv headers
#include "loader/DoListItem.h"

namespace lsst {
namespace qserv {
namespace loader {

/// A list of things that need to be done with timers.
/// Everything on the list is checked, if it's timer has expired, it
/// is queued and the timer reset.
/// If it is a single use item, it is deleted after successful completion.
class DoList {
public:
    typedef std::shared_ptr<DoList> Ptr;

    DoList(Central& central) : _central(central) {}
    DoList() = delete;
    DoList(DoList const&) = delete;
    DoList& operator=(DoList const&) = delete;

    ~DoList() = default;

    void checkList();
    bool addItem(DoListItem::Ptr const& item) {
        if (item == nullptr) return false;
        if (item->isAlreadyOnList()) return false; // fast atomic test
        {
            std::lock_guard<std::mutex> lock(_addListMtx);
            // Need to make sure this wasn't added before the mutex got locked.
            if (not item->setAddedToList(true)) {
                _addList.push_back(item);
                return true;
            }
        }
        return false;
    }

    void runItemNow(DoListItem::Ptr const& item);

private:
    std::list<DoListItem::Ptr> _list;
    std::mutex _listMtx; ///< Protects _list (lock this one first)

    std::list<DoListItem::Ptr> _addList;
    std::mutex _addListMtx; ///< Protects _addList (lock this one second)

    Central& _central;
};


}}} // namespace lsst:qserv:loader


#endif // LSST_QSERV_LOADER_DOLIST_H
