// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2008-2015 AURA/LSST.
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
#include "cconfig/CzarConfig.h"

// System headers
#include <stdexcept>

// Third party headers
#include "XrdSsi/XrdSsiLogger.hh"

// LSST headers
#include "lsst/log/Log.h"

// Qserv headers
#include "mysql/MySqlConfig.h"
#include "util/ConfigStore.h"
#include "util/IterableFormatter.h"

namespace {

LOG_LOGGER _log = LOG_GET("lsst.qserv.cconfig.CzarConfig");

void QservLogger(struct timeval const& mtime, unsigned long tID, const char* msg, int mlen) {
    static log4cxx::spi::LocationInfo xrdLoc(
            "client", log4cxx::spi::LocationInfo::calcShortFileName("client"), "<xrdssi>", 0);
    static LOG_LOGGER myLog = LOG_GET("lsst.qserv.xrdssi.msgs");

    if (myLog.isInfoEnabled()) {
        while (mlen && msg[mlen - 1] == '\n') --mlen;  // strip all trailing newlines
        std::string theMsg(msg, mlen);
        lsst::log::Log::MDC("LWP", std::to_string(tID));
        myLog.logMsg(log4cxx::Level::getInfo(), xrdLoc, theMsg);
    }
}

bool dummy = XrdSsiLogger::SetMCB(QservLogger, XrdSsiLogger::mcbClient);
}  // namespace

namespace lsst::qserv::cconfig {

std::mutex CzarConfig::_mtxOnInstance;

CzarConfig::Ptr CzarConfig::_instance;

CzarConfig::Ptr CzarConfig::create(std::string const& configFileName, std::string const& czarName) {
    std::lock_guard<std::mutex> const lock(_mtxOnInstance);
    if (_instance == nullptr) {
        _instance = std::shared_ptr<CzarConfig>(new CzarConfig(util::ConfigStore(configFileName), czarName));
    }
    return _instance;
}

CzarConfig::Ptr CzarConfig::instance() {
    std::lock_guard<std::mutex> const lock(_mtxOnInstance);
    if (_instance == nullptr) {
        throw std::logic_error("CzarConfig::" + std::string(__func__) + ": instance has not been created.");
    }
    return _instance;
}

CzarConfig::CzarConfig(util::ConfigStore const& configStore, std::string const& czarName)
        : _czarName(czarName) {
    _configValMap.readConfigStore(configStore);

    if (_cssPort->getVal() == 0 && _cssSocket->getVal().empty()) {
        throw util::ConfigException(ERR_LOC, " CzarConfig::" + std::string(__func__) + " Neither _cssPort " +
                                                     _cssPort->getSectionDotName() + "=" +
                                                     _cssPort->getValStr() + " nor _cssSocket " +
                                                     _cssSocket->getSectionDotName() + "=" +
                                                     _cssSocket->getValStr() + " have valid values.");
    }

    if (_replicationRegistryHost->getVal().empty()) {
        throw util::ConfigException(ERR_LOC, " CzarConfig::" + std::string(__func__) +
                                                     ": 'replication.registry_host' is not set.");
    }
    if (_replicationRegistryPort->getVal() == 0) {
        throw util::ConfigException(ERR_LOC, " CzarConfig::" + std::string(__func__) +
                                                     ": 'replication.registry_port' number can't be 0.");
    }
    if (_replicationRegistryHearbeatIvalSec->getVal() == 0) {
        throw util::ConfigException(ERR_LOC,
                                    " CzarConfig::" + std::string(__func__) +
                                            ": 'replication.registry_heartbeat_ival_sec' can't be 0.");
    }
    if (_replicationNumHttpThreads->getVal() == 0) {
        throw util::ConfigException(ERR_LOC, " CzarConfig::" + std::string(__func__) +
                                                     ": 'replication.num_http_threads' can't be 0.");
    }

    // Cache the cached version of the configuration in the JSON format. The JSON object
    // contains two collections of parameters: the "input" ones that were passed into
    // the contructor, and the "actual" ones that were expected by the current implementation
    // of Czar.
    _jsonConfig = nlohmann::json::object({{"input", configStore.toJson()},
                                          {"actual", nlohmann::json::object()},
                                          {"default", nlohmann::json::object()}});

    // Note that actual collection may contain parameters not mentioned in
    // the input configuration.
    nlohmann::json& actualJsonConfig = _jsonConfig["actual"];

    _configValMap.populateJson(actualJsonConfig);
    bool const useDefault = true;
    _configValMap.populateJson(_jsonConfig["default"], useDefault);

    actualJsonConfig["identity"] =
            nlohmann::json::object({{"name", _czarName}, {"id", std::to_string(_czarId)}});
}

void CzarConfig::setReplicationHttpPort(uint16_t port) {
    if (port == 0) {
        throw std::invalid_argument("CzarConfig::" + std::string(__func__) + ": port number can't be 0.");
    }
    _replicationHttpPort->setVal(port);
    // Update the relevant section of the JSON-ified configuration.
    _jsonConfig["actual"][_replicationHttpPort->getSection()][_replicationHttpPort->getName()] =
            _replicationHttpPort->getValStr();
}

void CzarConfig::setId(qmeta::CzarId id) {
    _czarId = id;
    // Update the relevant section of the JSON-ified configuration.
    _jsonConfig["actual"]["identity"]["id"] = std::to_string(_czarId);
}

mysql::MySqlConfig CzarConfig::getMySqlResultConfig() const {
    return mysql::MySqlConfig(_resultDbUser->getVal(), _resultDbPasswd->getVal(), _resultDbHost->getVal(),
                              _resultDbPort->getVal(), _resultDbUnixSocket->getVal(), _resultDbDb->getVal());
}

mysql::MySqlConfig CzarConfig::getMySqlQmetaConfig() const {
    return mysql::MySqlConfig(_qmetaUser->getVal(), _qmetaPasswd->getVal(), _qmetaHost->getVal(),
                              _qmetaPort->getVal(), _qmetaUnixSocket->getVal(), _qmetaDb->getVal());
}

mysql::MySqlConfig CzarConfig::getMySqlQStatusDataConfig() const {
    return mysql::MySqlConfig(_qstatusUser->getVal(), _qstatusPasswd->getVal(), _qstatusHost->getVal(),
                              _qstatusPort->getVal(), _qstatusUnixSocket->getVal(), _qstatusDb->getVal());
}

std::map<std::string, std::string> CzarConfig::getCssConfigMap() const {
    // TODO: The way CssConfig uses this is not ideal and probably should be changed.
    //       Maybe to return a map of copies of the related util::ConfigVal objects.
    return _configValMap.getSectionMapStr("css");
}

std::ostream& operator<<(std::ostream& out, CzarConfig const& czarConfig) {
    out << czarConfig._jsonConfig.dump();
    return out;
}

}  // namespace lsst::qserv::cconfig
