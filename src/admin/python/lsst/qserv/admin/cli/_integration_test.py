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

import backoff
from contextlib import closing
import logging
import mysql.connector

# MySQLInterfaceError can get thrown, we need to catch it.
# It's not exposed as a public python object but *is* used in mysql.connector unit tests.
from _mysql_connector import MySQLInterfaceError
import os
import requests
import shutil
import subprocess
from typing import List, Optional
from urllib.parse import urlparse
import yaml

from .. import itest_load
from ..qserv_backoff import max_backoff_sec, on_backoff
from ..replicationInterface import ReplicationInterface
from ..template import save_template_cfg
from .. import itest


_log = logging.getLogger(__name__)


@backoff.on_exception(
    exception=requests.exceptions.ConnectionError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def wait_for_replication_system(repl_ctrl_uri: str) -> None:
    """Wait for the replication system to be ready, indicated by successfully
    getting a version from it.

    Parameters
    ----------
    repl_ctrl_uri : str
        The uri to the replication controller.
    """
    ReplicationInterface(repl_ctrl_uri).version()


class DbServiceNotReady(RuntimeError):
    pass


@backoff.on_exception(
    exception=(DbServiceNotReady, mysql.connector.errors.DatabaseError, MySQLInterfaceError),
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def wait_for_db_service(db_uri: str, checkdb: str) -> None:
    """Wait for the database service to be ready.

    Parameters
    ----------
    db_uri : str
        The uri to the database service.
    checkdb : str
        The name of the database to check for.

    Raises
    ------
    DbServiceNotReady
        If the service is not ready. This is handled by the backoff decorator until
        a max wait time is exceeded, and then the exception will not be caught,
        and can be handled above or test execution can be aborted.
    """
    parsed = urlparse(db_uri)

    def get_databases() -> List[str]:
        """Get the names of the databases in the database service."""
        with closing(
            mysql.connector.connect(
                user=parsed.username,
                password=parsed.password,
                host=parsed.hostname,
                port=parsed.port,
            )
        ) as connection:
            with closing(connection.cursor()) as cursor:
                cursor.execute("show databases;")
                result: List[str] = []
                return [str(db) for (db,) in cursor.fetchall()]

    databases = get_databases()
    if checkdb in databases:
        return
    raise DbServiceNotReady(f"{checkdb} not in existing databases: {databases}")


def run_integration_tests(
    unload: bool,
    load: Optional[bool],
    reload: bool,
    load_http: bool,
    cases: List[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
    mysqld_user: str,
    mysqld_password: str,
) -> itest.ITestResults:
    """Top level script to run the integration tests.

    Parameters
    ----------
    unload : `bool`
        Remove test databases from the databases.
    load : `bool` or `None`
        True if the database should be loaded, False if not. If `None`, and
        unload == True then will not load the database, otherwise if `None` will
        load the database if it is not yet loaded into qserv (assumes the ref
        database matches the qserv database.)
    reload : `bool`
        Remove test databases and re-add them.
    load_http : `bool`
        The protocol to use for loading the data.
    cases : `list` [`str`]
        Run (and load/reload data if those flags are set) these test cases only.
    run_tests : `bool`
        True if the tests should be run. (False can be used to compare
        previously generated test outputs.)
    tests_yaml : `str`
        Path to the yaml file that contains the details about running the
        tests. The files will be merged, higher index files get priority.
    compare_resutls : `bool`
        If True run query output comparison, if False do not.
    mysqld_user : `str`
        The name of the qserv user.
    mysqld_password : `str`
        The password for the qserv user.

    Returns
    -------
    success : `bool`
        `True` if all query outputs were the same, otherwise `False`.
    """
    save_template_cfg(
        dict(
            mysqld_user_qserv=mysqld_user,
            mysqld_user_qserv_password=mysqld_password,
        )
    )

    with open(tests_yaml) as f:
        tests_data = yaml.safe_load(f.read())

    qserv_testdata_dir = tests_data["qserv-testdata-dir"]
    qserv_testdata_test_case_dir = tests_data["qserv-testdata-test-case-dir"]
    testdata_output = tests_data["testdata-output"]

    if not os.path.exists(qserv_testdata_dir):
        raise RuntimeError("qserv_testdata sources are not present.")

    if unload or load != False or reload or run_tests:
        wait_for_db_service(tests_data["reference-db-admin-uri"], "mysql")
        wait_for_db_service(tests_data["czar-db-admin-uri"], "qservMeta")
        wait_for_replication_system(tests_data["replication-controller-uri"])

    # If unload is True, and load is not specified (default is `None`, means
    # "load if not loaded") then change load to False; do not reload.
    if unload and load is None:
        load = False

    if unload or reload:
        itest_load.remove(
            repl_ctrl_uri=tests_data["replication-controller-uri"],
            ref_db_uri=tests_data["reference-db-uri"],
            test_cases_data=tests_data["test_cases"],
            ref_db_admin=tests_data["reference-db-admin-uri"],
            auth_key=tests_data["repl-auth-key"],
            admin_auth_key=tests_data["repl-admin-auth-key"],
            cases=cases,
        )

    if load != False or reload:
        itest_load.load(
            repl_ctrl_uri=tests_data["replication-controller-uri"],
            ref_db_uri=tests_data["reference-db-uri"],
            test_cases_data=tests_data["test_cases"],
            ref_db_admin=tests_data["reference-db-admin-uri"],
            auth_key=tests_data["repl-auth-key"],
            admin_auth_key=tests_data["repl-admin-auth-key"],
            cases=cases,
            load=load,
            load_http=load_http,
        )

    if run_tests:
        itest.run_queries(
            source=os.path.join(qserv_testdata_dir, qserv_testdata_test_case_dir),
            output=testdata_output,
            mysql=tests_data["reference-db-uri"],
            qserv=tests_data["qserv-uri"],
            run_cases=cases,
            test_cases_data=tests_data["test_cases"],
        )

    if compare_results:
        test_case_results = itest.compareQueryResults(run_cases=cases, outputs_dir=testdata_output)
    else:
        test_case_results = []

    return itest.ITestResults(
        test_case_results=test_case_results,
        ran_tests=run_tests,
        compared_results=compare_results,
    )


def run_integration_tests_http(
    unload: bool,
    load: Optional[bool],
    reload: bool,
    load_http: bool,
    cases: List[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
    mysqld_user: str,
    mysqld_password: str,
) -> itest.ITestResults:
    """Top level script to run the integration tests of the HTTP frontend.

    Parameters
    ----------
    unload : `bool`
        Remove test databases from the databases.
    load : `bool` or `None`
        True if the database should be loaded, False if not. If `None`, and
        unload == True then will not load the database, otherwise if `None` will
        load the database if it is not yet loaded into qserv (assumes the ref
        database matches the qserv database.)
    reload : `bool`
        Remove test databases and re-add them.
    load_http : `bool`
        The protocol to use for loading the data.
    cases : `list` [`str`]
        Run (and load/reload data if those flags are set) these test cases only.
    run_tests : `bool`
        True if the tests should be run. (False can be used to compare
        previously generated test outputs.)
    tests_yaml : `str`
        Path to the yaml file that contains the details about running the
        tests. The files will be merged, higher index files get priority.
    compare_resutls : `bool`
        If True run query output comparison, if False do not.
    mysqld_user : `str`
        The name of the qserv user.
    mysqld_password : `str`
        The password for the qserv user.

    Returns
    -------
    success : `bool`
        `True` if all query outputs were the same, otherwise `False`.
    """
    save_template_cfg(
        dict(
            mysqld_user_qserv=mysqld_user,
            mysqld_user_qserv_password=mysqld_password,
        )
    )

    with open(tests_yaml) as f:
        tests_data = yaml.safe_load(f.read())

    qserv_testdata_dir = tests_data["qserv-testdata-dir"]
    qserv_testdata_test_case_dir = tests_data["qserv-testdata-test-case-dir"]
    testdata_output = tests_data["testdata-output"]

    if not os.path.exists(qserv_testdata_dir):
        raise RuntimeError("qserv_testdata sources are not present.")

    if unload or load != False or reload or run_tests:
        wait_for_db_service(tests_data["reference-db-admin-uri"], "mysql")
        wait_for_db_service(tests_data["czar-db-admin-uri"], "qservMeta")
        wait_for_replication_system(tests_data["replication-controller-uri"])

    # If unload is True, and load is not specified (default is `None`, means
    # "load if not loaded") then change load to False; do not reload.
    if unload and load is None:
        load = False

    if unload or reload:
        itest_load.remove(
            repl_ctrl_uri=tests_data["replication-controller-uri"],
            ref_db_uri=tests_data["reference-db-uri"],
            test_cases_data=tests_data["test_cases"],
            ref_db_admin=tests_data["reference-db-admin-uri"],
            auth_key=tests_data["repl-auth-key"],
            admin_auth_key=tests_data["repl-admin-auth-key"],
            cases=cases,
        )

    if load != False or reload:
        itest_load.load(
            repl_ctrl_uri=tests_data["replication-controller-uri"],
            ref_db_uri=tests_data["reference-db-uri"],
            test_cases_data=tests_data["test_cases"],
            ref_db_admin=tests_data["reference-db-admin-uri"],
            auth_key=tests_data["repl-auth-key"],
            admin_auth_key=tests_data["repl-admin-auth-key"],
            cases=cases,
            load=load,
            load_http=load_http,
        )

    if run_tests:
        itest.run_queries_http(
            source=os.path.join(qserv_testdata_dir, qserv_testdata_test_case_dir),
            output=testdata_output,
            mysql=tests_data["reference-db-uri"],
            http=tests_data["qserv-http-uri"],
            run_cases=cases,
            test_cases_data=tests_data["test_cases"],
        )

    if compare_results:
        test_case_results = itest.compareQueryResults(run_cases=cases, outputs_dir=testdata_output)
    else:
        test_case_results = []

    return itest.ITestResults(
        test_case_results=test_case_results,
        ran_tests=run_tests,
        compared_results=compare_results,
    )


def run_integration_tests_http_ingest(
    run_tests: bool,
    keep_results: bool,
    tests_yaml: str,
) -> bool:
    """Top level script to run the integration tests of ingesting user tables via the HTTP frontend.

    Parameters
    ----------
    run_tests : `bool`
        True if the tests should be run. (False can be used to compare
        previously generated test outputs.)
    keep_results : `bool`
        True if the results should be kept after the tests are run.
    tests_yaml : `str`
        Path to the yaml file that contains the details about running the
        tests. The files will be merged, higher index files get priority.

    Returns
    -------
    success : `bool`
        `True` if loading succeeded and query outputs were the same as the inputs, otherwise `False`.
    """

    with open(tests_yaml) as f:
        tests_data = yaml.safe_load(f.read())

    if run_tests:
        wait_for_db_service(tests_data["czar-db-admin-uri"], "qservMeta")
        wait_for_replication_system(tests_data["replication-controller-uri"])
        return itest.run_http_ingest(
            http_frontend_uri=tests_data["qserv-http-uri"],
            keep_results=keep_results,
        )
    return True


def prepare_data(tests_yaml: str) -> bool:
    """Top level script to unzip and partition test datasets

    Parameters
    ----------
    tests_yaml : `str`
        Path to the yaml file that contains the details about running the
        tests. The files will be merged, higher index files get priority.

    Returns
    -------
    success : `bool`
        `True` if all query outputs were the same, otherwise `False`.
    """
    with open(tests_yaml) as f:
        tests_data = yaml.safe_load(f.read())

    qserv_testdata_dir = tests_data["qserv-testdata-dir"]

    if not os.path.exists(qserv_testdata_dir):
        raise RuntimeError("qserv_testdata sources are not present.")

    itest_load.prepare_data(test_cases_data=tests_data["test_cases"])

    return True
