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


import backoff
from contextlib import closing
import importlib
import logging
import mysql.connector
from _mysql_connector import MySQLInterfaceError
from typing import Optional, Type, Union

from ..admin.qserv_backoff import max_backoff_sec, on_backoff
from . import MigMgrArgs, Migration, SchemaMigMgr, SchemaUpdateRequired, Uninitialized, Version


_mig_module_name = "schema_migration"
_factory_method_name = "make_migration_manager"
_log = logging.getLogger(__name__)


def _load_migration_mgr(
    mod_name: str,
    connection: str,
    scripts_dir: str,
    mig_mgr_args: MigMgrArgs = dict(),
) -> SchemaMigMgr:
    """Dynamic loading of the migration manager based on module name.

    Parameters
    ----------
    mod_name : `str`
        Module name, e.g. "qmeta"
    connection : `str`
        The uri to the module database.
    scripts_dir : `str`
        Path where migration scripts are located, this is system-level directory,
        per-module scripts are usually located in sub-directories.
    mig_mgr_args: MigMgrArgs
        Optional; kwargs that will be expanded when calling the migration manager factory function.

    Returns
    -------
    Object which manages migrations for that module.

    Raises
    ------
    Exception is raised for any error.
    """

    # load module "lsst.qserv.<module>.schema_migration"
    try:
        mod_instance = importlib.import_module("lsst.qserv." + mod_name + "." + _mig_module_name)
    except ImportError:
        logging.error(
            "Failed to load %s module from lsst.qserv.%s package",
            _mig_module_name,
            mod_name,
        )
        raise

    # find a method with name "make_migration_manager"
    try:
        factory = getattr(mod_instance, _factory_method_name)
    except AttributeError:
        logging.error(
            "Module %s does not contain factory method %s.",
            _mig_module_name,
            _factory_method_name,
        )
        raise

    # call factory method, pass all needed arguments
    mgr = factory(connection=connection, scripts_dir=scripts_dir, **mig_mgr_args)

    return mgr


# def _normalizeConfig(config):
#     """Make connection parameters out of config.

#     We have a mess in our INI files in how we specify connection parameters
#     depending on which piece of C++ or Python code uses those parameters.
#     For our purposes I need to convert that mess into a mess acceptable by
#     getEngineFromArgs() method.

#     Parameters
#     ----------
#     config : dict
#         Parameters read from configuration file

#     Returns
#     -------
#     Dictionary with parameters passed to getEngineFromArgs()
#     """
#     res = {}
#     if config.get("technology") == "mysql":
#         res["drivername"] = "mysql+mysqldb"
#     elif config.get("technology") is not None:
#         raise ValueError(
#             "Unexpected technology specified for connection:"
#             " {}".format(config.get("technology"))
#         )
#     res["username"] = config.get("username") or config.get("user")
#     res["password"] = (
#         config.get("password") or config.get("passwd") or config.get("pass")
#     )
#     res["host"] = config.get("hostname") or config.get("host")
#     res["port"] = config.get("port")
#     res["database"] = config.get("database") or config.get("db")
#     socket = config.get("unix_socket") or config.get("socket")
#     if socket:
#         res["query"] = dict(unix_socket=socket)

#     return res


def smig(
    do_migrate: bool,
    check: bool,
    final: int,
    scripts: str,
    connection: str,
    module: str,
    mig_mgr_args: MigMgrArgs,
    update: bool,
) -> Optional[int]:
    """Execute schema migration.

    Parameters
    ----------
    do_migrate : `bool`
        True if the migration should be performed.
    check : `bool`
        Check that migration is needed, script returns 0 if schema is
        up-to-date, 1 otherwise.
    final : `int`
        Stop migration at given version, by default update to latest version.
    scripts : `str`
        Location for migration scripts.
    connection : `str`
        Connection string in format mysql://user:pass@host:port/database.
    module : `str`
        Name of Qserv module for which to update schema, e.g. qmeta.
    mig_mgr_args : `MigMgrArgs`
        List of arguments to forward to the module migration manager.
    update : `bool`
        `True` if modules may be upgraded from an integer version (or from
        `Uninitialized`). If `False` then modules may only be initialized, and
        if a module is already at an integer version then an error will be
        raised.

    Returns
    -------
    current : `int` or `None`
        If `check == True`: 0 if schema is current, or 1 if schema needs upgrading.
        Otherwise returns `None`.

    Raises
    ------
    IOError
        If the config_file directory does not exist.
    ImportError
        If the module can not be imported.
    AttributeError
        If the module does not have a migration manager factory method.
    ValueErrors
        If the config_file's 'technology' parameter specifies an unrecognized
        techology for database connection."""

    # if not connection:  # and not config_file:
    #     raise RuntimeError("A connection or config file is required.")

    # TODO do we still need to support smig cfg file?
    #      needs fixing to use mysql-connector instead of sqlalchmy
    #      (use of `engine` is removed)
    # elif config_file:
    #     if not config_section:
    #         parser.error("-s options required with -f")

    #     cfg = configparser.SafeConfigParser()
    #     if not cfg.read([config_file]):
    #         # file was not found, generate exception which should happen
    #         # if we tried to open that file
    #         raise IOError(2, "No such file or directory: '{}'".format(config_file))

    #     # will throw is section is missing
    #     config = dict(cfg.items(config_section))

    #     # instantiate database engine
    #     config = _normalizeConfig(config)
    #     engine = engineFactory.getEngineFromArgs(**config)

    # make an object which will manage migration process

    with closing(
        _load_migration_mgr(
            module,
            connection=connection,
            scripts_dir=scripts,
            mig_mgr_args=mig_mgr_args,
        )
    ) as mgr:
        current = mgr.current_version()
        _log.info("Current {} schema version: {}".format(module, current))

        latest = mgr.latest_version()
        _log.info("Latest {} schema version: {}".format(module, latest))

        def format_migration(migration: Migration) -> str:
            tag = " (X)" if migration.from_version >= current else ""
            return f"{migration.from_version} -> {migration.to_version} : {migration.name}{tag}"

        _log.info(
            f"Known migrations for {module}: "
            f"{', '.join(format_migration(migration) for migration in mgr.migrations)}"
        )

        if check:
            return 0 if mgr.current_version() == mgr.latest_version() else 1

        if (
            mgr.current_version() != Uninitialized
            and mgr.current_version() != mgr.latest_version()
            and update == False
        ):
            raise SchemaUpdateRequired(
                f"Can not upgrade {module} from version {mgr.current_version()} without upgrade=True."
            )

        if not mgr.migrations:
            raise RuntimeError(f"Did not find any migrations for {module}")

        # do the migrations
        migrated_to = mgr.migrate(final, do_migrate)
        if migrated_to is None:
            _log.info("No migration was needed")
        else:
            if do_migrate:
                _log.info("Database was migrated to version {}".format(migrated_to))
            else:
                _log.info("Database would be migrated to version {}".format(migrated_to))
    return None


class VersionMismatchError(RuntimeError):
    """Rasing a VersionMismatchError indicates that the schema do not match yet.
    This can be handled by @backoff which may retry later.
    """

    def __init__(self, module: str, current: Union[int, Type[Uninitialized]], latest: Version):
        super().__init__(f"Module {module} schema is at version {current}, latest is {latest}")


@backoff.on_exception(
    exception=(
        VersionMismatchError,
        mysql.connector.errors.DatabaseError,
        MySQLInterfaceError,
        mysql.connector.errors.ProgrammingError,
    ),
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def smig_block(connection: str, scripts: str, module: str) -> None:
    """Block waiting for another process to run schema migration on a database.

    Parameters
    ----------
    connection : `str`
        The database connection string.
    scripts : `str`
        The path to the migration scripts directory.
    module : `str`
        The name of the module whose database is being migrated.
    """
    if not connection:
        raise RuntimeError("A connection is required.")

    with closing(_load_migration_mgr(module, connection=connection, scripts_dir=scripts)) as mgr:
        current = mgr.current_version()
        latest = mgr.latest_version()
        if current != latest:
            raise VersionMismatchError(module=module, current=current, latest=latest)
        _log.info(f"smig_block: module {module} is at the latest version: {current}, continuing.")
