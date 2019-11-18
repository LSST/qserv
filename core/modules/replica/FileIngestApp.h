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
#ifndef LSST_QSERV_REPLICA_FILEINGESTAPP_H
#define LSST_QSERV_REPLICA_FILEINGESTAPP_H

// System headers
#include <list>
#include <memory>
#include <string>

// Third party headers
#include "nlohmann/json.hpp"

// Qserv headers
#include "replica/Application.h"

// This header declarations

namespace lsst {
namespace qserv {
namespace replica {

/**
 * Class FileIngestApp implements a tool which acts as a catalog data loading
 * client of the Replication system's catalog data ingest server.
 */
class FileIngestApp : public Application {

public:

    /// The pointer type for instances of the class
    typedef std::shared_ptr<FileIngestApp> Ptr;

    /**
     * Class FileIngestSpec represents a specification for a single file to
     * be ingested.
     */
    struct FileIngestSpec {
        std::string workerHost;         /// The host name or an IP address of a worker
        uint16_t    workerPort = 0;     /// The port number of the Ingest Service
        uint32_t    transactionId = 0;  /// An identifier of the super-transaction
        std::string tableName;          /// The base name of a table to be ingested
        std::string tableType;          /// The type of the table. Allowed options: 'P' or 'R'
        std::string inFileName;         /// The name of a local file to be ingested
    };

    /**
     * Read file ingest specifications from a JSON object. Here is a schema of
     * the object:
     * @code
     * [
     *   {"worker-host":<string>,
     *    "worker-port":<number>,
     *    "transaction-id":<number>,
     *    "table":<string>,
     *    "type":<string>,
     *    "path":<string>
     *   },
     *   ...
     * ]
     * @code
     *
     * Notes on the values of the parameters :
     * - "worker-host" is a DNS name or an IP address
     * - "worker-port" is a 16-bit unsigned integer number
     * - "transaction-id" is a 32-bit unsigned integer number
     * - "type" is either "R" for the regular table, or "P" for the partitioned one
     * - "path" is a path to the file to be read. The file has to be readable by the application
     *
     * @param jsonObj specifications packaged into a JSON object
     * @return a collection of specifications
     * @throws std::invalid_argument if the string can't be parsed
     */
    static std::list<FileIngestSpec> parseFileList(nlohmann::json const& jsonObj);

    /**
     * The factory method is the only way of creating objects of this class
     * because of the very base class's inheritance from 'enable_shared_from_this'.
     *
     * @param argc the number of command-line arguments
     * @param argv the vector of command-line arguments
     */
    static Ptr create(int argc, char* argv[]);

    // Default construction and copy semantics are prohibited

    FileIngestApp() = delete;
    FileIngestApp(FileIngestApp const&) = delete;
    FileIngestApp& operator=(FileIngestApp const&) = delete;

    ~FileIngestApp() override = default;

protected:
    int runImpl() final;

private:
    FileIngestApp(int argc, char* argv[]);

    /**
     * Read ingest specifications from a file supplied via the corresponding
     * command line parameter with command 'FILE-LIST'.
     * @return a list of file specifications
     */
    std::list<FileIngestSpec> _readFileList() const;

    /**
     * Ingest a single file as per the ingest specification
     * @param file a specification of the file
     * @throws invalid_argument for non existing files or incorrect file names
     */
    void _ingest(FileIngestSpec const& file) const;

    std::string _command;       /// 'FILE' or 'FILE-LIST' ingest scenarios
    std::string _fileListName;  /// The name of a file to read info for 'FILE-LIST' scenario

    FileIngestSpec _file;       /// File specification for the single file ingest ('FILE'))

    bool _verbose = false;      /// Print various stats upon a completion of the ingest
};

}}} // namespace lsst::qserv::replica

#endif /* LSST_QSERV_REPLICA_FILEINGESTAPP_H */
