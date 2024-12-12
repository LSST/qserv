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
#include "replica/config/ConfigTestData.h"

using namespace std;
using json = nlohmann::json;

namespace lsst::qserv::replica {

map<string, set<string>> ConfigTestData::parameters() {
    return map<string, set<string>>(
            {{"common", {"request-buf-size-bytes", "request-retry-interval-sec"}},
             {"registry", {"host", "port", "max-listen-conn", "threads", "heartbeat-ival-sec"}},
             {"controller",
              {"num-threads", "http-server-port", "http-max-listen-conn", "http-server-threads",
               "request-timeout-sec", "job-timeout-sec", "job-heartbeat-sec", "max-repl-level",
               "worker-evict-priority-level", "health-monitor-priority-level", "ingest-priority-level",
               "catalog-management-priority-level", "auto-register-workers", "auto-register-czars",
               "ingest-job-monitor-ival-sec", "num-director-index-connections", "director-index-engine"}},
             {"database",
              {"services-pool-size", "host", "port", "user", "password", "name", "qserv-master-user",
               "qserv-master-services-pool-size", "qserv-master-tmp-dir"}},
             {"xrootd",
              {"auto-notify", "request-timeout-sec", "host", "port", "allow-reconnect", "reconnect-timeout"}},
             {"worker",
              {"num-threads",
               "num-svc-processing-threads",
               "num-http-svc-threads",
               "num-fs-processing-threads",
               "fs-buf-size-bytes",
               "num-loader-processing-threads",
               "num-exporter-processing-threads",
               "num-http-loader-processing-threads",
               "num-async-loader-processing-threads",
               "async-loader-auto-resume",
               "async-loader-cleanup-on-resume",
               "http-max-listen-conn",
               "http-max-queued-requests",
               "http-svc-max-queued-requests",
               "svc-port",
               "http-svc-port",
               "fs-port",
               "data-dir",
               "loader-max-warnings",
               "loader-port",
               "loader-tmp-dir",
               "exporter-port",
               "exporter-tmp-dir",
               "http-loader-port",
               "http-loader-tmp-dir",
               "ingest-charset-name",
               "ingest-num-retries",
               "ingest-max-retries",
               "director-index-record-size"}}});
}

json ConfigTestData::data() {
    json obj;
    json& generalObj = obj["general"];
    generalObj["common"] =
            json::object({{"request-buf-size-bytes", 8192}, {"request-retry-interval-sec", 1}});
    generalObj["registry"] = json::object({{"host", "127.0.0.1"},
                                           {"port", 8081},
                                           {"max-listen-conn", 512},
                                           {"threads", 4},
                                           {"heartbeat-ival-sec", 10}});
    generalObj["controller"] = json::object({{"num-threads", 2},
                                             {"http-server-port", 8080},
                                             {"http-max-listen-conn", 256},
                                             {"http-server-threads", 3},
                                             {"request-timeout-sec", 100},
                                             {"job-timeout-sec", 200},
                                             {"job-heartbeat-sec", 300},
                                             {"max-repl-level", 2},
                                             {"worker-evict-priority-level", 1},
                                             {"health-monitor-priority-level", 2},
                                             {"ingest-priority-level", 3},
                                             {"catalog-management-priority-level", 4},
                                             {"auto-register-workers", 1},
                                             {"auto-register-czars", 0},
                                             {"ingest-job-monitor-ival-sec", 5},
                                             {"num-director-index-connections", 6},
                                             {"director-index-engine", "MyISAM"}});
    generalObj["database"] = json::object({{"host", "localhost"},
                                           {"port", 13306},
                                           {"user", "qsreplica"},
                                           {"password", "changeme"},
                                           {"name", "qservReplica"},
                                           {"qserv-master-user", "qsmaster"},
                                           {"services-pool-size", 2},
                                           {"qserv-master-tmp-dir", "/qserv/data/ingest"}});
    generalObj["xrootd"] = json::object({{"auto-notify", 0},
                                         {"host", "localhost"},
                                         {"port", 1104},
                                         {"request-timeout-sec", 400},
                                         {"allow-reconnect", 0},
                                         {"reconnect-timeout", 500}});
    generalObj["worker"] = json::object({{"num-threads", 3},
                                         {"num-svc-processing-threads", 4},
                                         {"num-http-svc-threads", 10},
                                         {"num-fs-processing-threads", 5},
                                         {"fs-buf-size-bytes", 1024},
                                         {"num-loader-processing-threads", 6},
                                         {"num-exporter-processing-threads", 7},
                                         {"num-http-loader-processing-threads", 8},
                                         {"num-async-loader-processing-threads", 9},
                                         {"async-loader-auto-resume", 0},
                                         {"async-loader-cleanup-on-resume", 0},
                                         {"http-max-listen-conn", 512},
                                         {"http-max-queued-requests", 1024},
                                         {"http-svc-max-queued-requests", 2048},
                                         {"svc-port", 51000},
                                         {"http-svc-port", 56000},
                                         {"fs-port", 52000},
                                         {"data-dir", "/data"},
                                         {"loader-max-warnings", 2},
                                         {"loader-port", 53000},
                                         {"loader-tmp-dir", "/tmp"},
                                         {"exporter-port", 54000},
                                         {"exporter-tmp-dir", "/tmp"},
                                         {"http-loader-port", 55000},
                                         {"http-loader-tmp-dir", "/tmp"}});

    obj["workers"] = json::array();
    {
        // A configuration of this worker is complete as it has all required
        // parameters.
        json worker = json::object({{"name", "worker-A"},
                                    {"is-enabled", 1},
                                    {"is-read-only", 0},
                                    {"svc-host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                    {"svc-port", 51001},
                                    {"http-svc-host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                    {"http-svc-port", 56001},
                                    {"fs-host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                    {"fs-port", 52001},
                                    {"data-dir", "/data/A"},
                                    {"loader-host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                    {"loader-port", 53002},
                                    {"loader-tmp-dir", "/tmp/A"},
                                    {"exporter-host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                    {"exporter-port", 53003},
                                    {"exporter-tmp-dir", "/tmp/export/A"},
                                    {"http-loader-host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                    {"http-loader-port", 53004},
                                    {"http-loader-tmp-dir", "/tmp/http/A"}});
        worker["qserv-worker"]["host"] = json::object({{"addr", "127.0.0.1"}, {"name", "host-A"}});
        worker["qserv-worker"]["port"] = 53004;
        obj["workers"].push_back(worker);
    }
    {
        // This configuration is incomplete. An assumption is that the corresponding
        // defaults will be loaded when the Configuration class will be processing
        // this definition.
        json worker = json::object({{"name", "worker-B"},
                                    {"is-enabled", 1},
                                    {"is-read-only", 1},
                                    {"svc-host", {{"addr", "168.1.1.1"}, {"name", "host-B"}}},
                                    {"http-svc-host", {{"addr", "168.1.1.1"}, {"name", "host-B"}}},
                                    {"fs-host", {{"addr", "168.1.1.1"}, {"name", "host-B"}}},
                                    {"data-dir", "/data/B"},
                                    {"loader-host", {{"addr", "168.1.1.1"}, {"name", "host-B"}}},
                                    {"exporter-host", {{"addr", "168.1.1.1"}, {"name", "host-B"}}},
                                    {"http-loader-host", {{"addr", "168.1.1.1"}, {"name", "host-B"}}}});
        worker["qserv-worker"]["host"] = json::object({{"addr", "168.1.1.1"}, {"name", "host-B"}});
        worker["qserv-worker"]["port"] = 53004;
        obj["workers"].push_back(worker);
    }
    {
        // This configuration is incomplete. An assumption is that the corresponding
        // defaults will be loaded when the Configuration class will be processing
        // this definition.
        json worker = json::object({{"name", "worker-C"},
                                    {"is-enabled", 0},
                                    {"is-read-only", 0},
                                    {"svc-host", {{"addr", "168.1.1.1"}, {"name", "host-C1"}}},
                                    {"http-svc-host", {{"addr", "168.1.1.7"}, {"name", "host-C7"}}},
                                    {"fs-host", {{"addr", "168.1.1.2"}, {"name", "host-C2"}}},
                                    {"loader-host", {{"addr", "168.1.1.3"}, {"name", "host-C3"}}},
                                    {"exporter-host", {{"addr", "168.1.1.4"}, {"name", "host-C4"}}},
                                    {"http-loader-host", {{"addr", "168.1.1.5"}, {"name", "host-C5"}}}});
        worker["qserv-worker"]["host"] = json::object({{"addr", "168.1.1.6"}, {"name", "host-C6"}});
        worker["qserv-worker"]["port"] = 53005;
        obj["workers"].push_back(worker);
    }
    obj["database_families"] = json::array();
    {
        json family = json::object({{"name", "production"},
                                    {"min_replication_level", 1},
                                    {"num_stripes", 11},
                                    {"num_sub_stripes", 12},
                                    {"overlap", 0.01667}});
        obj["database_families"].push_back(family);
    }
    {
        json family = json::object({{"name", "test"},
                                    {"min_replication_level", 2},
                                    {"num_stripes", 14},
                                    {"num_sub_stripes", 15},
                                    {"overlap", 0.001}});
        obj["database_families"].push_back(family);
    }
    obj["databases"] = json::array();
    {
        json database = json::object({{"database", "db1"},
                                      {"family_name", "production"},
                                      {"is_published", 1},
                                      {"create_time", 10},
                                      {"publish_time", 11},
                                      {"tables", json::array()}});
        {
            json table = json::object({{"name", "Table11"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id11"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl11"},
                                       {"longitude_key", "ra11"},
                                       {"is_published", 1},
                                       {"create_time", 110},
                                       {"publish_time", 111},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id11"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl11"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra11"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable11"},
                                       {"is_partitioned", 0},
                                       {"is_published", 1},
                                       {"create_time", 120},
                                       {"publish_time", 121}});
            database["tables"].push_back(table);
        }
        obj["databases"].push_back(database);
    }
    {
        json database = json::object({{"database", "db2"},
                                      {"family_name", "production"},
                                      {"is_published", 1},
                                      {"create_time", 20},
                                      {"publish_time", 21},
                                      {"tables", json::array()}});
        {
            json table = json::object({{"name", "Table21"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id21"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 0},
                                       {"latitude_key", "decl21"},
                                       {"longitude_key", "ra21"},
                                       {"is_published", 1},
                                       {"create_time", 210},
                                       {"publish_time", 211},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id21"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl21"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra21"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "Table22"},
                                       {"is_partitioned", 1},
                                       {"director_table", "Table21"},
                                       {"director_key", "id22"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl22"},
                                       {"longitude_key", "ra22"},
                                       {"is_published", 1},
                                       {"create_time", 220},
                                       {"publish_time", 221},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id22"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl22"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra22"}, {"type", "DOUBLE NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable21"},
                                       {"is_partitioned", 0},
                                       {"is_published", 1},
                                       {"create_time", 2210},
                                       {"publish_time", 2211}});
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable22"},
                                       {"is_partitioned", 0},
                                       {"is_published", 1},
                                       {"create_time", 2220},
                                       {"publish_time", 2221}});
            database["tables"].push_back(table);
        }
        obj["databases"].push_back(database);
    }
    {
        json database = json::object({{"database", "db3"},
                                      {"family_name", "production"},
                                      {"is_published", 1},
                                      {"create_time", 30},
                                      {"publish_time", 31},
                                      {"tables", json::array()}});
        {
            json table = json::object({{"name", "Table31"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id31"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl31"},
                                       {"longitude_key", "ra31"},
                                       {"is_published", 1},
                                       {"create_time", 310},
                                       {"publish_time", 311},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id31"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl31"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra31"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "Table32"},
                                       {"is_partitioned", 1},
                                       {"director_table", "Table31"},
                                       {"director_key", "id32"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl32"},
                                       {"longitude_key", "ra32"},
                                       {"is_published", 1},
                                       {"create_time", 320},
                                       {"publish_time", 321},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id32"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl32"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra32"}, {"type", "DOUBLE NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "Table33"},
                                       {"is_partitioned", 1},
                                       {"director_table", "Table31"},
                                       {"director_key", "id33"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", ""},
                                       {"longitude_key", ""},
                                       {"is_published", 1},
                                       {"create_time", 330},
                                       {"publish_time", 331},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id33"}, {"type", "BIGINT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable31"},
                                       {"is_partitioned", 0},
                                       {"is_published", 1},
                                       {"create_time", 3310},
                                       {"publish_time", 3311}});
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable32"},
                                       {"is_partitioned", 0},
                                       {"is_published", 1},
                                       {"create_time", 3320},
                                       {"publish_time", 3321}});
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable33"},
                                       {"is_partitioned", 0},
                                       {"is_published", 0},
                                       {"create_time", 3330},
                                       {"publish_time", 0}});
            database["tables"].push_back(table);
        }
        obj["databases"].push_back(database);
    }
    {
        json database = json::object({{"database", "db4"},
                                      {"family_name", "test"},
                                      {"is_published", 1},
                                      {"create_time", 40},
                                      {"publish_time", 41},
                                      {"tables", json::array()}});
        {
            json table = json::object({{"name", "Table41"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id41"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl41"},
                                       {"longitude_key", "ra41"},
                                       {"is_published", 1},
                                       {"create_time", 410},
                                       {"publish_time", 411},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id41"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl41"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra41"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "Table42"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id42"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl42"},
                                       {"longitude_key", "ra42"},
                                       {"is_published", 1},
                                       {"create_time", 420},
                                       {"publish_time", 421},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id42"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl42"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra42"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "RefMatch43"},
                                       {"is_partitioned", 1},
                                       {"director_table", "Table41"},
                                       {"director_key", "Table41_id"},
                                       {"director_table2", "Table42"},
                                       {"director_key2", "Table42_id"},
                                       {"flag", "flag"},
                                       {"ang_sep", 0.01},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", ""},
                                       {"longitude_key", ""},
                                       {"is_published", 0},
                                       {"create_time", 430},
                                       {"publish_time", 0},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "Table41_id"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "Table42_id"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "flag"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "RefMatch44"},
                                       {"is_partitioned", 1},
                                       {"director_table", "db2.Table21"},
                                       {"director_key", "Table21_id"},
                                       {"director_table2", "db3.Table31"},
                                       {"director_key2", "Table31_id"},
                                       {"flag", "flag"},
                                       {"ang_sep", 0.01667},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", ""},
                                       {"longitude_key", ""},
                                       {"is_published", 0},
                                       {"create_time", 440},
                                       {"publish_time", 0},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "Table21_id"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "Table31_id"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "flag"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        obj["databases"].push_back(database);
    }
    {
        json database = json::object({{"database", "db5"},
                                      {"family_name", "test"},
                                      {"is_published", 1},
                                      {"create_time", 50},
                                      {"publish_time", 51},
                                      {"tables", json::array()}});
        {
            json table = json::object({{"name", "Table51"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id51"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl51"},
                                       {"longitude_key", "ra51"},
                                       {"is_published", 1},
                                       {"create_time", 510},
                                       {"publish_time", 511},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id51"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl51"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra51"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        obj["databases"].push_back(database);
    }
    {
        json database = json::object({{"database", "db6"},
                                      {"family_name", "test"},
                                      {"is_published", 0},
                                      {"create_time", 60},
                                      {"publish_time", 0},
                                      {"tables", json::array()}});
        {
            json table = json::object({{"name", "Table61"},
                                       {"is_partitioned", 1},
                                       {"director_table", ""},
                                       {"director_key", "id61"},
                                       {"director_table2", ""},
                                       {"director_key2", ""},
                                       {"flag", ""},
                                       {"ang_sep", 0.0},
                                       {"unique_primary_key", 1},
                                       {"latitude_key", "decl61"},
                                       {"longitude_key", "ra61"},
                                       {"is_published", 0},
                                       {"create_time", 610},
                                       {"publish_time", 0},
                                       {"columns", json::array()}});
            json& columns = table["columns"];
            columns.push_back(json::object({{"name", "id61"}, {"type", "BIGINT NOT NULL"}}));
            columns.push_back(json::object({{"name", "decl61"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "ra61"}, {"type", "DOUBLE NOT NULL"}}));
            columns.push_back(json::object({{"name", "subChunkId"}, {"type", "INT NOT NULL"}}));
            database["tables"].push_back(table);
        }
        {
            json table = json::object({{"name", "MetaTable61"},
                                       {"is_partitioned", 0},
                                       {"is_published", 1},
                                       {"create_time", 6610},
                                       {"publish_time", 6611}});
            database["tables"].push_back(table);
        }
        obj["databases"].push_back(database);
    }
    obj["czars"] = json::array();
    {
        json czar = json::object({{"name", "default"},
                                  {"id", 123},
                                  {"host", {{"addr", "127.0.0.1"}, {"name", "host-A"}}},
                                  {"port", 59001}});
        obj["czars"].push_back(czar);
    }
    return obj;
}

}  // namespace lsst::qserv::replica
