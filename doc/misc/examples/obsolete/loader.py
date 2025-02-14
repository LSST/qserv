#! /usr/bin/env python

#
# LSST Data Management System
# Copyright 2008, 2009, 2010 LSST Corporation.
#
# This product includes software developed by the
# LSST Project (http://www.lsst.org/).
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the LSST License Statement and
# the GNU General Public License along with this program.  If not,
# see <http://www.lsstcorp.org/LegalNotices/>.
#

import getpass
import optparse
import os
import os.path
import re
import time
import warnings
from glob import glob
from itertools import cycle, groupby
from textwrap import dedent

import MySQLdb as sql

# -- Parallelization --------

try:
    # requires python 2.6.x
    import multiprocessing as mp

    _have_mp = True
except ImportError:
    # process in serial fashion
    _have_mp = False


class SerialPool:
    """Simple drop-in replacement for a subset of the multiprocessing.Pool
    class; all tasks are run in the same process as the caller."""

    def __init__(self, numWorkers):
        self._pool = [None]

    def map(self, fun, seq, chunkSize=None):
        return map(fun, seq)

    def close(self):
        pass


# -- Chunk to worker server assignment --------


def roundRobin(servers, chunks):
    a = sorted(zip(cycle(servers), chunks))
    return [(s, [e[1] for e in g]) for s, g in groupby(a, lambda x: x[0])]


# A map from strategy names to chunk assignment functions.
#
# A chunk assignment function takes a list of servers and a list of chunk
# files and returns a list of tuples [(S, C)] where S is a server and C
# is the list of all chunk files assigned to S.
strategies = {"round-robin": roundRobin}


# -- Database interaction --------


class SqlActions:
    """Higher level interface for database loading/cleanup tasks."""

    def __init__(self, host, port, user, passwd, socket=None, database="LSST"):
        kw = dict()
        if socket is None:
            kw["host"] = host
            for k in (
                (port, "port"),
                (user, "user"),
                (passwd, "passwd"),
                (socket, "unix_socket"),
                (database, "db"),
            ):
                if k[0] != None:
                    kw[k[1]] = k[0]
        else:
            for k in ((user, "user"), (passwd, "passwd"), (socket, "unix_socket"), (database, "db")):
                if k[0] != None:
                    kw[k[1]] = k[0]

        print("SqlActions init : %s" % str(kw))

        self.conn = sql.connect(**kw)
        self.cursor = self.conn.cursor()

    def _exec(self, stmt):
        print("DEBUG : %s" % stmt)
        self.cursor.execute(stmt)
        self.cursor.fetchall()

    def createDatabase(self, database):
        self._exec("CREATE DATABASE IF NOT EXISTS %s" % database)

    def dropDatabase(self, database):
        self._exec("DROP DATABASE IF EXISTS %s" % database)

    def dropTable(self, table):
        self._exec("DROP TABLE IF EXISTS %s" % table)

    def tableExists(self, table):
        components = table.split(".")
        if len(components) != 2:
            raise RuntimeError("Table name %s is not fully qualified" % table)
        self.cursor.execute(
            """
            SELECT COUNT(*)
            FROM INFORMATION_SCHEMA.TABLES
            WHERE table_schema = '%s' AND table_name = '%s'"""
            % (components[0], components[1])
        )
        return self.cursor.fetchone()[0] != 0

    def dropTables(self, database, prefix):
        self.cursor.execute(
            """
            SELECT table_name
            FROM INFORMATION_SCHEMA.TABLES
            WHERE table_schema = '%s' AND table_name LIKE '%s'"""
            % (database, prefix + "%")
        )
        for table in self.cursor.fetchall():
            self.cursor.execute("DROP TABLE IF EXISTS %s.%s" % (database, table[0]))
            self.cursor.fetchall()

    def getSchema(self, table):
        print("getSchema of table %s" % table)
        self.cursor.execute("SHOW CREATE TABLE %s" % table)
        return self.cursor.fetchone()[1]

    def loadPartitions(self, table, partFile, index=True):
        print("Loading partition table: %s with %s" % (table, os.path.abspath(partFile)))
        self.dropTable(table)
        self._exec(
            """
            CREATE TABLE %s (
                chunkId INT NOT NULL,
                subChunkId INT NOT NULL,
                numRows INT NOT NULL,
                raMin DOUBLE PRECISION NOT NULL,
                raMax DOUBLE PRECISION NOT NULL,
                declMin DOUBLE PRECISION NOT NULL,
                declMax DOUBLE PRECISION NOT NULL,
                overlap DOUBLE PRECISION NOT NULL,
                alpha DOUBLE PRECISION NOT NULL
            );"""
            % table
        )
        self._exec(
            """
            LOAD DATA LOCAL INFILE '%s'
            INTO TABLE %s
            FIELDS TERMINATED BY ','"""
            % (os.path.abspath(partFile), table)
        )
        if index:
            self._exec("ALTER TABLE %s ADD INDEX (chunkId, subChunkId)" % table)

    def createPrototype(self, table, schema):
        self._exec("DROP TABLE IF EXISTS %s" % table)
        # Doesn't work if table name contains `
        tableRe = r"^\s*CREATE\s+TABLE\s+`[^`]+`\s+"
        m = re.match(tableRe, schema)
        if m == None:
            raise RuntimeError("Could not extract columns from:\n%s" % schema)
        self._exec("CREATE TABLE %s %s" % (table, schema[m.end() :]))

    def createPaddedTable(self, table, npad):
        padded = table + "Padded"
        self._exec("CREATE TABLE %s LIKE %s" % (padded, table))
        for i in xrange(npad):
            self._exec(
                """
                ALTER TABLE %s
                ADD COLUMN (_pad_%d FLOAT NOT NULL)"""
                % (padded, i)
            )

    def getAverageRowSize(self, table):
        """Return the average row size of table. Note that for this to work,
        data must have been loaded into the table, otherwise MySQL stores
        0 in the avg_row_size column of INFORMATION_SCHEMA.TABLES.
        """
        components = table.split(".")
        if len(components) != 2:
            raise RuntimeError("Table name %s is not fully qualified" % table)
        self.cursor.execute(
            """
            SELECT avg_row_length
            FROM INFORMATION_SCHEMA.TABLES
            WHERE table_schema = '%s' AND table_name = '%s'"""
            % (components[0], components[1])
        )
        return self.cursor.fetchone()[0]

    def loadChunk(self, table, prototype, path, npad=None, index=False, dropPrimaryKey=False):
        if table == prototype:
            raise RuntimeError("Chunk and prototype tables have identical names: %s" % table)
        self.dropTable(table)
        self._exec("CREATE TABLE %s LIKE %s" % (table, prototype))
        if dropPrimaryKey:
            self._exec("ALTER TABLE %s DROP PRIMARY KEY" % (table))
        self._exec(
            "LOAD DATA LOCAL INFILE '%s' "
            "INTO TABLE %s "
            "FIELDS TERMINATED BY ','" % (os.path.abspath(path), table)
        )
        if npad != None and npad > 0:
            tmpTable = table + "Tmp"
            self._exec("RENAME TABLE %s TO %s" % (table, tmpTable))
            self._exec("CREATE TABLE %s LIKE %s" % (table, prototype + "Padded"))
            randomVals = ",".join(["RAND()"] * npad)
            self._exec("INSERT INTO %s SELECT *, %s FROM %s" % (table, randomVals, tmpTable))
            self.dropTable(tmpTable)
        # Create index on subChunkId
        if index:
            self.cursor.execute("ALTER TABLE %s ADD INDEX (subChunkId)" % table)
            self.cursor.fetchall()

    def testChunkTable(self, chunkPrefix, chunkId, partTable):
        """Run sanity checks on a chunk table set (the chunk table,
        and optionally a self and full overlap table). For now, the
        spherical coordinates of chunk table entries are hardcoded
        to the "ra" and "decl" columns.
        """
        chunkTable = chunkPrefix + "_" + str(chunkId)
        selfTable = chunkPrefix + "SelfOverlap_" + str(chunkId)
        fullTable = chunkPrefix + "FullOverlap_" + str(chunkId)
        if not self.tableExists(selfTable):
            selfTable = None
        if not self.tableExists(fullTable):
            fullTable = None

        # Test 1: make sure partition map data is reasonable
        self.cursor.execute(
            """
            SELECT COUNT(*) FROM %s
            WHERE raMin < 0.0 OR raMin >= 360.0 OR
                  raMax <= 0.0 OR raMax > 360.0 OR
                  raMin >= raMax OR
                  declMin < -90.0 OR declMax >= 90.01 OR
                  declMin >= declMax OR
                  numRows < 0 OR
                  alpha < 0.0 OR alpha > 180.0"""
            % partTable
        )
        nfailed = self.cursor.fetchone()[0]
        if nfailed > 0:
            print(
                dedent(
                    """\
                ERROR: found %d partition map entries with invalid data.
                       Errors can include any of the following: invalid
                       bounds (coordinate values out of range or min >= max),
                       a negative row count, or an invalid overlap width
                       (alpha)."""
                    % nfailed
                )
            )

        # Test 2: make sure spherical coordinates are in range
        self.cursor.execute(
            """
            SELECT COUNT(*) FROM %s
            WHERE ra < 0.0 OR ra >= 360.0 OR decl < -90.0 OR decl > 90.0"""
            % chunkTable
        )
        nfailed = self.cursor.fetchone()[0]
        if nfailed > 0:
            print(
                dedent(
                    """\
                ERROR: found %d records assigned to chunk %d (%s) with
                       invalid coordinates."""
                    % (nfailed, chunkId, chunkTable)
                )
            )

        # Test 3: make sure all entries are inside their sub-chunks
        self.cursor.execute(
            """
            SELECT COUNT(*) FROM %s AS c INNER JOIN %s AS p
            ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
            WHERE c.ra < p.raMin OR c.ra >= p.raMax OR
                  c.decl < p.declMin OR c.decl >= p.declMax"""
            % (chunkTable, partTable)
        )
        nfailed = self.cursor.fetchone()[0]
        if nfailed > 0:
            print(
                dedent(
                    """\
                ERROR: found %d records assigned to chunk %d (%s)
                       falling outside the bounds of their sub-chunks."""
                    % (nfailed, chunkId, chunkTable)
                )
            )

        # Test 4: make sure all self-overlap entries are outside but
        # "close" to their sub-chunks
        if selfTable:
            self.cursor.execute(
                """
                SELECT COUNT(*) FROM %s AS c INNER JOIN %s AS p
                ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
                WHERE c.ra >= p.raMin AND c.ra < p.raMax AND
                      c.decl >= p.declMin AND c.decl < p.declMax"""
                % (selfTable, partTable)
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    dedent(
                        """\
                    ERROR: found %d self-overlap records assigned to chunk
                           %d (%s) falling inside their sub-chunks."""
                        % (nfailed, chunkId, selfTable)
                    )
                )
            self.cursor.execute(
                """
                SELECT COUNT(*) FROM %s AS c INNER JOIN %s AS p
                ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
                WHERE NOT ((
                        c.decl >= p.declMin AND c.decl < p.declMax AND (
                            c.ra + 360.0 <  p.raMax + p.alpha AND
                            c.ra + 360.0 >= p.raMax
                        ) OR (
                            c.ra <  p.raMax + p.alpha AND
                            c.ra >= p.raMin
                        )
                    ) OR (
                        c.decl < p.declMin AND
                        c.decl >= p.declMin - p.overlap AND ((
                                c.ra + 360.0 <  p.raMax + p.alpha AND
                                c.ra + 360.0 >= p.raMin - p.alpha
                            ) OR (
                                c.ra <  p.raMax + p.alpha AND
                                c.ra >= p.raMin - p.alpha
                            ) OR (
                                c.ra - 360.0 <  p.raMax + p.alpha AND
                                c.ra - 360.0 >= p.raMin - p.alpha
                            )
                        )
                    )
                )"""
                % (selfTable, partTable)
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    dedent(
                        """\
                    WARNING: found %d self-overlap records assigned to chunk
                             %d (%s) falling outside the bounds of their
                             sub-chunk self-overlap regions."""
                        % (nfailed, chunkId, selfTable)
                    )
                )

        # Test 5: make sure all full-overlap entries are outside but
        # "close" to their sub-chunks
        if fullTable:
            self.cursor.execute(
                """
                SELECT COUNT(*) FROM %s AS c INNER JOIN %s AS p
                ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
                WHERE c.ra >= p.raMin AND c.ra < p.raMax AND
                      c.decl >= p.declMin AND c.decl < p.declMax"""
                % (fullTable, partTable)
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    dedent(
                        """\
                    ERROR: found %d full-overlap records assigned to chunk
                           %d (%s) falling inside their sub-chunks."""
                        % (nfailed, chunkId, fullTable)
                    )
                )
            self.cursor.execute(
                """
                SELECT COUNT(*) FROM %s AS c INNER JOIN %s AS p
                ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
                WHERE NOT (
                    c.decl >= p.declMin - p.overlap AND
                    c.decl < p.declMax + p.overlap AND ((
                            c.ra + 360.0 <  p.raMax + p.alpha AND
                            c.ra + 360.0 >= p.raMin - p.alpha
                        ) OR (
                            c.ra <  p.raMax + p.alpha AND
                            c.ra >= p.raMin - p.alpha
                        ) OR (
                            c.ra - 360.0 <  p.raMax + p.alpha AND
                            c.ra - 360.0 >= p.raMin - p.alpha
                        )
                    )
                )"""
                % (fullTable, partTable)
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    dedent(
                        """\
                    WARNING: found %d full-overlap records assigned to chunk
                             %d (%s) falling outside the bounds of their
                             sub-chunk full-overlap regions."""
                        % (nfailed, chunkId, fullTable)
                    )
                )

        # Test 6: make sure the partition map sub-chunk row counts agree
        # with the loaded table
        self.cursor.execute(
            """
            SELECT COUNT(*) FROM (
                SELECT COUNT(*) AS numRows, chunkId, subChunkId
                FROM %s GROUP BY chunkId, subChunkId) AS c
            INNER JOIN %s AS p
            ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
            WHERE c.numRows != p.numRows"""
            % (chunkTable, partTable)
        )
        nfailed = self.cursor.fetchone()[0]

    def close(self):
        try:
            self.cursor.close()
        except:
            pass
        self.conn.close()


# -- Utilities --------


def hostPort(sv):
    hp = sv.split(":")
    if len(hp) > 1:
        return (hp[0], int(hp[1]))
    else:
        return (hp[0], None)


def chunkIdFromPath(path):
    m = re.match(r".*_(\d+).csv$", path)
    if m == None:
        raise RuntimeError("Unable to extract chunk id from path %s" % path)
    return int(m.group(1))


def tableFromPath(path, opts, prefix=""):
    table = os.path.splitext(os.path.basename(path))[0]
    return opts.database + "." + prefix + table


class Params:
    """Parameter holder class for a specific database server"""

    def __init__(self, sv, opts):
        self.host, self.port = hostPort(sv)
        self.database = opts.database
        self.user = opts.user
        self.password = opts.password
        self.socket = opts.socket
        self.clean = opts.clean
        self.prototype = opts.prototype
        self.schema = opts.schema
        self.chunkPrefix = opts.chunkPrefix
        self.partFile = opts.partFile
        self.test = opts.test
        self.verbose = opts.verbose
        self.dropPrimaryKeyTable = opts.dropPrimaryKeyTable

    def __eq__(self, other):
        if isinstance(other, Params):
            return (self.host, self.port) == (other.host, other.port)
        return False

    def __cmp__(self, other):
        return cmp((self.host, self.port), (other.host, other.port))


def getWorkers(patternsOrPaths, master):
    """Given a list of server name patterns or a files containing a list of
    such patterns (one per line), return a list of distinct worker server
    names.
    """
    if patternsOrPaths == None or len(patternsOrPaths) == 0:
        return [master.strip()]
    workers = {}
    for pp in patternsOrPaths:
        if os.path.isfile(pp):
            f = open(pp, "rb")
            try:
                patterns = map(str.strip, f.readlines())
            finally:
                f.close()
        else:
            patterns = [pp.strip()]
        for pat in patterns:
            a = pat.split(",")
            hostPort = a[0]
            if len(a) == 1:
                workers[hostPort] = 1
            for tok in a[1:]:
                m = re.match(r"^(\d+)-(\d+)$", tok)
                if m == None:
                    i = int(tok)
                    if i < 0:
                        raise RuntimeError(
                            dedent(
                                """\
                            Negative integer in range specification of
                            server name pattern %s"""
                                % pat
                            )
                        )
                    workers[hostPort % i] = 1
                else:
                    i1 = int(m.group(1))
                    i2 = int(m.group(2))
                    for i in xrange(min(i1, i2), max(i1, i2) + 1):
                        workers[hostPort % i] = 1
    return workers.keys()


def findPartitionFile(inputDir):
    """Search inputDir for a single file name ending with 'Partitions.csv'."""
    matches = glob(os.path.join(inputDir, "*Partitions.csv"))
    if len(matches) > 1:
        raise RuntimeError("Multiple partition map files found:\n%s" % "\n".join(candidates))
    elif len(matches) == 0 or len(matches[0]) == len("Partitions.csv"):
        raise RuntimeError("No partition map CSV file found in %s" % inputDir)
    return matches[0]


def findChunkFiles(inputDir, prefix):
    """Find all chunk files with the given prefix in inputDir
    or any sub-directory thereof. A list of lists is returned,
    where each sub-list contains files with a common chunkId.
    """
    pat = re.compile("^" + re.escape(prefix) + r"(?:(?:Self|Full)Overlap)?_(\d+).csv")
    chunks = {}
    for root, dirs, files in os.walk(inputDir):
        for f in files:
            if pat.match(f) != None:
                chunkId = chunkIdFromPath(f)
                p = os.path.join(root, f)
                if chunkId in chunks:
                    for c in chunks[chunkId]:
                        if os.path.basename(c) == f:
                            raise RuntimeError(
                                dedent(
                                    """\
                                Found 2 identically named chunk files:
                                %s and %s"""
                                    % (p, c)
                                )
                            )
                    chunks[chunkId].append(p)
                else:
                    chunks[chunkId] = [p]
    for i in chunks:
        chunks[i].sort(reverse=True)
    return chunks.values()


# -- Actions --------


def dropDatabase(params):
    act = SqlActions(params.host, params.port, params.user, params.password, params.socket, params.database)
    try:
        act.dropDatabase(params.database)
    finally:
        act.close()


def cleanDatabase(params):
    act = SqlActions(params.host, params.port, params.user, params.password, params.socket, params.database)
    try:
        if params.clean != None:
            act.dropTables(params.database, params.clean)
    finally:
        act.close()


def masterInit(master, sampleFile, opts):
    """Initialize the master server. Loads the partition map, determines
    the average row size of a sample table, and retrieves the schema of the
    chunk prototype table. A (schema, npad) tuple is returned, where schema
    is the prototype table schema, and npad is the number of FLOAT columns
    to pad chunk tables by.
    """
    schema, npad = None, 0
    sampleTable = tableFromPath(sampleFile, opts, "_sample_")
    partTable = tableFromPath(opts.partFile, opts)
    if partTable == opts.prototype:
        raise RuntimeError(
            dedent(
                """\
            Chunk prototype and partition map tables have
            identical names: %s"""
                % partTable
            )
        )
    hp = hostPort(master)

    act = SqlActions(hp[0], hp[1], opts.user, opts.password, opts.socket, opts.database)
    try:
        act.createDatabase(opts.database)
        # Get prototype schema
        schema = act.getSchema(opts.prototype)
        # Load partition map
        act.loadPartitions(partTable, opts.partFile)
        # Load a sample chunk table and retrieve the row size
        if opts.rowSize != None and opts.rowSize > 0:
            try:
                act.loadChunk(sampleTable, opts.prototype, sampleFile, 0, False)
                avg = act.getAverageRowSize(sampleTable)
                if avg < opts.rowSize:
                    npad = (opts.rowSize - avg) / 4
                    if opts.verbose:
                        print("Adding %d FLOAT columns for padding" % npad)
            finally:
                act.dropTable(sampleTable)
    finally:
        act.close()
    return schema, npad


def loadWorker(args):
    """Loads chunk files on a worker server."""
    params, chunks = args
    act = SqlActions(params.host, params.port, params.user, params.password, params.socket, params.database)
    partTable = None

    try:
        act.createDatabase(params.database)
        prototype = params.database + "." + params.chunkPrefix + "Prototype"
        print("Loading chunk: %s" % prototype)
        if prototype != params.prototype:
            act.createPrototype(prototype, params.schema)
        if params.npad != None and params.npad > 0:
            act.createPaddedTable(prototype, params.npad)
        if params.test:
            partTable = tableFromPath(params.partFile, params, "_worker_")
            act.loadPartitions(partTable, params.partFile)
        for files in chunks:
            for f in files:
                table = tableFromPath(f, params)
                # Check if params.dropPrimaryKeyTable is non empty and if table contains it:
                dropPrimaryKey = params.dropPrimaryKeyTable and (params.dropPrimaryKeyTable in table)
                act.loadChunk(table, prototype, f, params.npad, True, dropPrimaryKey)
            if params.test:
                pfx = params.database + "." + params.chunkPrefix
                chunkId = chunkIdFromPath(files[0])
                act.testChunkTable(pfx, chunkId, partTable)
    finally:
        if partTable:
            try:
                act.dropTable(partTable)
            except:
                pass
        act.close()


# -- Command line interface --------


def main():
    usage = dedent("""\
    usage: %prog [options] <master> [<input_dir> <prototype>]

    Program which populates and/or cleans up the qserv master and worker
    servers with chunk tables and a partition map.

    <master>: A string of the form <host>[:<port>] specifying
    the qserv MySQL master database server.

    <input_dir>: Directory containing chunk CSV files. These can be located
    in arbitrary sub-directories.

    <prototype>: Chunk table prototype. This must be an existing table on
    the master server into which chunk files could successfully be loaded;
    its schema is used to create chunk tables elsewhere.
    """)

    parser = optparse.OptionParser(usage)
    parser.add_option(
        "-r",
        "--row-size",
        type="int",
        dest="rowSize",
        help=dedent("""\
        Desired size in bytes of database rows. If --row-size is specified
        and is greater than the natural row size of the input schema, then
        padding columns are added to the schema."""),
    )
    parser.add_option(
        "-w",
        "--workers",
        dest="workers",
        action="append",
        help=dedent("""\
        Server name pattern for worker servers; may be specified multiple
        times. If unspecified, the master server is used as the sole worker.
        The argument to --workers  may be either a file, which is expected
        to contain one <host>[:<port>][,<ranges>] pattern per line, or a
        single <host>[:<port>][,<ranges>] pattern.  <host> is a MySQL server
        host name or pattern. If <ranges> is specified, then the name must
        contain a single python integer formatting directive, e.g. %03d.
        The integers specified in <ranges> are substituted to form a list of
        server names from the pattern. <port> is a MySQL database port number,
        and defaults to 3306. <ranges> is a comma separated list of
        non-negative integers or integer ranges, where ranges are specified
        by separating two integers with a hyphen, e.g. "1-9" or "9-1". For
        example, the "lsst%02d.ncsa.uiuc.edu:5000,1-3" pattern would map to
        the following list of servers: lsst01.ncsa.uiuc.edu:5000,
        lsst02.ncsa.uiuc.edu:5000, lsst03.ncsa.uiuc.edu:5000. Duplicates in
        the resulting server list are removed."""),
    )
    parser.add_option(
        "-s",
        "--strategy",
        dest="strategy",
        default="round-robin",
        choices=["round-robin"],
        help=dedent("""\
        Chunk to worker server assignment strategy. For now, only the
        %default strategy (the default) is supported."""),
    )
    parser.add_option(
        "-u",
        "--user",
        dest="user",
        help=dedent("""\
        Database user name to use when connecting to MySQL servers."""),
    )
    parser.add_option(
        "-p",
        "--password",
        dest="password",
        help=dedent("""\
        If not specified, the user will be prompted for a database password to
        use when connecting to MySQL servers."""),
    )
    parser.add_option(
        "-d",
        "--database",
        dest="database",
        default="qserv_loader_test",
        help=dedent("""\
        Specifies the database into which tables should be loaded. The
        database is created if it does not already exist and defaults to
        %default."""),
    )
    parser.add_option("--socket", dest="socket", default=None, help="Socket to use for database connection.")
    parser.add_option(
        "-c",
        "--clean",
        dest="clean",
        help=dedent("""\
        Table name prefix identifying the chunk tables, partition map,
        and prototype tables to drop; this is done prior to loading."""),
    )
    parser.add_option(
        "--drop-primary-key-table",
        dest="dropPrimaryKeyTable",
        type="string",
        default="",
        help=dedent(""" Drop primary key."""),
    )
    parser.add_option(
        "-D",
        "--drop-database",
        dest="dropDatabase",
        action="store_true",
        help=dedent("""\
        Drops the database specified by --database. This is done prior
        to loading and takes precedence over --clean."""),
    )
    parser.add_option(
        "-t",
        "--test",
        dest="test",
        action="store_true",
        help=dedent("""\
        Runs sanity checks on chunk tables after they are loaded."""),
    )
    parser.add_option(
        "-j",
        "--num-jobs",
        type="int",
        dest="numJobs",
        help=dedent("""\
        (Python 2.6+) Number of parallel job processes to split activity
        over. Omitting this option or specifying a value less than 1 will
        launch as many workers as there are CPUs in the system. It is
        reasonable to set this to a number greater than the number of
        CPUs on the system since much of the work involves SQL queries on
        remote servers."""),
    )
    parser.add_option("-v", "--verbose", dest="verbose", action="store_true", help="Wordy progress reports.")

    (opts, args) = parser.parse_args()

    # Input validation and parsing
    print("DEBUG : %i" % len(args))
    if len(args) not in (1, 3):
        parser.error(
            dedent("""\
            A master server or a master server, input directory and
            prototype table must be specified.""")
        )
    master = args[0]
    workers = getWorkers(opts.workers, args[0])
    if not opts.password and not opts.socket:
        print("Please enter your mysql password")
        opts.password = getpass.getpass()
    startTime = time.time()
    inputDir = None
    if len(args) == 3:
        inputDir = args[1]
        if args[2].find(".") == -1:
            opts.prototype = opts.database + "." + args[2]
        else:
            opts.prototype = args[2]
        if not os.path.isdir(inputDir):
            parser.error(
                dedent(
                    """
                Specified input directory %s does not exist or is
                not a directory."""
                    % inputDir
                )
            )
        opts.partFile = findPartitionFile(inputDir)
        pfx = os.path.basename(opts.partFile)
        opts.chunkPrefix = pfx[: pfx.rfind("Partitions.csv")]
        chunkFiles = findChunkFiles(inputDir, opts.chunkPrefix)
    else:
        opts.prototype = None
        opts.partFile = None
        opts.chunkPrefix = None
        chunkFiles = []
    if opts.verbose:
        print("Master server: " + master)
        print("Worker servers:")
        for w in workers:
            print("\t" + w)
    # Filter out annoying "Unknown table" and "Unknown database" warnings
    # emitted by MySQLdb when dropping non existent tables/databases
    warnings.filterwarnings("ignore", "Unknown table.*")
    warnings.filterwarnings("ignore", "Can't create database.*database exists.*")
    warnings.filterwarnings("ignore", "Can't drop database.*database doesn't exist.*")

    # Setup parallel process pool
    if opts.numJobs <= 0:
        opts.numJobs = None
    if _have_mp and opts.numJobs != 1:
        poolType = mp.Pool
    else:
        poolType = SerialPool
    pool = poolType(opts.numJobs)
    try:
        opts.schema = None
        opts.npad = 0
        # Perform any requested cleanup
        masterParams = Params(master, opts)
        workerParams = [Params(sv, opts) for sv in workers]
        if master in workers:
            serverParams = workerParams
        else:
            serverParams = workerParams + [masterParams]
        if opts.dropDatabase:
            if opts.verbose:
                print("Dropping database %s on master and workers" % opts.database)
                t = time.time()
            pool.map(dropDatabase, serverParams)
            if opts.verbose:
                print("Dropped %s database(s) in %f sec" % (opts.database, time.time() - t))
        elif opts.clean:
            if opts.verbose:
                print("Cleaning %s tables prefixed with %s" % (opts.database, opts.clean))
                t = time.time()
            pool.map(cleanDatabase, serverParams)
            if opts.verbose:
                print("Cleaned %s database(s) in %f sec" % (opts.database, time.time() - t))
        # Load chunks
        if len(chunkFiles) > 0:
            print("Init master with options: %s" % opts)
            schema, npad = masterInit(master, chunkFiles[0][0], opts)
            for params in serverParams:
                params.schema, params.npad = schema, npad
            if opts.verbose:
                print("Loading tables %s.%s* on workers" % (opts.database, opts.chunkPrefix))
                t = time.time()
            pool.map(loadWorker, strategies[opts.strategy](workerParams, chunkFiles))
            if opts.verbose:
                print("Populated %s database(s) in %f sec" % (opts.database, time.time() - t))

    finally:
        pool.close()
    if opts.verbose:
        print("Total time: %f sec" % (time.time() - startTime))


if __name__ == "__main__":
    main()
