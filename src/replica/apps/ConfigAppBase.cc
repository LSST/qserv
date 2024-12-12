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
#include "replica/apps/ConfigAppBase.h"

// System headers
#include <iostream>
#include <vector>

// Qserv headers
#include "replica/util/Common.h"
#include "util/TablePrinter.h"

using namespace std;
using namespace lsst::qserv::replica;
namespace util = lsst::qserv::util;

namespace {

bool const injectDatabaseOptions = true;
bool const boostProtobufVersionCheck = false;
bool const enableServiceProvider = false;

}  // namespace

namespace lsst::qserv::replica {

ConfigAppBase::ConfigAppBase(int argc, char* argv[], string const& description)
        : Application(argc, argv, description, injectDatabaseOptions, boostProtobufVersionCheck,
                      enableServiceProvider),
          _configUrl("mysql://qsreplica@localhost:3306/qservReplica") {
    parser().option("config", "Configuration URL (a database connection string).", _configUrl)
            .flag("tables-vertical-separator",
                  "Print vertical separator when displaying tabular data in dumps.", _verticalSeparator);
}

int ConfigAppBase::runImpl() {
    _config = Configuration::load(_configUrl);
    return runSubclassImpl();
}

void ConfigAppBase::dumpWorkersAsTable(string const& indent, string const& caption) const {
    // Extract attributes of each worker and put them into the corresponding
    // columns. Translate tables cell values into strings when required.

    vector<string> name;
    vector<string> isEnabled;
    vector<string> isReadOnly;
    vector<string> dataDir;
    vector<string> svcHostPort;
    vector<string> httpSvcHostPort;
    vector<string> fsHostPort;
    vector<string> loaderHostPort;
    vector<string> loaderTmpDir;
    vector<string> exporterHostPort;
    vector<string> exporterTmpDir;
    vector<string> httpLoaderHostPort;
    vector<string> httpLoaderTmpDir;

    for (auto&& workerName : _config->allWorkers()) {
        auto const worker = _config->worker(workerName);
        name.push_back(worker.name);
        isEnabled.push_back(worker.isEnabled ? "yes" : "no");
        isReadOnly.push_back(worker.isReadOnly ? "yes" : "no");
        dataDir.push_back(worker.dataDir);
        svcHostPort.push_back(worker.svcHost.addr + ":" + to_string(worker.svcPort));
        httpSvcHostPort.push_back(worker.httpSvcHost.addr + ":" + to_string(worker.httpSvcPort));
        fsHostPort.push_back(worker.fsHost.addr + ":" + to_string(worker.fsPort));
        loaderHostPort.push_back(worker.loaderHost.addr + ":" + to_string(worker.loaderPort));
        loaderTmpDir.push_back(worker.loaderTmpDir);
        exporterHostPort.push_back(worker.exporterHost.addr + ":" + to_string(worker.exporterPort));
        exporterTmpDir.push_back(worker.exporterTmpDir);
        httpLoaderHostPort.push_back(worker.httpLoaderHost.addr + ":" + to_string(worker.httpLoaderPort));
        httpLoaderTmpDir.push_back(worker.httpLoaderTmpDir);
    }

    util::ColumnTablePrinter table(caption, indent, verticalSeparator());

    table.addColumn("name", name, util::ColumnTablePrinter::LEFT);
    table.addColumn("enabled", isEnabled);
    table.addColumn("read-only", isReadOnly);
    table.addColumn("Qserv data directory", dataDir, util::ColumnTablePrinter::LEFT);
    table.addColumn("Repl. svc", svcHostPort, util::ColumnTablePrinter::LEFT);
    table.addColumn("Repl. http-svc", httpSvcHostPort, util::ColumnTablePrinter::LEFT);
    table.addColumn("File svc", fsHostPort, util::ColumnTablePrinter::LEFT);
    table.addColumn("Binary ingest", loaderHostPort, util::ColumnTablePrinter::LEFT);
    table.addColumn(":tmp", loaderTmpDir, util::ColumnTablePrinter::LEFT);
    table.addColumn("Export svc", exporterHostPort, util::ColumnTablePrinter::LEFT);
    table.addColumn(":tmp", exporterTmpDir, util::ColumnTablePrinter::LEFT);
    table.addColumn("HTTP ingest", httpLoaderHostPort, util::ColumnTablePrinter::LEFT);
    table.addColumn(":tmp", httpLoaderTmpDir, util::ColumnTablePrinter::LEFT);

    table.print(cout, false, false);
    cout << endl;
}

void ConfigAppBase::dumpFamiliesAsTable(string const& indent, string const& caption) const {
    // Extract attributes of each family and put them into the corresponding
    // columns.

    vector<string> name;
    vector<size_t> replicationLevel;
    vector<unsigned int> numStripes;
    vector<unsigned int> numSubStripes;

    for (auto&& family : _config->databaseFamilies()) {
        auto const fi = _config->databaseFamilyInfo(family);
        name.push_back(fi.name);
        replicationLevel.push_back(fi.replicationLevel);
        numStripes.push_back(fi.numStripes);
        numSubStripes.push_back(fi.numSubStripes);
    }

    util::ColumnTablePrinter table(caption, indent, verticalSeparator());

    table.addColumn("name", name, util::ColumnTablePrinter::LEFT);
    table.addColumn("replication level", replicationLevel);
    table.addColumn("stripes", numStripes);
    table.addColumn("sub-stripes", numSubStripes);

    table.print(cout, false, false);
    cout << endl;
}

void ConfigAppBase::dumpDatabasesAsTable(string const& indent, string const& caption) const {
    // Extract attributes of each database and put them into the corresponding
    // columns.

    vector<string> familyName;
    vector<string> databaseName;
    vector<string> isPublished;
    vector<string> createTime;
    vector<string> publishTime;
    vector<string> tableName;
    vector<string> isPartitioned;
    vector<string> isDirector;
    vector<string> isRefMatch;
    vector<string> directorTable;
    vector<string> directorKey;
    vector<string> directorTable2;
    vector<string> directorKey2;
    vector<string> flagColName;
    vector<string> angSep;
    vector<string> latitudeColName;
    vector<string> longitudeColName;
    vector<string> tableIsPublished;
    vector<string> tableCreateTime;
    vector<string> tablePublishTime;
    vector<string> numColumns;

    string const noSpecificFamily;
    bool const allDatabases = true;
    for (auto&& databaseName_ : _config->databases(noSpecificFamily, allDatabases)) {
        auto&& database = _config->databaseInfo(databaseName_);
        for (auto& tableName_ : database.tables()) {
            auto&& table = database.findTable(tableName_);
            familyName.push_back(database.family);
            databaseName.push_back(database.name);
            isPublished.push_back(database.isPublished ? "yes" : "no");
            createTime.push_back(to_string(database.createTime));
            publishTime.push_back(to_string(database.publishTime));
            tableName.push_back(table.name);
            isPartitioned.push_back(table.isPartitioned ? "yes" : "no");
            isDirector.push_back(table.isDirector() ? "yes" : "no");
            isRefMatch.push_back(table.isRefMatch() ? "yes" : "no");
            directorTable.push_back(table.directorTable.databaseTableName());
            directorKey.push_back(table.directorTable.primaryKeyColumn());
            directorTable2.push_back(table.directorTable2.databaseTableName());
            directorKey2.push_back(table.directorTable2.primaryKeyColumn());
            flagColName.push_back(table.flagColName);
            angSep.push_back(to_string(table.angSep));
            latitudeColName.push_back(table.latitudeColName);
            longitudeColName.push_back(table.longitudeColName);
            tableIsPublished.push_back(table.isPublished ? "yes" : "no");
            tableCreateTime.push_back(to_string(table.createTime));
            tablePublishTime.push_back(to_string(table.publishTime));
            numColumns.push_back(to_string(table.columns.size()));
        }
    }

    util::ColumnTablePrinter table(caption, indent, verticalSeparator());

    table.addColumn("family", familyName, util::ColumnTablePrinter::LEFT);
    table.addColumn("database", databaseName, util::ColumnTablePrinter::LEFT);
    table.addColumn(":published", isPublished);
    table.addColumn(":create-time", createTime);
    table.addColumn(":publish-time", publishTime);
    table.addColumn("table", tableName, util::ColumnTablePrinter::LEFT);
    table.addColumn(":partitioned", isPartitioned);
    table.addColumn(":director", isDirector);
    table.addColumn(":ref-match", isRefMatch);
    table.addColumn(":director-table", directorTable);
    table.addColumn(":director-key", directorKey);
    table.addColumn(":director-table2", directorTable);
    table.addColumn(":director-key2", directorKey);
    table.addColumn(":flag-key", flagColName);
    table.addColumn(":ang-sep", angSep);
    table.addColumn(":latitude-key", latitudeColName);
    table.addColumn(":longitude-key", longitudeColName);
    table.addColumn(":published", tableIsPublished);
    table.addColumn(":create-time", tableCreateTime);
    table.addColumn(":publish-time", tablePublishTime);
    table.addColumn(":num-columns", numColumns);

    table.print(cout, false, false);
    cout << endl;
}

}  // namespace lsst::qserv::replica
