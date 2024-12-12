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
#include "replica/config/ConfigurationSchema.h"

// System headers
#include <algorithm>
#include <thread>

// Third-party headers
#include "boost/asio.hpp"

// Qserv headers
#include "global/constants.h"
#include "replica/util/Common.h"
#include "replica/util/ProtocolBuffer.h"

using namespace std;
using json = nlohmann::json;

namespace {
int const max_listen_connections = boost::asio::socket_base::max_listen_connections;
int const num_threads = std::thread::hardware_concurrency();
}  // namespace

namespace lsst::qserv::replica {

/**
 * The schema definition is nested dictionary in which the top-level key reprsents
 * the so called "categories" of parameters. Each entry under a category defines
 * a single parameter. Values of these parameters are obtained and modified
 * using the Configuration API methods 'get<T>` and 'set<T>`.
 *
 * All parameters have two mandatory attributes:
 *  - The attribute "description" contains the documentation string explaining the attribute
 *  - The attribute "default" holds the default value of the attribute. The value's type depends
 *    on the attribute's role, and once it's defined here it's enforced through the rest of
 *    the implementation. For instance, the type can't be changed via the method 'Configuration::set<T>'.
 *
 * Some parameters are also allowed to have the optional attributes:
 *   - The attribute "read-only" set to 1 would indicate that the parameter's state
 *     can't be changed via method 'Configuration::set<T>'.
 *   - The attribute "empty-allowed" set to 1 would relax parameter value's validation
 *     by method 'Configuration::set<T>' to allow 0 for numeric types and the empty string
 *     fr strings.
 *   - The attribute "security-context" if set to 1 would indicate to the API user that
 *     the parameter has some the security-sensitive context (passwords, authorization keys,
 *     etc.). Parameters possesing this attribute are supposed to be used with care by
 *     the dependent automation tools to avoid exposing sensitive information in log files,
 *     reports, etc.
 */
json const ConfigurationSchema::_schemaJson = json::object(
        {{"common",
          {{"request-buf-size-bytes",
            {{"description", "The default buffer size for network communications. Must be greater than 0."},
             {"default", 131072}}},
           {"request-retry-interval-sec",
            {{"description", "The default retry timeout for network communications. Must be greater than 0."},
             {"default", 1}}}}},
         {"registry",
          {{"host",
            {{"description", "The IP address or the DNS host name for the registry's HTTP server."},
             {"default", "localhost"}}},
           {"port",
            {{"description", "The port number for the registry's HTTP server. Must be greater than 0."},
             {"default", 25082}}},
           {"max-listen-conn",
            {{"description",
              "The maximum length of the queue of pending connections sent to the registry's HTTP server."
              " Must be greater than 0."},
             {"default", max_listen_connections}}},
           {"threads",
            {{"description",
              "The number of threads managed by BOOST ASIO for the HTTP server. Must be greater than 0."},
             {"default", min(8, num_threads)}}},
           {"heartbeat-ival-sec",
            {{"description",
              "The heartbeat interval for interactions with the workers Registry service. Must be greater "
              "than 0."},
             {"default", 5}}}}},
         {"controller",
          {{"num-threads",
            {{"description", "The number of threads managed by BOOST ASIO. Must be greater than 0."},
             {"default", min(8, num_threads)}}},
           {"request-timeout-sec",
            {{"description",
              "The default timeout for completing worker requests. A value depends on"
              " a scale of catalogs served by Qserv and ingested by the Replication/Ingest system."
              " It's recommended to set this parameter to 3600 seconds or higher. The value must be"
              " greater than 0."},
             {"default", 28800}}},
           {"job-timeout-sec",
            {{"description",
              "The default timeout for completing jobs. A value depends on"
              " a scale of catalogs served by Qserv and ingested by the Replication/Ingest system."
              " Some complex jobs run on the large catalogs may take many hours."
              " It's recommended to set this parameter to 3600 seconds or higher. The value must be"
              " greater than 0."},
             {"default", 86400}}},
           {"job-heartbeat-sec",
            {{"description", "The heartbeat interval for jobs. A value of 0 disables heartbeats."},
             {"empty-allowed", 1},
             {"default", 0}}},
           {"http-server-threads",
            {{"description",
              "The number of threads managed by BOOST ASIO for the HTTP server. Must be greater than 0."},
             {"default", min(8, num_threads)}}},
           {"http-server-port",
            {{"description", "The port number for the controller's HTTP server. Must be greater than 0."},
             {"default", 25081}}},
           {"http-max-listen-conn",
            {{"description",
              "The maximum length of the queue of pending connections sent to the controller's HTTP server."
              " Must be greater than 0."},
             {"default", max_listen_connections}}},
           {"max-repl-level",
            {{"description",
              "The maximum replicaton level that applies to any database family. This"
              " hard limit is meant to prevent the Replication system's algorithms from utilizing too"
              " much storage. The limit is enforced at run time. The number must be greater than 0."},
             {"default", 4}}},
           {"worker-evict-priority-level",
            {{"description",
              "The priority level of the worker eviction task that is run to compensate for"
              " the missing chunk replicas should be a worker became offline for an extended"
              " period of time."},
             {"empty-allowed", 1},
             {"default", PRIORITY_VERY_HIGH}}},
           {"health-monitor-priority-level",
            {{"description", "The priority level of the Cluster Health Monitoring task."},
             {"empty-allowed", 1},
             {"default", PRIORITY_VERY_HIGH}}},
           {"ingest-priority-level",
            {{"description", "The priority level of the time-critical catalog ingest activities."},
             {"empty-allowed", 1},
             {"default", PRIORITY_HIGH}}},
           {"catalog-management-priority-level",
            {{"description",
              "The priority level of the routine catalog management activities, such as scanning"
              " and recording replica dispositions, fixing up missing replicas, etc."},
             {"empty-allowed", 1},
             {"default", PRIORITY_LOW}}},
           {"auto-register-workers",
            {{"description",
              "Automatically scale a collection of workers by registering new workers reported by the "
              "Registry"
              " service. If the flag is set to 0 then new workers will be ignored."},
             {"empty-allowed", 1},
             {"default", 0}}},
           {"auto-register-czars",
            {{"description",
              "Automatically scale a collection of Czars by registering new Czars reported by the "
              "Registry"
              " service. If the flag is set to 0 then new Czars will be ignored."},
             {"empty-allowed", 1},
             {"default", 1}}},
           {"ingest-job-monitor-ival-sec",
            {{"description",
              "An interval (seconds) for monitoring progress of jobs submitted by the Controller during"
              " asynchronous ingest operations."},
             {"default", 60}}},
           {"num-director-index-connections",
            {{"description",
              "The number of the MySQL connection to the Qserv 'czar's database in the connection pool that"
              " is used by the 'director' index builder job. If using the InnoDB storage engine for"
              " the 'director' index table, a value of this parameter should be set to 2,"
              " which would allow the second MySQL thread to prepare data while the first thread"
              " is loading data into the table. Setting the parameter to some large number won't"
              " yield any benefits in terms of the overall performance of the index ingest. This"
              " will just result in the useless increase in the CPU time consumed by MySQL."},
             {"default", 2}}},
           {"director-index-engine",
            {{"description", "The default MySQL engine of the 'director' index tables."},
             {"default", "InnoDB"}}}}},
         {"database",
          {{"services-pool-size",
            {{"description", "The pool size at the client database services connector."},
             {"default", max(8, num_threads)}}},
           {"host",
            {{"description",
              "The host name of the MySQL server where the Replication system maintains its persistent state."
              " Note that this parameter can't be updated through the Configuration service as it's"
              " set up at the startup time of the Replication/Ingest system."},
             {"read-only", 1},
             {"default", "localhost"}}},
           {"port",
            {{"description",
              "The port number of the MySQL server where the Replication maintains its persistent state."
              " Note that this parameter can't be updated through the Configuration service as it's"
              " set up at the startup time of the Replication/Ingest system."},
             {"read-only", 1},
             {"default", 3306}}},
           {"user",
            {{"description",
              "The MySQL user account of a service where the Replication system maintains its persistent "
              "state."
              " Note that this parameter can't be updated through the Configuration service as it's"
              " set up at the startup time of the Replication/Ingest system."},
             {"read-only", 1},
             {"default", "qsreplica"}}},
           {"password",
            {{"description",
              "A password for the MySQL account where the Replication system maintains its persistent state"},
             {"read-only", 1},
             {"security-context", 1},
             {"empty-allowed", 1},
             {"default", ""}}},
           {"name",
            {{"description",
              "The name of a MySQL database for a service where the Replication system maintains its"
              " persistent state. Note that this parameter can't be updated through the Configuration"
              "  service as it's set up at the startup time of the Replication/Ingest system."},
             {"read-only", 1},
             {"default", "qservReplica"}}},
           {"qserv-master-services-pool-size",
            {{"description",
              "The pool size at the client database services connector for the Qserv Master database."},
             {"default", 2}}},
           {"qserv-master-user",
            {{"description",
              "The MySQL user account of a service where Qserv 'czar' maintains its persistent state."},
             {"default", "qsmaster"}}},
           {"qserv-master-tmp-dir",
            {{"description",
              "The temporary folder for exchanging data with the Qserv 'czar' database service."},
             {"default", "/qserv/data/ingest"}}}}},
         {"xrootd",
          {{"auto-notify",
            {{"description", "Automatically notify Qserv on changes in replica disposition."},
             {"empty-allowed", 1},
             {"default", 1}}},
           {"request-timeout-sec",
            {{"description", "The default timeout for communications with Qserv over XRootD/SSI."},
             {"default", 1800}}},
           {"host",
            {{"description",
              "The service location (the host name or an IP address) of XRootD/SSI for"
              " communications with Qserv."},
             {"default", "localhost"}}},
           {"port",
            {{"description",
              "A port number for the XRootD/SSI service needed for communications with Qserv."},
             {"default", 1094}}},
           {"allow-reconnect",
            {{"description",
              "XRootD/SSI connection handling mode. Set 0 to disable automatic reconnects."
              " Any other number would allow reconnects."},
             {"empty-allowed", 1},
             {"default", 1}}},
           {"reconnect-timeout",
            {{"description",
              "The default value limiting a duration of time for making automatic"
              " reconnects to the XRootD/SSI services before failing and reporting error"
              " (if the server is not up, or if it's not reachable for some reason)"},
             {"default", 3600}}}}},
         {"worker",
          {{"num-threads",
            {{"description", "The number of threads managed by BOOST ASIO. Must be greater than 0."},
             {"default", min(8, num_threads)}}},
           {"num-svc-processing-threads",
            {{"description", "The number of request processing threads in each Replication worker service."},
             {"default", min(8, num_threads)}}},
           {"num-http-svc-threads",
            {{"description",
              "The number of threads in each HTTP server frontend of Replication worker service."},
             {"default", min(8, num_threads)}}},
           {"num-fs-processing-threads",
            {{"description",
              "The number of request processing threads in each Replication worker's file service."},
             {"default", min(8, num_threads)}}},
           {"fs-buf-size-bytes",
            {{"description",
              "The default buffer size for file and network operations at Replication worker's file "
              "service."},
             {"default", 4194304}}},
           {"num-loader-processing-threads",
            {{"description",
              "The number of request processing threads in each Replication worker's ingest service."},
             {"default", num_threads}}},
           {"num-exporter-processing-threads",
            {{"description",
              "The number of request processing threads in each Replication worker's data exporting "
              "service."},
             {"default", num_threads}}},
           {"num-http-loader-processing-threads",
            {{"description",
              "The number of request processing threads in each Replication worker's HTTP-based ingest "
              "service."},
             {"default", min(8, num_threads)}}},
           {"num-async-loader-processing-threads",
            {{"description",
              "The number of request processing threads in each Replication worker's ASYNC ingest service."},
             {"default", min(8, num_threads)}}},
           {"async-loader-auto-resume",
            {{"description",
              "The flag controlling the behavior of Replication worker's ASYNC ingest service after"
              " its (deliberate or accidental) restarts. If the value of the parameter is not 0 then"
              " the service will resume processing incomplete (queued or on-going) requests."
              " Note that requests that were in the final state of loading data into MySQL before"
              " the restart won't be resumed. These will be marked as failed."
              " Setting a value of the parameter to 0 will result in failing all incomplete contribution"
              " requests existed before the restart. Note that requests failed in the last (loading) stage"
              " can't be resumed, and they will require aborting the corresponding super-transaction."},
             {"empty-allowed", 1},
             {"default", 1}}},
           {"async-loader-cleanup-on-resume",
            {{"description",
              "The flag controlling the behavior of Replication worker's ASYNC ingest service after"
              " a restart of the service. If the value of the parameter is not 0 the service will"
              " try cleaning up temporary files that might be left on disk by incomplete (queued or on-going)"
              " requests. This option may be disabled to allow debugging the service."},
             {"empty-allowed", 1},
             {"default", 1}}},
           {"http-max-listen-conn",
            {{"description",
              "The maximum length of the queue of pending connections sent to the Replication worker's"
              " HTTP-based ingest service. Must be greater than 0."},
             {"default", max_listen_connections}}},
           {"http-max-queued-requests",
            {{"description",
              "The maximum number of pending requests, i.e. requests accept()ed by"
              " the listener but still waiting to be routed by the HTTP server."
              " If set to 0 then no specific limit will be enforced. It's recommented to keep"
              " the default value unless there are specific reasons to change it."},
             {"empty-allowed", 1},
             {"default", 0}}},
           {"http-svc-max-queued-requests",
            {{"description",
              "The maximum number of pending requests, i.e. requests accept()ed by"
              " the listener but still waiting to be routed by the HTTP-based Worker Replication server."
              " If set to 0 then no specific limit will be enforced. It's recommented to keep"
              " the default value unless there are specific reasons to change it."},
             {"empty-allowed", 1},
             {"default", 0}}},
           {"svc-port",
            {{"description", "The port number for the worker replication service."}, {"default", 25000}}},
           {"http-svc-port",
            {{"description", "The port number for the HTTP-based worker replication service."},
             {"default", 25005}}},
           {"fs-port",
            {{"description", "The port number for the worker's file service."}, {"default", 25001}}},
           {"data-dir",
            {{"description",
              "The data directory from which the worker file service serves files"
              " to other workers. This folder is required to be the location where the MySQL"
              " service of Qserv worker stores its data."},
             {"default", "/qserv/data/mysql"}}},
           {"loader-max-warnings",
            {{"description",
              "The maximum number of warnings to retain after executing LOAD DATA [LOCAL] INFILE"
              " when ingesting contributions into the adjacent Qserv worker's MySQL database."
              " The warnings (if any) will be recorded in the persisent state of the"
              " Replication/Inhgest system and returned to the ingest workflow upon request."},
             {"default", 64}}},
           {"loader-port",
            {{"description", "The port number for the worker's binary file ingest service."},
             {"default", 25002}}},
           {"loader-tmp-dir",
            {{"description",
              "A location for temporary files stored by the worker's binary"
              " file ingest service before ingesting them into the adjacent Qserv worker's"
              " MySQL database."},
             {"default", "/qserv/data/ingest"}}},
           {"exporter-port",
            {{"description", "The port number for the worker's table export service."}, {"default", 25003}}},
           {"exporter-tmp-dir",
            {{"description",
              "A location for temporary files stored by the worker's table"
              " export service before returning them a client."},
             {"default", "/qserv/data/export"}}},
           {"http-loader-port",
            {{"description",
              "The port number for the worker's HTTP-based REST service for ingesting table"
              " contributions into the adjacent Qserv worker's MySQL database."},
             {"default", 25004}}},
           {"http-loader-tmp-dir",
            {{"description",
              "A location for temporary files stored by the worker's"
              " HTTP-based REST service ingesting table before ingesting them into"
              " the adjacent Qserv worker's MySQL database."},
             {"default", "/qserv/data/ingest"}}},
           {"ingest-charset-name",
            {{"description", "The name of a character set for parsing the payload of the contributions."},
             {"default", "latin1"}}},
           {"ingest-num-retries",
            {{"description",
              "The default number of the automated retries of failed contribution attempts"
              " in cases when such retries are still possible. The limit can be changed for"
              " individual contributions. Note that the effective number of retries specified"
              " by this parameter or the one set in the contribution requests can not"
              " exceed the 'hard' limit set in the related parameter 'worker','ingest-max-retries'."
              " Setting a value of the parameter to 0 will disable automatic retries (unless they are"
              " explicitly enabled or requested by the ingest workflows for individual contributions)."},
             {"empty-allowed", 1},
             {"default", 1}}},
           {"ingest-max-retries",
            {{"description",
              "The maximum number of the automated retries of failed contribution attempts"
              " in cases when such retries are still possible. The parameter represents the 'hard'"
              " limit for the number of retries regardless of what's specified in the related"
              " parameter 'worker','ingest-num-retries' or in the contributions requests."
              " The primary purpose of the parameter is to prevent accidental overloading"
              " of the ingest system should a very large number of retries accidentally specified"
              " by the ingest workflows for individual contributions. Setting a value of the parameter"
              " to 0 will unconditionally disable any retries."},
             {"empty-allowed", 1},
             {"default", 10}}},
           {"director-index-record-size",
            {{"description",
              "The recommended record size (in bytes) for reading from the 'director' index file."
              " Note that the size should not exceed the 'hard' limit of the Google Protobuf message"
              " size of " +
                      to_string(ProtocolBuffer::HARD_LIMIT) +
                      " bytes. Any number set higher than this limit will"
                      " get truncated down to match the limit at run time."},
             {"default", 16 * 1024 * 1024}}}}}});

string ConfigurationSchema::description(string const& category, string const& param) {
    return _attributeValue<string>(category, param, "description", "");
}

bool ConfigurationSchema::readOnly(string const& category, string const& param) {
    return _attributeValue<unsigned int>(category, param, "read-only", 0) != 0;
}

bool ConfigurationSchema::securityContext(string const& category, string const& param) {
    return _attributeValue<unsigned int>(category, param, "security-context", 0) != 0;
}

string ConfigurationSchema::defaultValueAsString(string const& category, string const& param) {
    return json2string("ConfigurationSchema::" + string(__func__) + " category: '" + category + "' param: '" +
                               param + "' ",
                       _attributeValueJson(category, param, "default"));
}

json ConfigurationSchema::defaultConfigData() {
    json result = json::object();
    vector<string> const generalCategories = {"common",   "registry", "controller",
                                              "database", "xrootd",   "worker"};
    for (string const& category : generalCategories) {
        json const& inCategoryJson = _schemaJson.at(category);
        json& outCategoryJson = result[category];
        for (auto&& itr : inCategoryJson.items()) {
            string const& param = itr.key();
            outCategoryJson[param] = itr.value().at("default");
        }
    }
    return result;
}

map<string, set<string>> ConfigurationSchema::parameters() {
    map<string, set<string>> result;
    json const data = defaultConfigData();
    for (auto&& categoryItr : data.items()) {
        string const& category = categoryItr.key();
        for (auto&& parameterItr : data.at(category).items()) {
            string const& parameter = parameterItr.key();
            result[category].insert(parameter);
        }
    }
    return result;
}

string ConfigurationSchema::json2string(string const& context, json const& obj) {
    if (obj.is_string()) return obj.get<string>();
    if (obj.is_boolean()) return obj.get<bool>() ? "1" : "0";
    if (obj.is_number_unsigned()) return to_string(obj.get<uint64_t>());
    if (obj.is_number_integer()) return to_string(obj.get<int64_t>());
    if (obj.is_number_float()) return to_string(obj.get<double>());
    throw invalid_argument(context + "unsupported data type of the value: " + obj.dump());
}

bool ConfigurationSchema::_emptyAllowed(string const& category, string const& param) {
    return _attributeValue<unsigned int>(category, param, "empty-allowed", 0) != 0;
}

json ConfigurationSchema::_restrictor(string const& category, string const& param) {
    return _attributeValue<json>(category, param, "restricted", json());
}

json ConfigurationSchema::_attributeValueJson(string const& category, string const& param,
                                              string const& attr) {
    auto const categoryItr = _schemaJson.find(category);
    if (categoryItr != _schemaJson.end()) {
        auto const paramItr = categoryItr->find(param);
        if (paramItr != categoryItr->end()) {
            auto const attrItr = paramItr->find(attr);
            if (attrItr != paramItr->end()) return *attrItr;
            throw invalid_argument("ConfigurationSchema::" + string(__func__) + " unknown attribute " + attr +
                                   " of parameter " + category + "." + param + ".");
        }
    }
    throw invalid_argument("ConfigurationSchema::" + string(__func__) + " unknown parameter " + category +
                           "." + param + ".");
}

}  // namespace lsst::qserv::replica
