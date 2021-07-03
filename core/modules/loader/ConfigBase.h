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
#ifndef LSST_QSERV_LOADER_CONFIGBASE_H
#define LSST_QSERV_LOADER_CONFIGBASE_H

// system headers
#include <map>
#include <memory>
#include <vector>

// Qserv headers
#include "util/ConfigStore.h"
#include "util/Issue.h"

namespace lsst {
namespace qserv {
namespace loader {

class ConfigErr : public util::Issue {
public:
    ConfigErr(util::Issue::Context const& ctx, std::string const& message) :
        util::Issue(ctx, message) {}
};


/// A class to store the information about a particular configuration
/// file element and allow it to be put on a list.
class ConfigElement {
public:
    typedef std::shared_ptr<ConfigElement> Ptr;
    typedef std::vector<Ptr> CfgElementList;
    enum Kind { STRING, INT, FLOAT }; // Possibly expand to time types.

    ConfigElement() = delete;
    virtual ~ConfigElement() = default;

    /// A factory to create the ConfigElement and add it to 'list'.
    static Ptr create(CfgElementList &list,
                      std::string const& header, std::string const& key,
                      Kind kind, bool required,
                      std::string const& default_="") {
        Ptr ptr(new ConfigElement(header, key, kind, required, default_));
        list.push_back(ptr);
        return ptr;
    }

    static std::string kindToStr(Kind kind);

    /// @return the full key including _header and _key.
    std::string getFullKey() const;

    std::string getValue() const { return _value; }

    /// @return an integer value. Throws ConfigErr if _kind is not INT
    int getInt() const;

    /// @return a double value. Throws ConfigErr if _kind is not FLOAT
    double getDouble() const;

    /// Set the _value for this element from 'cfgStore' using getFullKey() as the key.
    /// This function can throw util::ConfigStoreError.
    void setFromConfig(util::ConfigStore const& cfgStore);

    bool verifyValueIsOfKind();

    /// @return true if the string parses as an integer.
    bool isInteger() const;

    /// @return true if the string parses as a floating type.
    bool isFloat() const;

    /// This is only meant for testing.
    void setValue(std::string const& val) { _value = val; }

    /// Functions to dump this objects information to a log file. Child classes
    /// should only need to provide their own version for dump(std::ostream&).
    virtual std::ostream& dump(std::ostream &os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream &out, ConfigElement const& elem);

private:
    ConfigElement(std::string const& header, std::string const& key,
                  Kind kind, bool required, std::string const& default_)
            : _header(header), _key(key), _kind(kind), _required(required), _default(default_) {}

    std::string _header;       ///< name of the header for this element
    std::string _key;          ///< name of the key under the header
    Kind        _kind{STRING}; ///< Kind (type) of value expected
    std::string _value;        ///< value found in config or default
    bool _required{true};      ///< required to be in config.
    std::string _default;      ///< default value.
};


/// A base class for configuration loading. Child classes define elements
/// expected in the configuration files and provide access functions.
///
/// The constructor can throw. In most cases this is reasonable as exiting with an
/// error is safer than running with a bad configuration file. In other cases, care
/// needs to be taken.
class ConfigBase {
public:
    ConfigBase(ConfigBase const&) = delete;
    ConfigBase& operator=(ConfigBase const&) = delete;

    virtual ~ConfigBase() = default;

    virtual std::ostream& dump(std::ostream &os) const;
    std::string dump() const;
    friend std::ostream& operator<<(std::ostream &out, ConfigBase const& config);

protected:
    ConfigBase() = default;

    /// Set the values for all the elements in cfgList. This can
    /// only be meaningfully called after the child class has filled in cfgList.
    void setFromConfig(util::ConfigStore const& configStore);

    /// A list of ConfigElements that can be found in the configuration.
    ConfigElement::CfgElementList cfgList;
};

}}} // namespace lsst::qserv::loader

#endif // LSST_QSERV_LOADER_CONFIGBASE_H

