# This file is part of qserv.
#
# Developed for the LSST Data Management System.
# This product includes software developed by the LSST Project
# (https://www.lsst.org).
# See the COPYRIGHT file at the top-level directory of this distribution
# for details of code ownership.
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
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.


import gzip
import json
import logging
import os
import shutil
import subprocess
import tarfile
from collections.abc import Generator
from contextlib import closing
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any, NamedTuple, cast

import backoff
import yaml

import mysql.connector
from mysql.connector.abstracts import MySQLConnectionAbstract, MySQLCursorAbstract

from .constants import tmp_data_dir
from .itest_table import LoadTable
from .mysql_connection import mysql_connection
from .qserv_backoff import max_backoff_sec, on_backoff
from .replicationInterface import ReplicationInterface
from .template import apply_template_cfg

qserv_data_dir = "/qserv/data"

chunk_info_file = "chunk_info.json"

_log = logging.getLogger(__name__)


def unzip(source: str, destination: str) -> None:
    """Unzip a source file and write it to the target location."""
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with gzip.open(source, "rb") as _source, open(destination, "wb") as _target:
        shutil.copyfileobj(_source, _target)


def execute(cursor: MySQLCursorAbstract, stmt: str, multi: bool = False) -> None:
    """Execute a statement in a cursor, and clean up the cursor so it can be
    closed without causing warnings on the server."""
    warnings = []
    results = []
    # cursors may contain results objects (if multi==True) or a MySQLCursor (if
    # multi==False)
    cursors: list[Any] = []

    if multi:
        cursors = cursor.execute(stmt, multi=True)  # type: ignore
    else:
        cursor.execute(stmt, multi=False)
        cursors = [cursor]

    for cursor in cursors:
        # Cast here because MySQLCursorAbtract does not have with_rows for some reason, even though both of
        # its subclasses do...
        cursor = cast(mysql.connector.cursor.MySQLCursor | mysql.connector.cursor_cext.CMySQLCursor, cursor)
        if cursor.with_rows:
            for row in cursor.fetchall():
                results.append(row)
            if w := cursor.fetchwarnings():
                warnings.append(w)

    if warnings:
        _log.warn('Warnings were issued when executing "%s": %s', stmt, warnings)
    if results:
        _log.info('Results were returned when executing "%s": %s', results, stmt)


@backoff.on_exception(
    exception=mysql.connector.errors.DatabaseError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def _create_ref_db(ref_db_admin: str, name: str) -> None:
    """Create a database in the mysql used for integration test reference.

    Parameters
    ----------
    ref_db_admin : `str`
        URI to the reference db for the admin user.
    name : `str`
        The name of the database to create.
    """
    statements = [
        (
            "CREATE USER IF NOT EXISTS '{{ mysqld_user_qserv }}'@'localhost' "
            "IDENTIFIED BY '{{ mysqld_user_qserv_password }}';"
        ),
        (
            "CREATE USER IF NOT EXISTS '{{ mysqld_user_qserv }}'@'%' "
            "IDENTIFIED BY '{{ mysqld_user_qserv_password }}';"
        ),
        "CREATE DATABASE IF NOT EXISTS {name};",
        "GRANT ALL ON {name}.* TO '{{ mysqld_user_qserv }}'@'localhost';",
        "GRANT ALL ON {name}.* TO '{{ mysqld_user_qserv }}'@'%';",
        "FLUSH PRIVILEGES;",
    ]
    for stmt in statements:
        with closing(mysql_connection(uri=ref_db_admin)) as cnx:
            stmt = apply_template_cfg(stmt)
            # `format` must be called after `apply_template_cfg`, because `format`
            # consumes the inner braces (changes '{{ mysql_user_qserv}}' to
            # '{mysql_user_qserv }') and so breaks the jinja templating syntax.
            stmt = stmt.format(name=name)
            _log.debug("_create_ref_db stmt:%s", stmt)
            with closing(cnx.cursor()) as cursor:
                execute(cursor, stmt)


def _create_ref_table(cnx: MySQLConnectionAbstract, db: str, schema_file: str) -> None:
    """Create a table in the mysql database used for integration test reference.

    Parameters
    ----------
    cnx : `mysql.connector.abstracts.MySQLConnectionAbstract`
        Connection to the reference database that has permission to create a
        table.
    db : `str`
        The name of the database to create the table in.
    schema_file : `str`
        Absolute path to a file that contains the schema used to create the
        table.
    """
    _log.debug("_create_ref_table in db %s schema_file:%s", db, schema_file)
    if not cnx.is_connected():
        cnx.connect()
    with closing(cnx.cursor()) as cursor:
        stmt = f"USE {db}"
        execute(cursor, stmt)
        with open(schema_file) as f:
            execute(cursor, f.read(), multi=True)


def _load_ref_data(cnx: MySQLConnectionAbstract, data_file: str, db: str, table: LoadTable) -> None:
    """Load database data into the reference database.

    Parameters
    ----------
    cnx : `mysql.connector.abstracts.MySQLConnectionAbstract`
        Connection to the reference database that has permission to load data
        into the table.
    data_file : `str`
        The path to the file that contains data to be loaded via LOAD DATA LOCAL
        INFILE.
    db : `str`
        The name of the database that the table is in.
    table : `LoadTable`
        The descriptor of the table to load data into.
    """
    sql = f"LOAD DATA LOCAL INFILE '{data_file}' INTO TABLE {table.table_name}"
    sql = sql + f" FIELDS TERMINATED BY '{table.fields_terminated_by}'"
    sql = sql + f" ENCLOSED BY '{table.fields_enclosed_by}'"
    sql = sql + f" ESCAPED BY '{table.fields_escaped_by}'"
    sql = sql + f" LINES TERMINATED BY '{table.lines_terminated_by}'"
    _log.debug("_load_ref_data sql:%s", sql)
    if not cnx.is_connected():
        cnx.connect()
    with closing(cnx.cursor()) as cursor:
        stmt = f"USE {db}"
        execute(cursor, stmt)
        execute(cursor, sql)
        _log.info(f"inserted {cursor.rowcount} rows into {db}.{table.table_name}")


class LoadDb:
    """Contains information about a database to be loaded."""

    class TablePartition(NamedTuple):
        configs_t: list[str]
        output_t: str

    def __init__(self, load_db_cfg: dict[Any, Any]):
        # The path to the root of the database files.
        self.root = load_db_cfg["root"]

        self.id = load_db_cfg["id"]

        # self.tables is the list of table names to load into the database.
        self.tables = load_db_cfg["tables"]

        # The template for the absolute path to a table's data file.
        self.datafile_t = os.path.join(self.root, load_db_cfg["data"])

        # The template for the absolute path to a table's schema file.
        self.schema_t = os.path.join(self.root, load_db_cfg["schema"])

        # self.table_partition.configs_t contains the possibly templated
        # absolute paths to the partition config files.
        # self.table_partition.output contains the templated absolute path to
        # the output folder for the partitioner.
        self.table_partition = self.TablePartition(
            [os.path.join(self.root, config) for config in load_db_cfg["partition"]["config"]],
            load_db_cfg["partition"]["output"],  # ("output" is an absolute path.)
        )

        # self.ingest_db_cfg_file is the absolute path to the database ingest
        # config file.
        self.ingest_db_cfg_file = os.path.join(self.root, load_db_cfg["ingest"]["database"])

        # self.ingest_db_metadata_file is the absolute path to the database ingest
        # metadata file, used by qserv-ingest
        self.ingest_db_metadata_file = os.path.join(self.root, load_db_cfg["ingest"]["metadata"])

        # self.ingest_db_cfg is the database ingest config dict.
        with open(self.ingest_db_cfg_file) as f:
            self.ingest_db_cfg = yaml.safe_load(f.read())

        # self.ingest_table_t is the templated absolute path to the table ingest
        # config file.
        self.ingest_table_t = os.path.join(self.root, load_db_cfg["ingest"]["table"])

        # bool, indicates if the replicaiton-ingest system should build table
        # stats or not.
        self.build_table_stats = load_db_cfg.get("build_table_stats", False)

        # str, optional, the name of the qserv instance id, used by the
        # replication-ingest system.
        self.instance_id = load_db_cfg.get("instance_id", None)

    @property
    def name(self) -> str:
        """Accessor for the name of the database."""
        return str(self.ingest_db_cfg["database"])

    def iter_tables(self) -> Generator[LoadTable, None, None]:
        """Generator to get a LoadTable instance for each table to be loaded."""
        for table_name in self.tables:
            with open(self.ingest_table_t.format(table_name=table_name)) as f:
                ingest_config = json.load(f)
            yield LoadTable(
                table_name,
                ingest_config,
                self.datafile_t.format(table_name=table_name),
                [p.format(table_name=table_name) for p in self.table_partition.configs_t],
                self.table_partition.output_t.format(table_name=table_name),
                self.schema_t.format(table_name=table_name),
            )


def _partition(staging_dir: str, table: LoadTable, data_file: str) -> None:
    """Partition data for qserv ingest using sph-partition.

    Parameters
    ----------
    staging_dir : `str`
        The absolute path to a folder that can be used to stage files
        used during processing that do not need to be kept.
    table : `LoadTable`
        The description of the table to partition.
    data_file : `str`
        The absolute path to the file that contains the table data. (It
        may have been unzipped to a location different than in `table`.)
    """
    # If config file paths are relative, make them absolute with the data file's dirname.
    partition_config_files = [
        f if os.path.isabs(f) else os.path.join(os.path.dirname(table.data_file), f)
        for f in table.partition_config_files
    ]
    os.makedirs(staging_dir)
    args = ["sph-partition-matches" if table.is_match else "sph-partition"]
    for config_file in partition_config_files:
        args.append("--config-file")
        args.append(config_file)
    args.extend(
        [
            f"--in.path={data_file}",
            "--verbose",
            "--mr.num-workers=6",
            "--mr.pool-size=32768",
            "--mr.block-size=16",
            f"--out.dir={staging_dir}",
        ]
    )
    result = subprocess.run(args, stdout=subprocess.PIPE, encoding="utf-8", errors="replace")
    result.check_returncode()
    partition_info = result.stdout
    partition_info = partition_info.replace("-nan", "null")
    with open(os.path.join(staging_dir, chunk_info_file), "w") as f:
        f.write(partition_info)


def _prep_table_data(load_table: LoadTable, dest_dir: str) -> tuple[str, str]:
    """Unzip and partition, if needed, input data for a table.

    Parameters
    ----------
    table : `LoadTable`
        Contains details about the table to which belong processed input files.
    dest_dir : `str`
        Directory where input files will be unziped and partitioned.

    Returns
    -------
    staging_dir : `str`
        The absolute path to a folder that has been used to stage files used
        during processing that do not need to be kept.
    data_file : `str`
        The absolute path to the file that contains the table data.
        (It may have been unzipped to a location different than in table.)
    """
    _log.info("_prep_table_data(1): dest_dir: %s load_table.data_file: %s", dest_dir, load_table.data_file)
    if load_table.is_gzipped:
        data_file = os.path.join(dest_dir, os.path.splitext(os.path.basename(load_table.data_file))[0])
        unzip(source=load_table.data_file, destination=data_file)
    else:
        data_file = load_table.data_file
    # Create the partitioned table data into chunks
    staging_dir = os.path.join(dest_dir, load_table.data_staging_dir)
    if load_table.is_partitioned:
        _partition(staging_dir, load_table, data_file)
    _log.info("_prep_table_data(2): staging_dir: %s data_file: %s", staging_dir, data_file)
    return staging_dir, data_file


def _load_database(
    load_db: LoadDb,
    ref_db_uri: str,
    ref_db_admin: str,
    repl_ctrl_uri: str,
    auth_key: str,
    admin_auth_key: str,
    load_http: bool,
) -> None:
    """Load a database.

    Parameters
    ----------
    load_db : `LoadDb`
        Contains details about the database to be loaded.
    ref_db_uri : `str`
        URI to the reference db for the non-admin user.
    ref_db_admin : `str`
        URI to the reference db for the admin user.
    repl_ctrl_uri : `str`
        URI to the replication controller.
    auth_key : `str`
        The authorizaiton key for the replication-ingest system.
    admin_auth_key : `str`
        The admin authorizaiton key for the replication-ingest system.
    load_http : `bool`, optional
        The protocol to use for loading the data.
    """
    _log.info(
        "Loading database %s for test %s auth_key %s admin_auth_key %s",
        load_db.name,
        load_db.id,
        auth_key,
        admin_auth_key,
    )
    repl = ReplicationInterface(repl_ctrl_uri, auth_key, admin_auth_key)

    @backoff.on_exception(
        exception=ReplicationInterface.CommandError,
        wait_gen=backoff.expo,
        on_backoff=on_backoff(log=_log),
        max_time=max_backoff_sec,
    )
    def do_ingest_database() -> None:
        repl.ingest_database(load_db.ingest_db_cfg)

    do_ingest_database()

    _create_ref_db(ref_db_admin, load_db.name)

    for table in load_db.iter_tables():
        with closing(mysql_connection(ref_db_admin, local_infile=True)) as cnx:
            _create_ref_table(cnx, load_db.name, table.ref_db_table_schema_file)
            # TODO maybe we should be cosuming more info from each case's description.yaml (like field sep)

            with TemporaryDirectory(dir=qserv_data_dir) as tmp_dir:
                staging_dir, data_file = _prep_table_data(table, tmp_dir)
                _load_ref_data(
                    cnx,
                    data_file,
                    load_db.name,
                    table,
                )

                @backoff.on_exception(
                    exception=ReplicationInterface.CommandError,
                    wait_gen=backoff.expo,
                    on_backoff=on_backoff(log=_log),
                    max_time=max_backoff_sec,
                )
                def do_ingest_table_config() -> None:
                    repl.ingest_table_config(table.ingest_config)

                do_ingest_table_config()

                transaction_id = repl.start_transaction(load_db.name)
                if table.is_partitioned:
                    repl.ingest_chunks_data(
                        transaction_id=transaction_id,
                        table=table,
                        chunks_folder=staging_dir,
                        chunk_info_file=os.path.join(staging_dir, chunk_info_file),
                        load_http=load_http,
                    )
                else:
                    repl.ingest_table_data(
                        transaction_id=transaction_id,
                        table=table,
                        data_file=data_file,
                        load_http=load_http,
                    )
                repl.commit_transaction(transaction_id)

    repl.publish_database(load_db.name)
    if load_db.build_table_stats:
        if not load_db.instance_id:
            raise RuntimeError("To build table stats, instance_id must contain a non-empty value.")
        repl.build_table_stats(load_db.name, load_db.tables, load_db.instance_id)


def _remove_database(
    case_data: dict[Any, Any],
    ref_db_connection: str,
    repl_ctrl_uri: str,
    auth_key: str,
    admin_auth_key: str,
) -> None:
    """Remove an integration test database.

    Parameters
    ----------
    case_data : Dict[Any, Any]
        Contains data about the test cases.
    ref_db_connection : str
        URI to the reference db.
    repl_ctrl_uri : str
        URI to the replication controller.
    auth_key : `str`
        The authorizaiton key for the replication-ingest system.
    admin_auth_key : `str`
        The admin authorizaiton key for the replication-ingest system.
    """
    _log.debug("_remove_database %s", case_data)
    remove_db = LoadDb(case_data)

    repl = ReplicationInterface(repl_ctrl_uri, auth_key, admin_auth_key)
    repl.delete_database(remove_db.name, admin=True)

    sql = f"DROP DATABASE IF EXISTS {remove_db.name}"
    with closing(mysql_connection(uri=ref_db_connection)) as cnx:
        with closing(cnx.cursor()) as cursor:
            execute(cursor, sql)


def _get_cases(cases: list[str] | None, test_cases_data: list[dict[Any, Any]]) -> list[dict[Any, Any]]:
    """Get the test case data for the cases listed in cases.

    Parameters
    ----------
    Same as `cases` and `test_case_data` parameters of `load`.

    Returns
    -------
    selected_cases : `list`
        The test cases from `test_cases_data` that are named in `cases`,
        or all the `test_cases_data` if `cases` does not name cases.

    Raises
    ------
    RuntimeError
        If `cases` names a case that is not in `test_cases_data`.
    """
    if cases:
        db_data = {db["id"]: db for db in test_cases_data}
        try:
            cases_data = [db_data[case] for case in cases]
        except KeyError as e:
            raise RuntimeError(f"{e.args[0]} is not in {test_cases_data}")
    else:
        cases_data = test_cases_data
    return cases_data


def prepare_data(
    test_cases_data: list[dict[Any, Any]],
) -> None:
    """Unzip and partition integration test dataset(s) inside a persistent volume
       also R-I system configuration files for input data

    Parameters
    ----------
    test_cases_data : `list` [ `dict` ]
        Dicts whose values will be used to initialize a LoadDb class instance.

    """
    _log.debug("test cases data:\n%s", test_cases_data)
    cases_data = _get_cases(None, test_cases_data)
    work_dir = os.path.join("/tmp", "datasets")
    for case_data in cases_data:
        load_db = LoadDb(case_data)
        dest_dir = os.path.join(work_dir, load_db.id)
        if os.path.exists(dest_dir):
            shutil.rmtree(dest_dir)
        Path(dest_dir).mkdir(parents=True, exist_ok=True)
        ingest_db_json = load_db.ingest_db_cfg_file
        shutil.copy(ingest_db_json, dest_dir)
        shutil.copy(load_db.ingest_db_metadata_file, dest_dir)
        for table_name in load_db.tables:
            ingest_table_json = load_db.ingest_table_t.format(table_name=table_name)
            shutil.copy(ingest_table_json, dest_dir)
        _log.info(
            "Preparing input dataset %s for test %s inside directory %s", load_db.name, load_db.id, dest_dir
        )
        for table in load_db.iter_tables():
            _prep_table_data(table, dest_dir)

    output_filename = os.path.join(tmp_data_dir, "datasets.tgz")
    _log.info("Archiving input datasets for integration tests to %s", output_filename)
    with tarfile.open(output_filename, "w:gz") as tar:
        tar.add(work_dir, arcname=os.path.basename(work_dir))


def load(
    repl_ctrl_uri: str,
    ref_db_uri: str,
    test_cases_data: list[dict[Any, Any]],
    ref_db_admin: str,
    load: bool | None,
    load_http: bool,
    cases: list[str] | None,
    auth_key: str,
    admin_auth_key: str,
    remove: bool = False,
) -> None:
    """Partition and ingest integration test data into qserv.

    Parameters
    ----------
    repl_ctrl_uri : `str`
        URI to the replication controller.
    ref_db_uri : `str`
        URI to the reference db for the non-admin user.
    test_cases_data : `list` [ `dict` ]
        Dicts whose values will be used to initialize a LoadDb class instance.
    ref_db_admin : `str`
        The connection URI for the root/admin user of the reference database.
    load : `bool` or `None`
        True if the database should be loaded, False if not. If `None`, and
        unload == True then will not load the database, otherwise if `None` will
        load the database if it is not yet loaded into qserv (assumes the ref
        database matches the qserv database.)
    load_http : `bool`
        The protocol to use for loading the data.
    cases : `list` [`str`], optional
        Restrict loading to these test cases if provided.
    auth_key : `str`
        The authorizaiton key for the replication-ingest system.
    admin_auth_key : `str`
        The admin authorizaiton key for the replication-ingest system.
    remove : bool, optional
        Instead of loading the databases, remove them. By default False

    Raises
    ------
    RuntimeError
        If the named database is not in the provided yaml.
    """
    _log.debug("test cases data:\n%s", test_cases_data)
    cases_data = _get_cases(cases, test_cases_data)
    qserv_dbs = ReplicationInterface(repl_ctrl_uri).get_databases()
    for case_data in cases_data:
        load_db = LoadDb(case_data)
        if load is True or (load is None and load_db.name not in qserv_dbs):
            _load_database(
                load_db,
                ref_db_uri,
                ref_db_admin,
                repl_ctrl_uri,
                auth_key,
                admin_auth_key,
                load_http=load_http,
            )


def remove(
    repl_ctrl_uri: str,
    ref_db_uri: str,
    test_cases_data: list[dict[Any, Any]],
    ref_db_admin: str,
    auth_key: str,
    admin_auth_key: str,
    cases: list[str] | None = None,
) -> None:
    """Remove integration test data from qserv.

    Parameters
    ----------
    Same as same-named arguments to `load`.
    """
    cases_data = _get_cases(cases, test_cases_data)
    for case_data in cases_data:
        _remove_database(case_data, ref_db_admin, repl_ctrl_uri, auth_key, admin_auth_key)
