// -*- LSST-C++ -*-
/*
 * LSST Data Management System
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

#ifndef LSST_QSERV_UTIL_CONFIGVALMAP_H
#define LSST_QSERV_UTIL_CONFIGVALMAP_H

// System headers
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>

// Third party headers
#include <nlohmann/json.hpp>

// Qserv headers
#include "util/ConfigStore.h"
#include "util/Issue.h"

namespace lsst::qserv::util {

/// Class for handling configuration exceptions.
class ConfigException : public util::Issue {
public:
    ConfigException(Context const& ctx, std::string const& msg) : util::Issue(ctx, msg) {}
};

class ConfigValMap;

/// Base class for storing values, usually from configuration files, that have
/// identifiers consisting of a `section` and a `name`.
/// This class is meant to be used with ConfigValMap.
/// TODO:UJ a command line argument can be added to this and if the command
///         line argument is found, it will override the value in the file.
class ConfigVal {
public:
    using Ptr = std::shared_ptr<ConfigVal>;

    ConfigVal() = delete;
    ConfigVal(std::string const& section, std::string const& name, bool required, bool hidden)
            : _section(section), _name(name), _required(required), _hidden(hidden) {}
    virtual ~ConfigVal() = default;

    ConfigVal(ConfigVal const&) = delete;
    ConfigVal& operator=(ConfigVal const&) = delete;

    std::string getSection() const { return _section; }
    std::string getName() const { return _name; }
    std::string getSectionDotName() const { return _section + "." + _name; }

    /// Return true if the value is required to be from a file.
    bool isRequired() const { return _required; }

    /// Return true if this value should not be seen by users or logs.
    bool isHidden() const { return _hidden; }

    bool isValSetFromFile() const { return _valSetFromFile; }

    /// All child classes should be able to return a valid string version of their value,
    /// but this function will hide values of `_hidden` `ConfigVal`.
    /// If the string value of something that is hidden is needed, call getValStrDanger().
    virtual std::string getValStr() const final { return (isHidden()) ? "*****" : getValStrDanger(); }

    /// All child classes should be able to return a valid string version of their default
    /// value, but this function will hide values of `_hidden` `ConfigVal`.
    /// If the string value of something that is hidden is needed, call getValStrDanger().
    virtual std::string getDefValStr() const final { return (isHidden()) ? "*****" : getDefValStrDanger(); }

    /// All child classes should be able to return a valid string version of their value,
    /// and this function will show `_hidden` values, which is dangerous.
    virtual std::string getValStrDanger() const = 0;

    /// All child classes should be able to return a valid string version of their
    /// default value, and this function will show `_hidden` values, which is dangerous.
    virtual std::string getDefValStrDanger() const = 0;

    /// If possible, get the value (`_val` and `_defVal`) for this item from `configStore`.
    /// If the value cannot be set, the default value remains unchanged.
    virtual void setValFromConfigStore(util::ConfigStore const& configStore) final;

    /// Version of `setValFromConfigStore` used by derived classes to get the
    /// correct type when querying `configStore`.
    virtual void setValFromConfigStoreChild(util::ConfigStore const& configStore) = 0;

    /// @throws ConfigException if the value is invalid or already in the map.
    static void addToMapBase(ConfigValMap& configValMap, Ptr const&);

private:
    std::string const _section;    ///< section name that this item belongs to.
    std::string const _name;       ///< name of this item within its section.
    bool const _required;          ///< Set to true if this value must be found in the file.
    bool const _hidden;            ///< Set to true if the value should be hidden from users and logs.
    std::string _description;      ///< description of this configuration value, if available.
    bool _valSetFromFile = false;  ///< set to true if value was set from configuration file.

protected:
    /// Set to true if the value is set from a file. This is used with _required to verify that
    /// all required values were found in a file. See ConfigValMap::checkRequired()
    void setValSetFromFile(bool setFromFile) { _valSetFromFile = setFromFile; }

    /// Used to log when values are set, particularly from the template classes.
    void logValSet(std::string const& msg = std::string());
};

/// A template child of ConfigVal that can actually store the value. Unfortunately
/// reliably reading values from ConfigStore requires derived classes for specific
/// types.
template <typename T>
class ConfigValT : public ConfigVal {
public:
    /// Provide a shared_ptr to a new ConfigValT<T> object and adds it to `configValMap`.
    /// @throws ConfigException if there are problems.
    static std::shared_ptr<ConfigValT<T>> create(ConfigValMap& configValMap, std::string const& section,
                                                 std::string const& name, bool required, T defVal,
                                                 bool hidden = false) {
        auto newPtr =
                std::shared_ptr<ConfigValT<T>>(new ConfigValT<T>(section, name, required, defVal, hidden));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    virtual ~ConfigValT() = default;

    /// @see `ConfigVal::getValStrDanger()`
    std::string getValStrDanger() const override {
        std::stringstream os;
        os << _val;
        return os.str();
    }

    /// @see `ConfigVal::getValStrDanger()`
    std::string getDefValStrDanger() const override {
        std::stringstream os;
        os << _defVal;
        return os.str();
    }

    T const& getVal() const { return _val; }
    T const& getDefVal() const { return _defVal; }
    void setVal(T val) {
        _val = val;
        logValSet();
    }

protected:
    ConfigValT(std::string const& section, std::string const& name, bool required, T defVal, bool hidden)
            : ConfigVal(section, name, required, hidden), _defVal(defVal), _val(_defVal) {}

private:
    T const _defVal;  ///< Default value for the item this class is storing.
    T _val;           ///< Value for the item this class is storing.
};

/// Bool is special case for json as the value should be "true" or "false" but
/// ConfigStore has it as '0' or '1'.
class ConfigValTBool : public ConfigValT<bool> {
public:
    using BoolPtr = std::shared_ptr<ConfigValTBool>;

    ConfigValTBool() = delete;
    virtual ~ConfigValTBool() = default;

    static BoolPtr create(ConfigValMap& configValMap, std::string const& section, std::string const& name,
                          bool required, bool defVal, bool hidden = false) {
        auto newPtr = BoolPtr(new ConfigValTBool(section, name, required, defVal, hidden));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    /// Return "false" if `bVal` is '0', otherwise "true".
    static std::string toString(bool bVal);

    /// @see `ConfigVal::getValStrDanger()`
    std::string getValStrDanger() const override { return toString(getVal()); }

    /// @see `ConfigVal::setValFromConfigStoreChild()`
    void setValFromConfigStoreChild(util::ConfigStore const& configStore) override {
        setVal(configStore.getIntRequired(getSectionDotName()));
    }

private:
    ConfigValTBool(std::string const& section, std::string const& name, bool required, bool defVal,
                   bool hidden)
            : ConfigValT<bool>(section, name, required, defVal, hidden) {}
};

/// This class reads and stores string values.
class ConfigValTStr : public ConfigValT<std::string> {
public:
    using StrPtr = std::shared_ptr<ConfigValTStr>;

    ConfigValTStr() = delete;
    virtual ~ConfigValTStr() = default;

    static StrPtr create(ConfigValMap& configValMap, std::string const& section, std::string const& name,
                         bool required, std::string const& defVal, bool hidden = false) {
        auto newPtr = StrPtr(new ConfigValTStr(section, name, required, defVal, hidden));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    void setValFromConfigStoreChild(util::ConfigStore const& configStore) override {
        setVal(configStore.getRequired(getSectionDotName()));
    }

private:
    ConfigValTStr(std::string const& section, std::string const& name, bool required,
                  std::string const& defVal, bool hidden)
            : ConfigValT<std::string>(section, name, required, defVal, hidden) {}
};

// This class reads and stores integer values.
class ConfigValTInt : public ConfigValT<int64_t> {
public:
    using IntPtr = std::shared_ptr<ConfigValTInt>;

    ConfigValTInt() = delete;
    virtual ~ConfigValTInt() = default;

    static IntPtr create(ConfigValMap& configValMap, std::string const& section, std::string const& name,
                         bool required, int64_t defVal, bool hidden = false) {
        auto newPtr = IntPtr(new ConfigValTInt(section, name, required, defVal, hidden));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    void setValFromConfigStoreChild(util::ConfigStore const& configStore) override {
        setVal(configStore.getIntRequired(getSectionDotName()));
    }

private:
    ConfigValTInt(std::string const& section, std::string const& name, bool required, int64_t defVal,
                  bool hidden)
            : ConfigValT<int64_t>(section, name, required, defVal, hidden) {}
};

/// This class reads and stores unsigned unsigned integer values.
class ConfigValTUInt : public ConfigValT<uint64_t> {
public:
    using UIntPtr = std::shared_ptr<ConfigValTUInt>;

    ConfigValTUInt() = delete;
    virtual ~ConfigValTUInt() = default;

    static UIntPtr create(ConfigValMap& configValMap, std::string const& section, std::string const& name,
                          bool required, uint64_t defVal, bool hidden = false) {
        auto newPtr = UIntPtr(new ConfigValTUInt(section, name, required, defVal, hidden));
        addToMapBase(configValMap, newPtr);
        return newPtr;
    }

    void setValFromConfigStoreChild(util::ConfigStore const& configStore) override {
        int64_t vInt = configStore.getIntRequired(getSectionDotName());
        if (vInt < 0) {
            throw ConfigException(ERR_LOC, "ConfigValUInt " + getSectionDotName() + " was negative " +
                                                   std::to_string(vInt));
        }
        setVal(configStore.getIntRequired(getSectionDotName()));
    }

private:
    ConfigValTUInt(std::string const& section, std::string const& name, bool required, uint64_t defVal,
                   bool hidden)
            : ConfigValT<uint64_t>(section, name, required, defVal, hidden) {}
};

/// Read values from a configuration source, such as util::ConfigStore or yaml,
/// enabling reasonably possible verification, and output as a json object.
/// util::ConfigStore is limited in its capabilities.
class ConfigValMap {
public:
    using NameMap = std::map<std::string, ConfigVal::Ptr>;  ///< key is ConfigVal::_name
    using SectionMap = std::map<std::string, NameMap>;      ///< key is ConfigVal::_section

    /// Insert `newVal` into the map at a location determined by section and name.
    /// @throw ConfigException if the entry already exists.
    void addEntry(ConfigVal::Ptr const& newVal);

    /// Return a pointer to the entry for `section` and `name`, returning
    /// nullptr if the entry cannot be found.
    ConfigVal::Ptr getEntry(std::string const& section, std::string const& name);

    /// Read the configuration values for all items in the map from `configStore`.
    /// All values that are found will have `_valSetFromFile` set to true.
    /// @throws ConfigException if there are problems.
    void readConfigStore(util::ConfigStore const& configStore);

    /// Returns false and an empty string if all `_required` values were set from the file,
    /// otherwise, it returns true and a string listing the problem entries.
    std::tuple<bool, std::string> checkRequired() const;

    /// This function fills the supplied `js` object with entries from all values
    /// in `_sectionMap` given by section and then all names in that section.
    /// @param `js` json object to populate.
    /// @param `useDefault` set to true to populate `js` with default values instead of
    ///        actual values.
    void populateJson(nlohmann::json& js, bool useDefault = false) const;

    /// Return a map of name, value for all entries in `section`.
    std::map<std::string, std::string> getSectionMapStr(std::string const& section) const;

private:
    SectionMap _sectionMap;
};
}  // namespace lsst::qserv::util

#endif  // LSST_QSERV_UTIL_CONFIGVALMAP_H
