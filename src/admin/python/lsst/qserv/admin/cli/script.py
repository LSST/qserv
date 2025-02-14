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


import logging
import os
import shlex
import socket
import subprocess
import sys
import time
from contextlib import closing
from functools import partial
from pathlib import Path
from collections.abc import Callable, Sequence

import backoff
from sqlalchemy.engine.url import URL, make_url

import mysql.connector

from ...schema import MigMgrArgs, SchemaUpdateRequired, smig, smig_block
from ..itest import ITestResults
from ..itest_table import LoadTable
from ..qserv_backoff import on_backoff
from ..replicationInterface import ReplicationInterface
from ..template import apply_template_cfg_file, save_template_cfg
from . import _integration_test, options
from .utils import Targs

smig_dir_env_var = "QSERV_SMIG_DIRECTORY"
default_smig_dir = "/usr/local/qserv/smig"


def smig_dir() -> str:
    return os.environ.get(smig_dir_env_var, default_smig_dir)


admin_smig_dir = "admin/schema"
css_smig_dir = "css/schema"
rproc_smig_dir = "rproc/schema"
qmeta_smig_dir = "qmeta/schema"
worker_smig_dir = "worker/schema"
replication_controller_smig_dir = "replica/schema"

mysqld_user_qserv = "qsmaster"
mysqld_user_qserv_password = "CHANGEME2"

ld_preload = "libjemalloc.so.2"

_log = logging.getLogger(__name__)


def _jitter(f: float) -> float:
    return 0.0


@backoff.on_exception(
    exception=SchemaUpdateRequired,
    on_backoff=on_backoff(log=_log),
    wait_gen=backoff.constant,
    interval=10,  # Wait 10 seconds between retries.
    jitter=_jitter,  # Don't add jitter (random small changes up or down) to the wait time.
    giveup=lambda e: bool(os.environ.get("UNIT_TEST", False)),
)
def _wait_for_update(smig_func: Callable[[], None]) -> None:
    """Wrapper for a smig function that includes a backoff for the case where
    the module needs to be updated but does not get updated by this process,
    allowing the process to wait here for the module to be updated.
    """
    smig_func()


def _do_smig(
    module_smig_dir: str,
    module: str,
    connection: str,
    update: bool,
    *,
    mig_mgr_args: MigMgrArgs = None,
) -> None:
    """Run schema migration on a module's database.

    Parameters
    ----------
    module_smig_dir : str
        The path to the module's schema migration files, inside the `smig_dir`.
    module : str
        The name of the module whose schema is being migrated.
    connection : str
        The uri to the database that will be affected.
    update : bool
        If the database is already initialized, do run any available updates.
    mig_mgr_args : MigMgrArgs
        Arguments to the __init__ function of the `SchemaMigMgr` subclass, by
        default None
    """
    smig_func = partial(
        smig,
        do_migrate=True,
        check=False,
        final=None,
        scripts=os.path.join(smig_dir(), module_smig_dir),
        connection=connection,
        module=module,
        mig_mgr_args=mig_mgr_args or dict(),
        update=update,
    )
    if update:
        smig_func()
    else:
        _wait_for_update(smig_func)


def _do_smig_block(module_smig_dir: str, module: str, connection: str) -> None:
    """Wait for a module's schema to be updated to the latest version.

    Parameters
    ----------
    module_smig_dir : str
        The path to the module's schema migration files, inside the `smig_dir`.
    module : str
        The name of the module whose schema is being migrated.
    connection : str
        The uri to the database that will be affected.
    """
    smig_block(
        scripts=os.path.join(smig_dir(), module_smig_dir),
        connection=connection,
        module=module,
    )


class InvalidQueryParameter(RuntimeError):
    """Raised when a URI contains query keys that are not supported for that
    URI.
    """

    pass


def _process_uri(uri: str, query_keys: Sequence[str], option: str, block: bool) -> URL:
    """Convert a string URI to a sqlalchemy URL. Verify query keys are valid.
    If indicated by block==True and a hostname and port are provided, wait until
    the database at `uri` is processing connection requests (even if they are
    rejected, if the socket is open the database is running).

    Parameters
    ----------
    uri : str
        The uri string to process.
    query_keys : Sequence[str]
        The keys that are allowed to be in the query.
    option : str
        The option name that is associated with the URI.
    block : bool
        If true and the a hostname and port are in the uri, then block until the
        the server is processing TCP connections.

    Raises
    ------
    InvalidQueryParameter
        Raised if there are values in `keys` that are not in `query_keys`

    Returns
    -------
    url : sqlalchemy.engine.url.URL
        The `URL` object derived from the parsed `uri`.
    """

    @backoff.on_exception(
        exception=mysql.connector.errors.DatabaseError,
        wait_gen=backoff.expo,
        on_backoff=on_backoff(log=_log),
    )
    def wait_for_db(url: URL) -> None:
        try:
            with closing(
                mysql.connector.connect(
                    user=url.username or "",
                    password=url.password or "",
                    host=url.host or "",
                    port=url.port or "",
                )
            ):
                pass
        except mysql.connector.errors.ProgrammingError:
            # ProgrammingError is raised if we don't have permission to connect (yet...).
            # This is ok; the db is active & reachable and that's all we're waiting for here.
            pass

    url = make_url(uri)
    if any(remainders := set(url.query.keys()) - set(query_keys)):
        raise InvalidQueryParameter(
            f"Invalid query key(s) ({remainders}); {option} accepts {query_keys or 'no keys'}."
        )
    if block and url.host and url.port:
        wait_for_db(url)
    return url


def smig_czar(connection: str, update: bool) -> None:
    """Apply schema migration scripts to czar modules.

    Parameters
    ----------
    connection : `str`
        Connection string in format mysql://user:pass@host:port/database
    update : bool
        False if workers may only be smigged from an `Uninitialized` state, or
        True if they maybe upgraded from a (previously initialized) version.
    """
    for module_smig_dir, module in (
        (admin_smig_dir, "admin"),
        (css_smig_dir, "css"),
        (rproc_smig_dir, "rproc"),
        (qmeta_smig_dir, "qmeta"),
    ):
        _do_smig(module_smig_dir, module, connection, update)


def smig_replication_controller(
    db_uri: str | None,
    db_admin_uri: str,
    update: bool,
) -> None:
    """Apply schema migration scripts to the replication controller.

    Parameters
    ----------
    db_uri : `str`, optional
        The connection string for the replication manager database for the
        non-admin user. Required when initializing the database, not needed
        when upgrading the database.
    db_admin_uri : `str`
        Connection string in format mysql://user:pass@host:port/database
    update : bool
        False if workers may only be smigged from an `Uninitialized` state, or
        True if they maybe upgraded from a (previously initialized) version.
    """
    _do_smig(
        replication_controller_smig_dir,
        "replica",
        db_admin_uri,
        update,
        mig_mgr_args=dict(
            repl_connection=db_uri,
        ),
    )


def smig_worker(connection: str, update: bool) -> None:
    """Apply schema migration scripts to the worker modules.

    Parameters
    ----------
    connection : `str`
        Connection string in format mysql://user:pass@host:port/database
    update : bool, optional
        False if workers may only be smigged from an `Uninitialized` state, or
        True if they maybe upgraded from a (previously initialized) version, by
        default False.
    """
    _do_smig(admin_smig_dir, "admin", connection, update)
    _do_smig(worker_smig_dir, "worker", connection, update)


def enter_manager_cmsd(
    targs: Targs,
    cmsd_manager_cfg_file: str,
    cmsd_manager_cfg_path: str,
    cmd: str,
) -> None:
    """Start a cmsd manager qserv node.

    Parameters
    ----------
    targs : `Targs`
        The arguments for template expansion.
    cmsd_manager_cfg_file : str
        Path to the cmsd manager config file.
    cmsd_manager_cfg_path : str
        Location to render cmsd_manager_cfg_template.
    cmd : str
        The jinja2 template for the command for this function to execute.
    """
    apply_template_cfg_file(cmsd_manager_cfg_file, cmsd_manager_cfg_path, targs)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
    )

    sys.exit(_run(args=None, env=env, cmd=cmd))


def enter_xrootd_manager(
    targs: Targs,
    xrootd_manager_cfg_file: str,
    xrootd_manager_cfg_path: str,
    cmd: str,
) -> None:
    """Start an xrootd manager qserv node.

    Parameters
    ----------
    targs : Targs
        The arguments for template expansion.
    xrootd_manager_cfg_file : str
        Path to the cmsd manager config file.
    xrootd_manager_cfg_path : str
        Location to render cmsd_manager_cfg_template.
    cmd : str
        The jinja2 template for the command for this function to execute.
    """
    apply_template_cfg_file(xrootd_manager_cfg_file, xrootd_manager_cfg_path, targs)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
    )

    sys.exit(_run(args=None, env=env, cmd=cmd))


def enter_worker_cmsd(
    targs: Targs,
    db_uri: str,
    cmsd_worker_cfg_file: str,
    cmsd_worker_cfg_path: str,
    xrdssi_cfg_file: str,
    xrdssi_cfg_path: str,
    log_cfg_file: str,
    cmd: str,
) -> None:
    """Start a worker cmsd node.

    Parameters
    ----------
    vnid_config : str
        The config parameters used by the qserv cmsd to get the vnid
        from the specified source (static string, a file or worker database).
    targs : Targs
        The arguments for template expansion.
    db_uri : str
        The non-admin URI to the worker's database.
    cmsd_worker_cfg_file : str
        The path to the worker cmsd config file.
    cmsd_worker_cfg_path : str
        The location to render the worker cmsd config file.
    xrdssi_cfg_file : str
        The path to the xrdssi config file.
    xrdssi_cfg_path : str
        The location to render the the xrdssi config file.
    log_cfg_file : `str`
        Location of the log4cxx config file.
    cmd : str
        The jinja2 template for the command for this function to execute.
    """
    url = _process_uri(
        uri=db_uri,
        query_keys=("socket",),
        option=options.option_db_uri.args[0],
        block=True,
    )
    targs["db_host"] = url.host
    targs["db_port"] = url.port or ""
    targs["db_socket"] = url.query.get("socket", "")

    apply_template_cfg_file(cmsd_worker_cfg_file, cmsd_worker_cfg_path, targs)
    apply_template_cfg_file(xrdssi_cfg_file, xrdssi_cfg_path, targs)

    _do_smig_block(admin_smig_dir, "admin", db_uri)
    # wait before worker database will be fully initialized as needed
    # for the vnid plugin to function correctly
    _do_smig_block(worker_smig_dir, "worker", db_uri)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
    )

    sys.exit(_run(args=None, env=env, cmd=cmd))


def enter_worker_xrootd(
    targs: Targs,
    db_uri: str,
    db_admin_uri: str,
    cmsd_worker_cfg_file: str,
    cmsd_worker_cfg_path: str,
    xrdssi_cfg_file: str,
    xrdssi_cfg_path: str,
    log_cfg_file: str,
    cmd: str,
) -> None:
    """Start a worker xrootd node.

    Parameters
    ----------
    targs : Targs
        The arguments for template expansion.
    db_uri : str
        The non-admin URI to the proxy's database.
    db_admin_uri : str
        The admin URI to the proxy's database.
    cmsd_worker_cfg_file : str
        The path to the worker cmsd config file.
    cmsd_worker_cfg_path : str
        The location to render to the worker cmsd config file.
    xrdssi_cfg_file : str
        The path to the xrdssi config file.
    xrdssi_cfg_path : str
        The location to render to the xrdssi config file.
    log_cfg_file : `str`
        Location of the log4cxx config file.
    cmd : `str`
        The jinja2 template for the command for this function to execute.
    """

    # TODO This sets the amount of data that can be locked into memory to
    # almost the entire amount of memory on the machine. I think in a dev-env
    # (docker-only on a single machine) we don't want to grab quite so much
    # memory? TBD WTD here - do/don't set this? set it to how much? needs a way
    # to set env (dev/prod/etc)
    # # Increase limit for locked-in-memory size
    # MLOCK_AMOUNT=$(grep MemTotal /proc/meminfo | awk '{printf("%.0f\n", $2 - 1000000)}')
    # ulimit -l "$MLOCK_AMOUNT"

    if not os.path.exists(targs["results_dirname"]):
        os.makedirs(targs["results_dirname"])

    url = _process_uri(
        uri=db_uri,
        query_keys=("socket",),
        option=options.option_db_uri.args[0],
        block=True,
    )
    _ = _process_uri(
        uri=db_admin_uri,
        query_keys=("socket",),
        option=options.option_db_admin_uri.args[0],
        block=False,
    )

    targs["db_host"] = url.host
    targs["db_port"] = url.port or ""
    targs["db_socket"] = url.query.get("socket", "")

    save_template_cfg(targs)
    save_template_cfg({"mysqld_user_qserv": mysqld_user_qserv})
    save_template_cfg({"mysqld_user_qserv_password": mysqld_user_qserv_password})

    smig_worker(db_admin_uri, update=False)

    # TODO worker (and manager) xrootd+cmsd pair should "share" the cfg file
    # it's in different containers but should be same source & processing.
    # Rename these files to be more agnostic.
    apply_template_cfg_file(cmsd_worker_cfg_file, cmsd_worker_cfg_path)
    apply_template_cfg_file(xrdssi_cfg_file, xrdssi_cfg_path)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
    )

    sys.exit(_run(args=None, env=env, cmd=cmd))


def enter_worker_repl(
    db_admin_uri: str,
    repl_connection: str,
    log_cfg_file: str,
    cmd: str,
    run: bool,
) -> None:
    """Start a worker replication node.

    Parameters
    ----------
    replic_worker_args : `list` [ `str` ]
        A list of options and arguments that will be passed directly to the
        replica worker app.
    db_admin_uri : str
        The admin URI to the worker's database.
    repl_connection : `str`
        The connection string for the replication manager database for the
        non-admin user (created using the `connection`), the user is typically
        "qsreplica".
    log_cfg_file : `str`
        Location of the log4cxx config file.
    cmd : `str`
        The jinja2 template for the command for this function to execute.
    run : `bool`
        Run the subcommand that is executed by entrypoint if `True`. Otherwise,
        print the command and arguments that would have been run.
    instance_id : `str`
        A unique identifier of a Qserv instance served by the Replication
        System. Its value will be passed along various internal communication
        lines of the system to ensure that all services are related to the same
        instance. This mechanism also prevents 'cross-talks' between two (or
        many) Replication System's setups in case of an accidental
        mis-configuration.
    """
    _ = _process_uri(
        uri=db_admin_uri,
        query_keys=(),
        option=options.option_db_admin_uri.args[0],
        block=True,
    )
    _ = _process_uri(
        uri=repl_connection,
        query_keys=(),
        option=options.option_db_admin_uri.args[0],
        block=True,
    )

    # N.B. When the controller smigs the replication database, if it is migrating from Uninitialized
    # it will also set initial configuration values in the replication database. It sets the schema
    # version of the replica database *after* setting the config values, which allows us to wait here
    # on the schema version to be sure that there are values in the database.
    _do_smig_block(replication_controller_smig_dir, "replica", repl_connection)

    ingest_folder = "/qserv/data/ingest"
    if not os.path.exists(ingest_folder):
        os.makedirs(ingest_folder)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
    )

    while True:
        # This loop exists because it is possible for qserv-replica-worker to
        # register itself with the replica controller before the call to
        # qserv-replica-config has finished processing the config. The
        # controller then rejects the replica worker, and the replica worker is
        # not resilient to this condition (does wait and try again to register
        # itself). Do not bother to check the return code because if
        # qserv-replica-worker returned then by definition it failed, and we
        # just wait a moment and restart it.
        # This is recorded in DM-31252
        _run(args=None, cmd=cmd, env=env, run=run)
        _log.info("qserv-replica-worker exited. waiting 5 seconds and restarting.")
        time.sleep(5)


def enter_proxy(
    targs: Targs,
    db_uri: str,
    db_admin_uri: str,
    proxy_backend_address: str,
    proxy_cfg_file: str,
    proxy_cfg_path: str,
    czar_cfg_file: str,
    czar_cfg_path: str,
    log_cfg_file: str,
    cmd: str,
) -> None:
    """Entrypoint script for the proxy container.

    Parameters
    ----------
    targs : Targs
        The arguments for template expansion.
    db_uri : str
        The non-admin URI to the proxy's database.
    db_admin_uri : str
        The admin URI to the proxy's database.
    proxy_backend_address : `str`
        A colon-separated ip address and port number (e.g. "127.0.0.1:3306")
        substituted into my-proxy.cnf.jinja, used by mysql proxy.
    proxy_cfg_file : `str`
        Path to the mysql proxy config file.
    proxy_cfg_path : `str`
        Location to render the mysql proxy config file.
    czar_cfg_file : `str`
        Path to the czar config file.
    czar_cfg_path : `str`
        Location to render the czar config file.
    log_cfg_file : `str`
        Location of the log4cxx config file.
    cmd : `str`
        The jinja2 template for the command for this function to execute.
    """
    url = _process_uri(
        uri=db_uri,
        query_keys=("socket",),
        option=options.option_db_uri.args[0],
        block=True,
    )
    _ = _process_uri(
        uri=db_admin_uri,
        query_keys=("socket",),
        option=options.option_db_admin_uri.args[0],
        block=True,
    )

    _log.info("Using log4cxx config file at %s", log_cfg_file)

    save_template_cfg(targs)
    save_template_cfg(
        {
            "proxy_backend_address": proxy_backend_address,
            "mysqld_user_qserv": url.username,
            "mysqld_user_qserv_password": url.password,
            "czar_db_host": url.host or "",
            "czar_db_port": url.port or "",
            "czar_db_socket": url.query.get("socket", ""),
        }
    )

    # uses vars: proxy_backend_address
    apply_template_cfg_file(proxy_cfg_file, proxy_cfg_path)
    # uses vars: czar_db_host, czar_db_port, czar_db_socket,
    apply_template_cfg_file(czar_cfg_file, czar_cfg_path)

    # czar smigs these modules, that have templated values:
    #  admin: mysqld_user_qserv, replication_controller_FQDN, mysql_monitor_password
    #  css: (no templated values)
    #  rproc: (no templated values)
    #  qmeta: mysqld_user_qserv
    smig_czar(db_admin_uri, update=False)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
        QSERV_CONFIG=czar_cfg_path,
    )

    sys.exit(_run(args=None, cmd=cmd, env=env))


def enter_czar_http(
    targs: Targs,
    db_uri: str,
    czar_cfg_file: str,
    czar_cfg_path: str,
    log_cfg_file: str,
    http_ssl_cert_file: str,
    http_ssl_private_key_file: str,
    cmd: str,
) -> None:
    """Entrypoint script for the proxy container.

    Parameters
    ----------
    targs : Targs
        The arguments for template expansion.
    db_uri : str
        The non-admin URI to the Czar's database.
    czar_cfg_file : `str`
        Path to the czar config file.
    czar_cfg_path : `str`
        Location to render the czar config file.
    log_cfg_file : `str`
        Location of the log4cxx config file.
    http_ssl_cert_file : `str`
        The path to the SSL certificate file.
    http_ssl_private_key_file : `str`
        The path to the SSL private key file.
    cmd : `str`
        The jinja2 template for the command for this function to execute.
    """
    url = _process_uri(
        uri=db_uri,
        query_keys=("socket",),
        option=options.option_db_uri.args[0],
        block=True,
    )

    _log.info("Using log4cxx config file at %s", log_cfg_file)

    save_template_cfg(targs)
    save_template_cfg(
        {
            "mysqld_user_qserv": url.username,
            "mysqld_user_qserv_password": url.password,
            "czar_db_host": url.host or "",
            "czar_db_port": url.port or "",
            "czar_db_socket": url.query.get("socket", ""),
        }
    )

    # uses vars: czar_db_host, czar_db_port, czar_db_socket,
    apply_template_cfg_file(czar_cfg_file, czar_cfg_path)

    _do_smig_block(qmeta_smig_dir, "qmeta", db_uri)

    # check if the SSL certificate and private key files exist and create
    # them if they don't.
    if not os.path.exists(http_ssl_cert_file) or not os.path.exists(http_ssl_private_key_file):
        _log.info(
            "Generating self-signed SSL/TLS certificate %s and private key %s for HTTPS",
            http_ssl_cert_file,
            http_ssl_private_key_file,
        )
        country = "US"
        state = "California"
        loc = "Menlo Park"
        org = "SLAC National Accelerator Laboratory"
        org_unit = "Rubin Observatory"
        hostname = socket.gethostbyaddr(socket.gethostname())[0]  # FQDN if available
        subj = f"/C={country}/ST={state}/L={loc}/O={org}/OU={org_unit}/CN={hostname}"
        openssl_cmd = [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:4096",
            "-out",
            http_ssl_cert_file,
            "-keyout",
            http_ssl_private_key_file,
            "-sha256",
            "-days",
            "365",
            "-nodes",
            "-subj",
            subj,
        ]
        ret = subprocess.run(
            openssl_cmd,
            env=dict(
                os.environ,
            ),
            cwd="/home/qserv",
        )
        if ret.returncode != 0:
            raise RuntimeError("Failed to create SSL certificate and private key files.")

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
        QSERV_CONFIG=czar_cfg_path,
    )

    sys.exit(_run(args=None, cmd=cmd, env=env))


def enter_replication_controller(
    db_uri: str,
    db_admin_uri: str,
    log_cfg_file: str,
    cmd: str,
    run: bool,
) -> None:
    """Entrypoint script for the entrypoint controller.

    Parameters
    ----------
    db_uri : `str`
        The connection string for the replication manager database for the
        non-admin user (created using the `connection`), the user is typically
        "qsreplica".
    db_admin_uri : `str`
        The connection string for the replication manager database for the
        administrative (typically root) user.
    log_cfg_file : `str`
        The path to the log4cxx config file.
    cmd : `str`
        The jinja2 template for the command for this function to execute.
    run : `bool`
        Run the subcommand that is executed by entrypoint if `True`. Otherwise,
        print the command and arguments that would have been run.
    """

    _ = _process_uri(
        uri=db_uri,
        query_keys=(),
        option=options.option_db_uri.args[0],
        block=True,
    )
    _ = _process_uri(
        uri=db_admin_uri,
        query_keys=("socket",),
        option=options.option_db_admin_uri.args[0],
        block=True,
    )
    if run:
        smig_replication_controller(
            db_admin_uri=db_admin_uri,
            db_uri=db_uri,
            update=False,
        )

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
    )

    sys.exit(_run(args=None, cmd=cmd, env=env, run=run))


def enter_replication_registry(
    db_uri: str,
    db_admin_uri: str,
    log_cfg_file: str,
    cmd: str,
    run: bool,
) -> None:
    """Entrypoint script for the replication worker registry.

    Parameters
    ----------
    db_uri : `str`
        The connection string for the replication manager database for the
        non-admin user (created using the `connection`), the user is typically
        "qsreplica".
    db_admin_uri : `str`
        The connection string for the replication manager database for the
        administrative (typically root) user.
    log_cfg_file : `str`
        The path to the log4cxx config file.
    cmd : `str`
        The jinja2 template for the command for this function to execute.
    run : `bool`
        Run the subcommand that is executed by entrypoint if `True`. Otherwise,
        print the command and arguments that would have been run.
    """

    _ = _process_uri(
        uri=db_uri,
        query_keys=(),
        option=options.option_db_uri.args[0],
        block=True,
    )
    _ = _process_uri(
        uri=db_admin_uri,
        query_keys=("socket",),
        option=options.option_db_admin_uri.args[0],
        block=True,
    )

    # N.B. When the replication controller smigs the replication database, if it is migrating from Uninitialized
    # it will also set initial configuration values in the replication database. It sets the schema
    # version of the replica database *after* setting the config values, which allows us to wait here
    # on the schema version to be sure that there are values in the database.
    _do_smig_block(replication_controller_smig_dir, "replica", db_uri)

    env = dict(
        os.environ,
        LD_PRELOAD=ld_preload,
        LSST_LOG_CONFIG=log_cfg_file,
    )

    sys.exit(_run(args=None, cmd=cmd, env=env, run=run))


def smig_update(czar_connection: str, worker_connections: list[str], repl_connection: str) -> None:
    """Update smig on nodes that need it.

    All connection strings are in format mysql://user:pass@host:port/database

    Parameters
    ----------
    czar_connection : `str`
        Connection string to the czar database.
    worker_connections : `list` [ `str` ]
        Connection strings to the worker databases.
    repl_connection : `str`
        Connection string replication controller database.
    """
    if czar_connection:
        smig_czar(connection=czar_connection, update=True)
    if worker_connections:
        for c in worker_connections:
            smig_worker(connection=c, update=True)
    if repl_connection:
        smig_replication_controller(db_admin_uri=repl_connection, db_uri=None, update=True)


def _run(
    args: Sequence[str | int] | None,
    cmd: str | None = None,
    env: dict[str, str] | None = None,
    debug_port: int | None = None,
    run: bool = True,
    check_returncode: bool = False,
) -> int:
    """Run a command in a subprocess.

    Parameters
    ----------
    args : List[Union[str, int]]
        The command and arguments to the command. Mutually exclusive with `cmd`.
    cmd : str, optional
        The command and arguments to run, in the form of a string.
    env : Dict[str, str], optional
        The environment variables to run the command with, by default None which
        uses the same environment as the current shell.
    debug_port : Optional[int], optional
        If provided, runs the command in gdbserver using the given port number.
        If not provided runs the command normally, by default None
    run : bool, optional
        If False, instead of running the command, the command that would have
        been run is printed and 0 is returned, as though the command had run
        successfully. If True, runs the command normally, by default True
    check_returncode : bool
        If true, will call `check_returncode` on the result of `subprocess.run`.
        This is useful when a script command runs multiple subprocesses during
        its execution and the subprocess is expected to return correctly.
        When a script command runs exactly one subprocess and exits when that
        command exits it is usually sufficient to say `return
        sys.exit(_run(...))` and let the caller handle the exit code.

    Returns
    -------
    exit_code : `int`
        The exit code of the command that was run.
    """
    if args and cmd:
        raise RuntimeError("Invalid use of `args` and `cmd`.")
    if cmd is not None and args is not None:
        raise RuntimeError("If `cmd` is not `None`, `args` must not be `None`.")
    if args:
        str_args = [str(a) for a in args]
        if debug_port:
            str_args = ["gdbserver", f"localhost:{debug_port}"] + str_args
        if not run:
            print(" ".join(str_args))
            return 0
        result = subprocess.run(str_args, env=env, cwd="/home/qserv")
    if cmd:
        args = shlex.split(cmd)
        result = subprocess.run(args, env=env, cwd="/home/qserv")
    if check_returncode:
        result.check_returncode
    return result.returncode


def delete_database(
    repl_ctrl_uri: str,
    database: str,
    admin: bool,
    auth_key: str,
    admin_auth_key: str,
) -> None:
    """Remove a database from qserv.

    Parameters
    ----------
    repl_ctrl_uri : `str`
        The uri to the replication controller service.
    database : `str`
        The name of the database to delete.
    admin : `bool`
        True if the admin auth key should be used.
    auth_key : `str`
        The authorizaiton key for the replication-ingest system.
    admin_auth_key : `str`
        The admin authorizaiton key for the replication-ingest system.
    """
    repl = ReplicationInterface(repl_ctrl_uri, auth_key, admin_auth_key)
    repl.delete_database(database, admin)


def load_simple(repl_ctrl_uri: str, auth_key: str, load_http: bool) -> None:
    """Load a simple predefined database into qserv.

    The database is called "test101" and have a table called Object with one row.

    Parameters
    ----------
    repl_ctrl_uri : `str`
        The uri to the replication controller service.
    auth_key : `str`
        The authorizaiton key for the replication-ingest system.
    load_http : `bool`
        If true, the database will be loaded using the http interface.
    """
    repl = ReplicationInterface(repl_ctrl_uri, auth_key)

    database = "test101"

    repl.ingest_database(
        dict(
            database=database,
            num_stripes=340,
            num_sub_stripes=3,
            overlap=0.01667,
            auto_build_secondary_index=1,
            local_load_secondary_index=1,
        ),
    )
    table_name = "Object"
    ingest_config = dict(
        database=database,
        table=table_name,
        is_partitioned=1,
        chunk_id_key="chunkId",
        sub_chunk_id_key="subChunkId",
        is_director=1,
        director_key="objectId",
        latitude_key="dec",
        longitude_key="ra",
        schema=[
            {"name": "objectId", "type": "BIGINT NOT NULL"},
            {"name": "ra", "type": "DOUBLE NOT NULL"},
            {"name": "dec", "type": "DOUBLE NOT NULL"},
            {"name": "property", "type": "DOUBLE"},
            {"name": "chunkId", "type": "INT UNSIGNED NOT NULL"},
            {"name": "subChunkId", "type": "INT UNSIGNED NOT NULL"},
        ],
    )
    data_file = os.path.join(Path(__file__).parent.absolute(), "chunk_0.txt")
    partition_config_files = list[str]()
    data_staging_dir = ""
    ref_db_table_schema_file = ""
    table = LoadTable(
        table_name,
        ingest_config,
        data_file,
        partition_config_files,
        data_staging_dir,
        ref_db_table_schema_file,
    )
    repl.ingest_table_config(table.ingest_config)
    transaction_id = repl.start_transaction(database=database)
    chunk_location = repl.ingest_chunk_config(transaction_id, "0")
    repl.ingest_data_file(
        transaction_id,
        "0",
        False,
        chunk_location.host,
        chunk_location.port,
        chunk_location.http_host,
        chunk_location.http_port,
        data_file=data_file,
        table=table,
        load_http=load_http,
    )
    repl.commit_transaction(transaction_id)
    repl.publish_database(database)


def integration_test(
    repl_connection: str,
    unload: bool,
    load: bool | None,
    reload: bool,
    load_http: bool,
    cases: list[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
) -> ITestResults:
    if repl_connection is not None:
        _do_smig_block(admin_smig_dir, "replica", repl_connection)

    return _integration_test.run_integration_tests(
        unload=unload,
        load=load,
        reload=reload,
        load_http=load_http,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
        mysqld_user=mysqld_user_qserv,
        mysqld_password=mysqld_user_qserv_password,
    )


def integration_test_http(
    repl_connection: str,
    unload: bool,
    load: bool | None,
    reload: bool,
    load_http: bool,
    cases: list[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
) -> ITestResults:
    if repl_connection is not None:
        _do_smig_block(admin_smig_dir, "replica", repl_connection)

    return _integration_test.run_integration_tests_http(
        unload=unload,
        load=load,
        reload=reload,
        load_http=load_http,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
        mysqld_user=mysqld_user_qserv,
        mysqld_password=mysqld_user_qserv_password,
    )


def integration_test_http_ingest(
    repl_connection: str,
    run_tests: bool,
    keep_results: bool,
    tests_yaml: str,
) -> bool:
    if repl_connection is not None:
        _do_smig_block(admin_smig_dir, "replica", repl_connection)

    return _integration_test.run_integration_tests_http_ingest(
        run_tests=run_tests,
        keep_results=keep_results,
        tests_yaml=tests_yaml,
    )


def prepare_data(
    tests_yaml: str,
) -> bool:
    return _integration_test.prepare_data(tests_yaml=tests_yaml)


def spawned_app_help(
    cmd: str,
) -> None:
    """Print the help output for a spawned app.

    Parameters
    ----------
    cmd : str
        The name of the command that spawns an app. May be followed by arguments
        & options to the command, these are ignored.
    """
    app = cmd.split()[0]
    print(f"Help for '{app}':\n", flush=True)
    _run(cmd=f"{app} --help", args=None)
    sys.exit(0)
