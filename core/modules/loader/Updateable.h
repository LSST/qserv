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
#ifndef LSST_QSERV_LOADER_UPDATEABLE_H
#define LSST_QSERV_LOADER_UPDATEABLE_H

// system headers
#include <list>
#include <memory>

// Qserv headers



namespace lsst {
namespace qserv {
namespace loader {

/// The purpose of the classes here is to help detect when a desired state has been achieved.
/// These objects do not know their desired state. The entities that do know the desired state
/// are found on the DoList objects. An alternative approach would be to have these objects
/// store the target state, and send messages until they actual value match the target value.


/// A class to allow an object to be notified when the value of an Updatable
/// object is updated.
template <class T>
class UpdateNotify {
public:
    UpdateNotify() = default;
    virtual ~UpdateNotify() = default;

    virtual void updateNotify(T& oldVal, T& newVal) = 0;
};


/// A class that keeps a list of objects interested in the value of an object,
/// and contacts them when its value is updated. (Updated means the value was set, maybe not changed.)
template <class T>
class Updatable {
public:
    Updatable() = default;
    explicit Updatable(T const& val) : _value(val) {}
    Updatable(Updatable const&) = delete;
    Updatable& operator=(Updatable const&) = delete;

    void update(T const& val) {
        T oldVal = _value;
        _value = val;
        _notifyAll(oldVal, _value);
    }

    T get() { return _value; }

    void registerNotify(std::shared_ptr<UpdateNotify<T>> const& un) {
        _notifyList.push_back(un);
    }
private:
    void _notifyAll(T& oldVal, T& newVal) {
        auto iter = _notifyList.begin();
        while (iter != _notifyList.end()) {
            auto unPtr = (*iter).lock();
            if (unPtr == nullptr) {
                iter = _notifyList.erase(iter);
            } else {
                unPtr->updateNotify(oldVal, newVal);
                ++iter;
            }
        }
    }

    std::list<std::weak_ptr<UpdateNotify<T>>> _notifyList;

    T _value;
};


}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_UPDATEABLE_H
