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
from functools import total_ordering
from glob import glob
from itertools import cycle, groupby
from textwrap import dedent

import mysqlclient

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

    def __init__(self, num_workers):
        self._pool = [None]

    def map(self, fun, seq, chunk_size=None):
        return map(fun, seq)

    def close(self):
        pass


# -- Chunk to worker server assignment --------


def round_robin(servers, chunks):
    a = sorted(zip(cycle(servers), chunks))
    return [(s, [e[1] for e in g]) for s, g in groupby(a, lambda x: x[0])]


# A map from strategy names to chunk assignment functions.
#
# A chunk assignment function takes a list of servers and a list of chunk
# files and returns a list of tuples [(S, C)] where S is a server and C
# is the list of all chunk files assigned to S.
strategies = {"round-robin": round_robin}


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
                if k[0] is not None:
                    kw[k[1]] = k[0]
        else:
            for k in ((user, "user"), (passwd, "passwd"), (socket, "unix_socket"), (database, "db")):
                if k[0] is not None:
                    kw[k[1]] = k[0]

        print(f"SqlActions init : {kw!s}")

        self.conn = mysqlclient.connect(**kw)
        self.cursor = self.conn.cursor()

    def _exec(self, stmt):
        print(f"DEBUG : {stmt}")
        self.cursor.execute(stmt)
        self.cursor.fetchall()

    def create_database(self, database):
        self._exec(f"CREATE DATABASE IF NOT EXISTS {database}")

    def drop_database(self, database):
        self._exec(f"DROP DATABASE IF EXISTS {database}")

    def drop_table(self, table):
        self._exec(f"DROP TABLE IF EXISTS {table}")

    def table_exists(self, table):
        components = table.split(".")
        if len(components) != 2:
            raise RuntimeError(f"Table name {table} is not fully qualified")
        self.cursor.execute(
            "SELECT COUNT(*) FROM INFORMATION_SCHEMA.TABLES"
            f"WHERE table_schema = '{components[0]}' AND table_name = '{components[1]}'"
        )
        return self.cursor.fetchone()[0] != 0

    def drop_tables(self, database, prefix):
        self.cursor.execute(
            "SELECT table_name FROM INFORMATION_SCHEMA.TABLES"
            f"WHERE table_schema = '{database}' AND table_name LIKE '{prefix}%'"
        )
        for table in self.cursor.fetchall():
            self.cursor.execute(f"DROP TABLE IF EXISTS {database}.{table[0]}")
            self.cursor.fetchall()

    def get_schema(self, table):
        print(f"get_schema of table {table}")
        self.cursor.execute(f"SHOW CREATE TABLE {table}")
        return self.cursor.fetchone()[1]

    def load_partitions(self, table, part_file, index=True):
        print(f"Loading partition table: {table} with {os.path.abspath(part_file)}")
        self.drop_table(table)
        self._exec(
            f"""
            CREATE TABLE {table} (
                chunkId INT NOT NULL,
                subChunkId INT NOT NULL,
                numRows INT NOT NULL,
                raMin DOUBLE PRECISION NOT NULL,
                raMax DOUBLE PRECISION NOT NULL,
                declMin DOUBLE PRECISION NOT NULL,
                declMax DOUBLE PRECISION NOT NULL,
                overlap DOUBLE PRECISION NOT NULL,
                alpha DOUBLE PRECISION NOT NULL
            )"""
        )
        self._exec(
            f"""
            LOAD DATA LOCAL INFILE '{os.path.abspath(part_file)}'
            INTO TABLE {table}
            FIELDS TERMINATED BY ','"""
        )
        if index:
            self._exec(f"ALTER TABLE {table} ADD INDEX (chunkId, subChunkId)")

    def create_prototype(self, table, schema):
        self._exec(f"DROP TABLE IF EXISTS {table}")
        # Doesn't work if table name contains `
        table_re = r"^\s*CREATE\s+TABLE\s+`[^`]+`\s+"
        m = re.match(table_re, schema)
        if m is None:
            raise RuntimeError(f"Could not extract columns from:\n{schema}")
        self._exec(f"CREATE TABLE {table} {schema[m.end() :]}")

    def create_padded_table(self, table, npad):
        padded = table + "Padded"
        self._exec(f"CREATE TABLE {padded} LIKE {table}")
        for i in range(npad):
            self._exec(
                f"""
                ALTER TABLE {padded}
                ADD COLUMN (_pad_{i} FLOAT NOT NULL)"""
            )

    def get_average_row_size(self, table):
        """Return the average row size of table. Note that for this to work,
        data must have been loaded into the table, otherwise MySQL stores
        0 in the avg_row_size column of INFORMATION_SCHEMA.TABLES.
        """
        components = table.split(".")
        if len(components) != 2:
            raise RuntimeError(f"Table name {table} is not fully qualified")
        self.cursor.execute(
            f"""
            SELECT avg_row_length
            FROM INFORMATION_SCHEMA.TABLES
            WHERE table_schema = '{components[0]}' AND table_name = '{components[1]}'"""
        )
        return self.cursor.fetchone()[0]

    def load_chunk(self, table, prototype, path, npad=None, index=False, drop_primary_key=False):
        if table == prototype:
            raise RuntimeError(f"Chunk and prototype tables have identical names: {table}")
        self.drop_table(table)
        self._exec(f"CREATE TABLE {table} LIKE {prototype}")
        if drop_primary_key:
            self._exec(f"ALTER TABLE {table} DROP PRIMARY KEY")
        self._exec(
            f"LOAD DATA LOCAL INFILE '{os.path.abspath(path)}' INTO TABLE {table} FIELDS TERMINATED BY ','"
        )
        if npad is not None and npad > 0:
            tmp_table = table + "Tmp"
            self._exec(f"RENAME TABLE {table} TO {tmp_table}")
            self._exec(f"CREATE TABLE {table} LIKE {prototype + 'Padded'}")
            random_vals = ",".join(["RAND()"] * npad)
            self._exec(f"INSERT INTO {table} SELECT *, {random_vals} FROM {tmp_table}")
            self.drop_table(tmp_table)
        # Create index on subChunkId
        if index:
            self.cursor.execute(f"ALTER TABLE {table} ADD INDEX (subChunkId)")
            self.cursor.fetchall()

    def test_chunk_table(self, chunk_prefix, chunk_id, part_table):
        """Run sanity checks on a chunk table set (the chunk table,
        and optionally a self and full overlap table). For now, the
        spherical coordinates of chunk table entries are hardcoded
        to the "ra" and "decl" columns.
        """
        chunk_table = chunk_prefix + "_" + str(chunk_id)
        self_table = chunk_prefix + "SelfOverlap_" + str(chunk_id)
        full_table = chunk_prefix + "FullOverlap_" + str(chunk_id)
        if not self.table_exists(self_table):
            self_table = None
        if not self.table_exists(full_table):
            full_table = None

        # Test 1: make sure partition map data is reasonable
        self.cursor.execute(
            f"""
            SELECT COUNT(*) FROM {part_table}
            WHERE raMin < 0.0 OR raMin >= 360.0 OR
                  raMax <= 0.0 OR raMax > 360.0 OR
                  raMin >= raMax OR
                  declMin < -90.0 OR declMax >= 90.01 OR
                  declMin >= declMax OR
                  numRows < 0 OR
                  alpha < 0.0 OR alpha > 180.0"""
        )
        nfailed = self.cursor.fetchone()[0]
        if nfailed > 0:
            print(
                f"ERROR: found {nfailed} partition map entries with invalid data. Errors can include any "
                "of the following: invalid bounds (coordinate values out of range or min >= max), "
                "a negative row count, or an invalid overlap width (alpha)."
            )

        # Test 2: make sure spherical coordinates are in range
        self.cursor.execute(
            f"""
            SELECT COUNT(*) FROM {chunk_table}
            WHERE ra < 0.0 OR ra >= 360.0 OR decl < -90.0 OR decl > 90.0"""
        )
        nfailed = self.cursor.fetchone()[0]
        if nfailed > 0:
            print(
                f"ERROR: found {nfailed} records assigned to chunk {chunk_id} ({chunk_table}) "
                f"with invalid coordinates."
            )

        # Test 3: make sure all entries are inside their sub-chunks
        self.cursor.execute(
            f"""
            SELECT COUNT(*) FROM {chunk_table} AS c INNER JOIN {part_table} AS p
            ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
            WHERE c.ra < p.raMin OR c.ra >= p.raMax OR
                  c.decl < p.declMin OR c.decl >= p.declMax"""
        )
        nfailed = self.cursor.fetchone()[0]
        if nfailed > 0:
            print(
                f"ERROR: found {nfailed} records assigned to chunk {chunk_id} ({chunk_table} "
                "falling outside the bounds of their sub-chunks."
            )

        # Test 4: make sure all self-overlap entries are outside but
        # "close" to their sub-chunks
        if self_table:
            self.cursor.execute(
                f"""
                SELECT COUNT(*) FROM {self_table} AS c INNER JOIN {part_table} AS p
                ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
                WHERE c.ra >= p.raMin AND c.ra < p.raMax AND
                      c.decl >= p.declMin AND c.decl < p.declMax"""
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    f"ERROR: found {nfailed} self-overlap records assigned to chunk {chunk_id} "
                    f"({self_table}) falling inside their sub-chunks."
                )
            self.cursor.execute(
                f"""
                SELECT COUNT(*) FROM {self_table} AS c INNER JOIN {part_table} AS p
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
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    f"WARNING: found {nfailed} self-overlap records assigned to chunk {chunk_id} "
                    f"({self_table}) falling outside the bounds of their sub-chunk self-overlap regions."
                )

        # Test 5: make sure all full-overlap entries are outside but
        # "close" to their sub-chunks
        if full_table:
            self.cursor.execute(
                f"""
                SELECT COUNT(*) FROM {full_table} AS c INNER JOIN {part_table} AS p
                ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
                WHERE c.ra >= p.raMin AND c.ra < p.raMax AND
                      c.decl >= p.declMin AND c.decl < p.declMax"""
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    f"ERROR: found {nfailed} full-overlap records assigned to chunk {chunk_id} "
                    f" ({full_table}) falling inside their sub-chunks."
                )
            self.cursor.execute(
                f"""
                SELECT COUNT(*) FROM {full_table} AS c INNER JOIN {part_table} AS p
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
            )
            nfailed = self.cursor.fetchone()[0]
            if nfailed > 0:
                print(
                    f"WARNING: found {nfailed} full-overlap records assigned to chunk {chunk_id} "
                    f"({full_table}) falling outside the bounds of their sub-chunk full-overlap regions."
                )

        # Test 6: make sure the partition map sub-chunk row counts agree
        # with the loaded table
        self.cursor.execute(
            f"""
            SELECT COUNT(*)
            FROM (SELECT COUNT(*) AS numRows, chunkId, subChunkId
            FROM {chunk_table} GROUP BY chunkId, subChunkId) AS c
            INNER JOIN {part_table} AS p
            ON (c.chunkId = p.chunkId AND c.subChunkId = p.subChunkId)
            WHERE c.numRows != p.numRows"""
        )
        nfailed = self.cursor.fetchone()[0]

    def close(self):
        try:
            self.cursor.close()
        except Exception:
            pass
        self.conn.close()


# -- Utilities --------


def host_port(sv):
    hp = sv.split(":")
    if len(hp) > 1:
        return (hp[0], int(hp[1]))
    else:
        return (hp[0], None)


def chunk_id_from_path(path):
    m = re.match(r".*_(\d+).csv$", path)
    if m is None:
        raise RuntimeError(f"Unable to extract chunk id from path {path}")
    return int(m.group(1))


def table_from_path(path, opts, prefix=""):
    table = os.path.splitext(os.path.basename(path))[0]
    return opts.database + "." + prefix + table


@total_ordering
class Params:
    """Parameter holder class for a specific database server"""

    def __init__(self, sv, opts):
        self.host, self.port = host_port(sv)
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

    def __le__(self, other):
        return (self.host, self.port) <= (other.host, other.port)


def get_workers(patterns_or_paths, master):
    """Given a list of server name patterns or a files containing a list of
    such patterns (one per line), return a list of distinct worker server
    names.
    """
    if patterns_or_paths is None or len(patterns_or_paths) == 0:
        return [master.strip()]
    workers = {}
    for pp in patterns_or_paths:
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
            host_port = a[0]
            if len(a) == 1:
                workers[host_port] = 1
            for tok in a[1:]:
                m = re.match(r"^(\d+)-(\d+)$", tok)
                if m is None:
                    i = int(tok)
                    if i < 0:
                        raise RuntimeError(
                            f"Negative integer in range specification of server name pattern {pat}"
                        )
                    workers[host_port % i] = 1
                else:
                    i1 = int(m.group(1))
                    i2 = int(m.group(2))
                    for i in range(min(i1, i2), max(i1, i2) + 1):
                        workers[host_port % i] = 1
    return workers.keys()


def find_partition_file(input_dir):
    """Search inputDir for a single file name ending with 'Partitions.csv'."""
    matches = glob(os.path.join(input_dir, "*Partitions.csv"))
    if len(matches) > 1:
        raise RuntimeError(f"Multiple partition map files found:\n{'\n'.join(matches)}")
    elif len(matches) == 0 or len(matches[0]) == len("Partitions.csv"):
        raise RuntimeError(f"No partition map CSV file found in {input_dir}")
    return matches[0]


def find_chunk_files(input_dir, prefix):
    """Find all chunk files with the given prefix in inputDir
    or any sub-directory thereof. A list of lists is returned,
    where each sub-list contains files with a common chunkId.
    """
    pat = re.compile("^" + re.escape(prefix) + r"(?:(?:Self|Full)Overlap)?_(\d+).csv")
    chunks = {}
    for root, _, files in os.walk(input_dir):
        for f in files:
            if pat.match(f) is not None:
                chunk_id = chunk_id_from_path(f)
                p = os.path.join(root, f)
                if chunk_id in chunks:
                    for c in chunks[chunk_id]:
                        if os.path.basename(c) == f:
                            raise RuntimeError(f"Found 2 identically named chunk files: {p} and {c}")
                    chunks[chunk_id].append(p)
                else:
                    chunks[chunk_id] = [p]
    for i in chunks:
        chunks[i].sort(reverse=True)
    return chunks.values()


# -- Actions --------


def drop_database(params):
    act = SqlActions(params.host, params.port, params.user, params.password, params.socket, params.database)
    try:
        act.drop_database(params.database)
    finally:
        act.close()


def clean_database(params):
    act = SqlActions(params.host, params.port, params.user, params.password, params.socket, params.database)
    try:
        if params.clean is not None:
            act.drop_tables(params.database, params.clean)
    finally:
        act.close()


def master_init(master, sample_file, opts):
    """Initialize the master server. Loads the partition map, determines
    the average row size of a sample table, and retrieves the schema of the
    chunk prototype table. A (schema, npad) tuple is returned, where schema
    is the prototype table schema, and npad is the number of FLOAT columns
    to pad chunk tables by.
    """
    schema, npad = None, 0
    sample_table = table_from_path(sample_file, opts, "_sample_")
    part_table = table_from_path(opts.partFile, opts)
    if part_table == opts.prototype:
        raise RuntimeError(f"Chunk prototype and partition map tables have identical names: {part_table}")
    hp = host_port(master)

    act = SqlActions(hp[0], hp[1], opts.user, opts.password, opts.socket, opts.database)
    try:
        act.create_database(opts.database)
        # Get prototype schema
        schema = act.get_schema(opts.prototype)
        # Load partition map
        act.load_partitions(part_table, opts.partFile)
        # Load a sample chunk table and retrieve the row size
        if opts.rowSize is not None and opts.rowSize > 0:
            try:
                act.load_chunk(sample_table, opts.prototype, sample_file, 0, False)
                avg = act.get_average_row_size(sample_table)
                if avg < opts.rowSize:
                    npad = (opts.rowSize - avg) / 4
                    if opts.verbose:
                        print(f"Adding {npad} FLOAT columns for padding")
            finally:
                act.drop_table(sample_table)
    finally:
        act.close()
    return schema, npad


def load_worker(args):
    """Loads chunk files on a worker server."""
    params, chunks = args
    act = SqlActions(params.host, params.port, params.user, params.password, params.socket, params.database)
    part_table = None

    try:
        act.create_database(params.database)
        prototype = params.database + "." + params.chunkPrefix + "Prototype"
        print(f"Loading chunk: {prototype}")
        if prototype != params.prototype:
            act.create_prototype(prototype, params.schema)
        if params.npad is not None and params.npad > 0:
            act.create_padded_table(prototype, params.npad)
        if params.test:
            part_table = table_from_path(params.partFile, params, "_worker_")
            act.load_partitions(part_table, params.partFile)
        for files in chunks:
            for f in files:
                table = table_from_path(f, params)
                # Check if params.dropPrimaryKeyTable is non empty and if table contains it:
                drop_primary_key = params.dropPrimaryKeyTable and (params.dropPrimaryKeyTable in table)
                act.load_chunk(table, prototype, f, params.npad, True, drop_primary_key)
            if params.test:
                pfx = params.database + "." + params.chunkPrefix
                chunk_id = chunk_id_from_path(files[0])
                act.test_chunk_table(pfx, chunk_id, part_table)
    finally:
        if part_table:
            try:
                act.drop_table(part_table)
            except Exception:
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
    print(f"DEBUG : {len(args)}")
    if len(args) not in (1, 3):
        parser.error(
            dedent("""\
            A master server or a master server, input directory and
            prototype table must be specified.""")
        )
    master = args[0]
    workers = get_workers(opts.workers, args[0])
    if not opts.password and not opts.socket:
        print("Please enter your mysql password")
        opts.password = getpass.getpass()
    start_time = time.time()
    input_dir = None
    if len(args) == 3:
        input_dir = args[1]
        if args[2].find(".") == -1:
            opts.prototype = opts.database + "." + args[2]
        else:
            opts.prototype = args[2]
        if not os.path.isdir(input_dir):
            parser.error(f"Specified input directory {input_dir} does not exist or is not a directory.")
        opts.partFile = find_partition_file(input_dir)
        pfx = os.path.basename(opts.partFile)
        opts.chunkPrefix = pfx[: pfx.rfind("Partitions.csv")]
        chunk_files = find_chunk_files(input_dir, opts.chunkPrefix)
    else:
        opts.prototype = None
        opts.partFile = None
        opts.chunkPrefix = None
        chunk_files = []
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
        pool_type = mp.Pool
    else:
        pool_type = SerialPool
    pool = pool_type(opts.numJobs)
    try:
        opts.schema = None
        opts.npad = 0
        # Perform any requested cleanup
        master_params = Params(master, opts)
        worker_params = [Params(sv, opts) for sv in workers]
        if master in workers:
            server_params = worker_params
        else:
            server_params = [*worker_params, master_params]
        if opts.dropDatabase:
            if opts.verbose:
                print(f"Dropping database {opts.database} on master and workers")
                t = time.time()
            pool.map(drop_database, server_params)
            if opts.verbose:
                print(f"Dropped {opts.database} database(s) in {time.time() - t} sec")
        elif opts.clean:
            if opts.verbose:
                print(f"Cleaning {opts.database} tables prefixed with {opts.clean}")
                t = time.time()
            pool.map(clean_database, server_params)
            if opts.verbose:
                print(f"Cleaned {opts.database} database(s) in {time.time() - t} sec")
        # Load chunks
        if len(chunk_files) > 0:
            print(f"Init master with options: {opts}")
            schema, npad = master_init(master, chunk_files[0][0], opts)
            for params in server_params:
                params.schema, params.npad = schema, npad
            if opts.verbose:
                print(f"Loading tables {opts.database}.{opts.chunkPrefix}* on workers")
                t = time.time()
            pool.map(load_worker, strategies[opts.strategy](worker_params, chunk_files))
            if opts.verbose:
                print(f"Populated {opts.database} database(s) in {time.time() - t} sec")

    finally:
        pool.close()
    if opts.verbose:
        print("Total time: %f sec" % (time.time() - start_time))


if __name__ == "__main__":
    main()
