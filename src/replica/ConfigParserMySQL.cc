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

// Class header
#include "replica/ConfigParserMySQL.h"

// System headers
#include <list>
#include <stdexcept>

// Qserv headers
#include "replica/ConfigurationExceptions.h"

using namespace std;
using json = nlohmann::json;

namespace lsst {
namespace qserv {
namespace replica {

int const ConfigParserMySQL::expectedSchemaVersion = 6;


ConfigParserMySQL::ConfigParserMySQL(database::mysql::Connection::Ptr const& conn,
                                     json& data,
                                     map<string, WorkerInfo>& workers,
                                     map<string, DatabaseFamilyInfo>& databaseFamilies,
                                     map<string, DatabaseInfo>& databases)
    :   _conn(conn),
        _data(data),
        _workers(workers),
        _databaseFamilies(databaseFamilies),
        _databases(databases) {
}


void ConfigParserMySQL::parse() {

    _parseVersion();

    // Read and update the transient state the general parameters and defaults
    // shared by all components of the Replication system. The table also provides
    // default values for some critical parameters of the worker-side services.
    _parseGeneral();

    // Parse groupped parameters
    _parseWorkers();
    _parseDatabaseFamilies();
    _parseDatabases();
}


void ConfigParserMySQL::_parseVersion() {
    string const table = "QMetadata";
    if (!_conn->tableExists(table)) {
        throw ConfigError(
                _context + " the metadata table '" + table + "' doesn't exist in the database '"
                + _conn->connectionParams().database + "'.");
    }
    string const colname = "value";
    string version;
    bool const isNotNull = _conn->executeSingleValueSelect(
        "SELECT " + _conn->sqlId(colname) + " FROM " + _conn->sqlId(table)
        + " WHERE " + _conn->sqlEqual("metakey", "version"),
        colname,
        version);
    if (!isNotNull) {
        throw ConfigError(_context + " no schema version info found in the metadata table '" + table + "'.");
    }
    string const expectedSchemaVersionStr = to_string(expectedSchemaVersion);
    if (version != expectedSchemaVersionStr) {
        throw ConfigError(
                _context + " schema version '" + version + "' found in the metadata table '" + table
                + "' doesn't match the expected version '" + expectedSchemaVersionStr
                + "'. Please, run the schema upgrade application.");
    }
}


void ConfigParserMySQL::_parseGeneral() {
    _conn->execute("SELECT * FROM " + _conn->sqlId("config"));
    while (_conn->next(_row)) {
        string category;
        _row.get("category", category);

        string param;
        _row.get("param", param);

        json obj;
        try {
            obj = _data.at(category).at(param);
        } catch (exception const& ex) {
            throw invalid_argument(
                    _context + " no transient schema match for the parameter, category: '"
                    + category + "' param: '" + param + "'.");
        }
        if (obj.is_string()) {
            _storeGeneralParameter<string>(category, param);
        } else if (obj.is_number_unsigned()) {
            _storeGeneralParameter<uint64_t>(category, param);
        } else if (obj.is_number_integer()) {
            _storeGeneralParameter<int64_t>(category, param);
        } else if (obj.is_number_float()) {
            _storeGeneralParameter<double>(category, param);
        } else {
            throw invalid_argument(
                    _context + " unsupported transient schema type for the parameter, category: '"
                    + category + "' param: '" + param + "'.");
        }
    }
}


void ConfigParserMySQL::_parseWorkers() {
    json& defaults = _data.at("worker_defaults");
    _conn->execute("SELECT * FROM " + _conn->sqlId("config_worker"));
    while (_conn->next(_row)) {
        WorkerInfo info;
        info.name = _parseParam<string>("name");
        info.isEnabled = _parseParam<int>("is_enabled") != 0;
        info.isReadOnly = _parseParam<int>("is_read_only") != 0;
        info.svcHost = _parseParam<string>("svc_host");
        info.svcPort = _parseParam<uint16_t>("svc_port", defaults);
        info.fsHost = _parseParam<string>("fs_host", info.svcHost);
        info.fsPort = _parseParam<uint16_t>("fs_port", defaults);
        info.dataDir = _parseParam<string>("data_dir", defaults);
        info.loaderHost = _parseParam<string>("loader_host", info.svcHost);
        info.loaderPort = _parseParam<uint16_t>("loader_port", defaults);
        info.loaderTmpDir = _parseParam<string>("loader_tmp_dir", defaults);
        info.exporterHost = _parseParam<string>("exporter_host", info.svcHost);
        info.exporterPort  = _parseParam<uint16_t>("exporter_port", defaults);
        info.exporterTmpDir = _parseParam<string>("exporter_tmp_dir", defaults);
        info.httpLoaderHost = _parseParam<string>("http_loader_host", info.svcHost);
        info.httpLoaderPort  = _parseParam<uint16_t>("http_loader_port", defaults);
        info.httpLoaderTmpDir = _parseParam<string>("http_loader_tmp_dir", defaults);
        _workers[info.name] = info;
    }
}


void ConfigParserMySQL::_parseDatabaseFamilies() {
    _conn->execute("SELECT * FROM " + _conn->sqlId("config_database_family"));
    while (_conn->next(_row)) {
        DatabaseFamilyInfo info;
        info.name = _parseParam<string>("name");
        info.replicationLevel = _parseParam<unsigned int>("min_replication_level");
        info.numStripes = _parseParam<unsigned int>("num_stripes");
        info.numSubStripes = _parseParam<unsigned int>("num_sub_stripes");
        info.overlap = _parseParam<double>("overlap");
        _databaseFamilies[info.name] = info;
    }
}


void ConfigParserMySQL::_parseDatabases() {
    _conn->execute("SELECT * FROM " + _conn->sqlId("config_database"));
    while (_conn->next(_row)) {
        DatabaseInfo info;
        info.name = _parseParam<string>("database");
        info.family = _parseParam<string>("family_name");
        info.isPublished = _parseParam<int>("is_published") != 0;
        _databases[info.name] = info;
    }
    // Read database-specific table definitions and extend the corresponding database entries.
    _conn->execute("SELECT * FROM " + _conn->sqlId("config_database_table"));
    while (_conn->next(_row)) {
        string const database = _parseParam<string>("database");
        string const table = _parseParam<string>("table");
        DatabaseInfo& info = _databases[database];
        if (_parseParam<int>("is_partitioned") != 0) {
            info.directorTable[table] = _parseParam<string>("director_table");
            info.directorTableKey[table] = _parseParam<string>("director_key");
            if (info.directorTableKey[table].empty()) {
                throw ConfigError(
                    _context + " the key 'director_key' of the partitioned table: '"
                    + table + "' is empty.");
            }
            info.latitudeColName[table] = _parseParam<string>("latitude_key");
            info.longitudeColName[table] = _parseParam<string>("longitude_key");
            if (info.directorTable[table].empty()) {
                if (info.latitudeColName[table].empty() || info.longitudeColName[table].empty()) {
                    throw ConfigError(
                        _context + " one of the spatial coordinate keys 'latitude_key' or 'longitude_key'"
                        " of the director table: '" + table + "' is empty.");
                }
            } else {
                if (info.latitudeColName[table].empty() != info.longitudeColName[table].empty()) {
                    throw ConfigError(
                        _context + " inconsistent definition of the spatial coordinate keys 'latitude_key'"
                        " and 'longitude_key' of the dependent table: '" + table + "'. Both keys need to be"
                        " either empty or be defined.");
                }
            }
            info.partitionedTables.push_back(table);
        } else {
            info.regularTables.push_back(table);
        }
    }

    // Validate referential integrity between the "director" and "dependent" tables
    // to ensure each dependent table has a valid "director".
    for (auto&& databaseItr: _databases) {
        string const& database = databaseItr.first;
        DatabaseInfo& info = databaseItr.second;
        for (auto&& itr: info.directorTable) {
            string const& table = itr.first;
            string const& director = itr.second;
            if (!director.empty()) {
                if (!info.directorTable.count(director)) {
                    throw ConfigError(
                        _context + " the director table '" + director + "' of database '" + database
                        + "' required by the dependent table '" + table
                        + "' is not found in the configuration.");
                } else {
                    if (!info.directorTable.at(director).empty()) {
                        throw ConfigError(
                            _context + " the table: '" + table + "' of database '" + database
                            + "' is defined as depending on another dependent table '" + director
                            + "' instead of a valid director table.");
                    }
                }
            }
        }
    }

    // Read schema for each table (if available)
    for (auto&& databaseItr: _databases) {
        string const& database = databaseItr.first;
        DatabaseInfo& info = databaseItr.second;
        for (auto&& table: info.tables()) {
            list<SqlColDef>& columns = info.columns[table];
            _conn->execute(
                "SELECT "     + _conn->sqlId("col_name") + "," + _conn->sqlId("col_type") +
                "  FROM "     + _conn->sqlId("config_database_table_schema") +
                "  WHERE "    + _conn->sqlEqual("database", database) +
                "    AND "    + _conn->sqlEqual("table", table) +
                "  ORDER BY " + _conn->sqlId("col_position") + " ASC");
            while (_conn->next(_row)) {
                columns.push_back(SqlColDef(
                        _parseParam<string>("col_name"),
                        _parseParam<string>("col_type")
                ));
            }
        }
    }
}

}}} // namespace lsst::qserv::replica
