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
#include "replica/util/Common.h"

// System headers
#include <stdexcept>

// Third party headers
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

using namespace std;
using json = nlohmann::json;

namespace lsst::qserv::replica {

string status2string(ProtocolStatusExt status) { return ProtocolStatusExt_Name(status); }

string overlapSelector2str(ChunkOverlapSelector selector) {
    switch (selector) {
        case ChunkOverlapSelector::CHUNK:
            return "CHUNK";
        case ChunkOverlapSelector::OVERLAP:
            return "OVERLAP";
        case ChunkOverlapSelector::CHUNK_AND_OVERLAP:
            return "CHUNK_AND_OVERLAP";
    }
    throw invalid_argument(
            "lsst::qserv::replica::" + string(__func__) + " unhandled selector: " +
            to_string(static_cast<typename std::underlying_type<ChunkOverlapSelector>::type>(selector)));
}

ostream& operator<<(ostream& os, ChunkOverlapSelector selector) {
    os << overlapSelector2str(selector);
    return os;
}

ChunkOverlapSelector str2overlapSelector(string const& str) {
    if (str == "CHUNK")
        return ChunkOverlapSelector::CHUNK;
    else if (str == "OVERLAP")
        return ChunkOverlapSelector::OVERLAP;
    else if (str == "CHUNK_AND_OVERLAP")
        return ChunkOverlapSelector::CHUNK_AND_OVERLAP;
    throw invalid_argument("lsst::qserv::replica::" + string(__func__) + " the input string '" + str +
                           "' doesn't match any selector.");
}

////////////////////////////////////////////
//                Generators              //
////////////////////////////////////////////

replica::Mutex Generators::_mtx;

string Generators::uniqueId() {
    replica::Lock lock(_mtx, "Generators::" + string(__func__));
    boost::uuids::uuid id = boost::uuids::random_generator()();
    return boost::uuids::to_string(id);
}

///////////////////////////////////////////
//                SqlColDef              //
///////////////////////////////////////////

list<SqlColDef> parseSqlColumns(json const& columnsJsonArray) {
    if (!columnsJsonArray.is_array()) {
        throw invalid_argument("lsst::qserv::replica::" + string(__func__) +
                               "  columnsJsonArray is not an array");
    }
    list<SqlColDef> columns;
    for (auto const& column : columnsJsonArray) {
        columns.emplace_back(column.at("name"), column.at("type"));
    }
    return columns;
}

///////////////////////////////////////////
//                SqlIndexDef            //
///////////////////////////////////////////

SqlIndexDef::SqlIndexDef(json const& indexSpecJson) {
    if (!indexSpecJson.is_object()) {
        throw invalid_argument("lsst::qserv::replica::" + string(__func__) +
                               "  indexSpecJson is not an object");
    }
    spec = indexSpecJson.value("spec", "DEFAULT");
    name = indexSpecJson.at("name");
    comment = indexSpecJson.value("comment", "");
    auto const keysJsonArray = indexSpecJson.at("keys");
    if (!keysJsonArray.is_array()) {
        throw invalid_argument("lsst::qserv::replica::" + string(__func__) + "  keys is not an array");
    }
    for (auto const& key : keysJsonArray) {
        keys.emplace_back(key.at("name"), key.at("length"), key.at("ascending"));
    }
}

////////////////////////////////////////////
//        Parameters of requests          //
////////////////////////////////////////////

ReplicationRequestParams::ReplicationRequestParams(ProtocolRequestReplicate const& request)
        : database(request.database()),
          chunk(request.chunk()),
          sourceWorker(request.worker()),
          sourceWorkerHost(request.worker_host()),
          sourceWorkerPort(request.worker_port()),
          sourceWorkerDataDir(request.worker_data_dir()) {}

DeleteRequestParams::DeleteRequestParams(ProtocolRequestDelete const& request)
        : database(request.database()), chunk(request.chunk()) {}

FindRequestParams::FindRequestParams(ProtocolRequestFind const& request)
        : database(request.database()), chunk(request.chunk()) {}

FindAllRequestParams::FindAllRequestParams(ProtocolRequestFindAll const& request)
        : database(request.database()) {}

EchoRequestParams::EchoRequestParams(ProtocolRequestEcho const& request)
        : data(request.data()), delay(request.delay()) {}

SqlRequestParams::SqlRequestParams(ProtocolRequestSql const& request) : maxRows(request.max_rows()) {
    auto const requestType = request.type();
    switch (requestType) {
        case ProtocolRequestSql::QUERY:
            type = QUERY;
            break;
        case ProtocolRequestSql::CREATE_DATABASE:
            type = CREATE_DATABASE;
            break;
        case ProtocolRequestSql::DROP_DATABASE:
            type = DROP_DATABASE;
            break;
        case ProtocolRequestSql::ENABLE_DATABASE:
            type = ENABLE_DATABASE;
            break;
        case ProtocolRequestSql::DISABLE_DATABASE:
            type = DISABLE_DATABASE;
            break;
        case ProtocolRequestSql::GRANT_ACCESS:
            type = GRANT_ACCESS;
            break;
        case ProtocolRequestSql::CREATE_TABLE:
            type = CREATE_TABLE;
            break;
        case ProtocolRequestSql::DROP_TABLE:
            type = DROP_TABLE;
            break;
        case ProtocolRequestSql::REMOVE_TABLE_PARTITIONING:
            type = REMOVE_TABLE_PARTITIONING;
            break;
        case ProtocolRequestSql::DROP_TABLE_PARTITION:
            type = DROP_TABLE_PARTITION;
            break;
        case ProtocolRequestSql::GET_TABLE_INDEX:
            type = GET_TABLE_INDEX;
            break;
        case ProtocolRequestSql::CREATE_TABLE_INDEX:
            type = CREATE_TABLE_INDEX;
            break;
        case ProtocolRequestSql::DROP_TABLE_INDEX:
            type = DROP_TABLE_INDEX;
            break;
        case ProtocolRequestSql::ALTER_TABLE:
            type = ALTER_TABLE;
            break;
        case ProtocolRequestSql::TABLE_ROW_STATS:
            type = TABLE_ROW_STATS;
            break;
        default:
            throw runtime_error("SqlRequestParams::" + string(__func__) +
                                "  unsupported request type: " + ProtocolRequestSql_Type_Name(requestType));
    }
    if (request.has_query()) query = request.query();
    if (request.has_user()) user = request.user();
    if (request.has_password()) password = request.password();
    if (request.has_database()) database = request.database();
    if (request.has_table()) table = request.table();
    if (request.has_engine()) engine = request.engine();
    if (request.has_partition_by_column()) partitionByColumn = request.partition_by_column();
    if (request.has_transaction_id()) transactionId = request.transaction_id();

    for (int index = 0; index < request.columns_size(); ++index) {
        auto const& column = request.columns(index);
        columns.emplace_back(column.name(), column.type());
    }
    for (int index = 0; index < request.tables_size(); ++index) {
        tables.push_back(request.tables(index));
    }
    if (request.has_batch_mode()) batchMode = request.batch_mode();
    if (request.has_index_spec()) indexSpec = IndexSpec(request.index_spec());
    if (request.has_index_name()) indexName = request.index_name();
    if (request.has_index_comment()) indexComment = request.index_comment();

    for (int i = 0; i < request.index_columns_size(); ++i) {
        auto const& column = request.index_columns(i);
        indexColumns.emplace_back(column.name(), column.length(), column.ascending());
    }
}

string SqlRequestParams::type2str() const {
    switch (type) {
        case QUERY:
            return "QUERY";
        case CREATE_DATABASE:
            return "CREATE_DATABASE";
        case DROP_DATABASE:
            return "DROP_DATABASE";
        case ENABLE_DATABASE:
            return "ENABLE_DATABASE";
        case DISABLE_DATABASE:
            return "DISABLE_DATABASE";
        case GRANT_ACCESS:
            return "GRANT_ACCESS";
        case CREATE_TABLE:
            return "CREATE_TABLE";
        case DROP_TABLE:
            return "DROP_TABLE";
        case REMOVE_TABLE_PARTITIONING:
            return "REMOVE_TABLE_PARTITIONING";
        case DROP_TABLE_PARTITION:
            return "DROP_TABLE_PARTITION";
        case GET_TABLE_INDEX:
            return "GET_TABLE_INDEX";
        case CREATE_TABLE_INDEX:
            return "CREATE_TABLE_INDEX";
        case DROP_TABLE_INDEX:
            return "DROP_TABLE_INDEX";
        case ALTER_TABLE:
            return "ALTER_TABLE";
        case TABLE_ROW_STATS:
            return "TABLE_ROW_STATS";
    }
    throw runtime_error("SqlRequestParams::" + string(__func__) + "  unsupported request type");
}

SqlRequestParams::IndexSpec::IndexSpec(ProtocolRequestSql::IndexSpec spec) {
    switch (spec) {
        case ProtocolRequestSql::DEFAULT: {
            _spec = IndexSpec::DEFAULT;
            break;
        }
        case ProtocolRequestSql::UNIQUE: {
            _spec = IndexSpec::UNIQUE;
            break;
        }
        case ProtocolRequestSql::FULLTEXT: {
            _spec = IndexSpec::FULLTEXT;
            break;
        }
        case ProtocolRequestSql::SPATIAL: {
            _spec = IndexSpec::SPATIAL;
            break;
        }
        default:
            throw invalid_argument("SqlRequestParams::IndexSpec::" + string(__func__) +
                                   "  unsupported protocol index specification: '" +
                                   ProtocolRequestSql_IndexSpec_Name(spec) + "'");
    }
}

SqlRequestParams::IndexSpec::IndexSpec(string const& str) {
    if (str == "DEFAULT") {
        _spec = IndexSpec::DEFAULT;
    } else if (str == "UNIQUE") {
        _spec = IndexSpec::UNIQUE;
    } else if (str == "FULLTEXT") {
        _spec = IndexSpec::FULLTEXT;
    } else if (str == "SPATIAL") {
        _spec = IndexSpec::SPATIAL;
    } else {
        throw invalid_argument("SqlRequestParams::IndexSpec::" + string(__func__) +
                               "  unsupported index specification: '" + str + "'");
    }
}

string SqlRequestParams::IndexSpec::str() const {
    switch (_spec) {
        case IndexSpec::DEFAULT:
            return "DEFAULT";
        case IndexSpec::UNIQUE:
            return "UNIQUE";
        case IndexSpec::FULLTEXT:
            return "FULLTEXT";
        case IndexSpec::SPATIAL:
            return "SPATIAL";
    }
    throw runtime_error("SqlRequestParams::IndexSpec::" + string(__func__) +
                        "  unsupported index specification");
}

ProtocolRequestSql::IndexSpec SqlRequestParams::IndexSpec::protocol() const {
    switch (_spec) {
        case IndexSpec::DEFAULT:
            return ProtocolRequestSql::DEFAULT;
        case IndexSpec::UNIQUE:
            return ProtocolRequestSql::UNIQUE;
        case IndexSpec::FULLTEXT:
            return ProtocolRequestSql::FULLTEXT;
        case IndexSpec::SPATIAL:
            return ProtocolRequestSql::SPATIAL;
    }
    throw runtime_error("SqlRequestParams::IndexSpec::" + string(__func__) +
                        "  unsupported index specification: '" + str() + "'");
}

ostream& operator<<(ostream& os, SqlRequestParams const& params) {
    // Make the output to look like a serialized JSON object to allow parsing
    // log files using standard tools.

    json objParams;
    objParams["type"] = params.type2str();
    objParams["maxRows"] = params.maxRows;
    objParams["query"] = params.query;
    objParams["user"] = params.user;
    objParams["password"] = "******";
    objParams["database"] = params.database;
    objParams["table"] = params.table;
    objParams["engine"] = params.engine;
    objParams["partitionByColumn"] = params.partitionByColumn;
    objParams["transactionId"] = params.transactionId;
    objParams["batchMode"] = params.batchMode ? 1 : 0;

    json objParamsColumns = json::array();
    for (auto&& coldef : params.columns) {
        json objColDef;
        objColDef["name"] = coldef.name;
        objColDef["type"] = coldef.type;
        objParamsColumns.push_back(objColDef);
    }
    objParams["columns"] = objParamsColumns;

    json objParamsTables = json::array();
    for (auto&& table : params.tables) {
        objParamsTables.push_back(table);
    }
    objParams["tables"] = objParamsTables;
    objParams["index_spec"] = params.indexSpec.str();
    objParams["index_name"] = params.indexName;
    objParams["index_comment"] = params.indexComment;

    json objParamsIndexColumns = json::array();
    for (auto&& column : params.indexColumns) {
        json objColumn;
        objColumn["name"] = column.name;
        objColumn["length"] = column.length;
        objColumn["ascending"] = column.ascending ? 1 : 0;
        objParamsIndexColumns.push_back(objColumn);
    }
    objParams["index_columns"] = objParamsIndexColumns;

    json obj;
    obj["SqlRequestParams"] = objParams;

    os << obj.dump();
    return os;
}

DirectorIndexRequestParams::DirectorIndexRequestParams(ProtocolRequestDirectorIndex const& request)
        : database(request.database()),
          chunk(request.chunk()),
          hasTransactions(request.has_transactions()),
          transactionId(request.transaction_id()) {}

string tableNameBuilder(string const& databaseName, string const& tableName, string const& suffix) {
    size_t const tableNameLimit = 64;
    string const name = databaseName + "__" + tableName + suffix;
    if (name.size() > tableNameLimit) {
        throw invalid_argument("replica::" + string(__func__) + " MySQL table name limit of " +
                               to_string(tableNameLimit) + " characters has been exceeded for table '" +
                               name + "'.");
    }
    return name;
}

}  // namespace lsst::qserv::replica
