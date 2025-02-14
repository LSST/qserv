"""Module defining methods used in schema migration of QMeta database."""

__all__ = ["make_migration_manager"]

import logging
from collections.abc import Sequence

import backoff
from lsst.qserv.admin.qserv_backoff import max_backoff_sec, on_backoff
from lsst.qserv.schema import Migration, SchemaMigMgr, Uninitialized, Version

import mysql.connector

_log = logging.getLogger(__name__)

database = "qservMeta"


class QMetaMigrationManager(SchemaMigMgr):
    """Class implementing schema migration for QMeta database."""

    def __init__(self, connection: str, scripts_dir: str):
        super().__init__(scripts_dir, connection)

    def current_version(self) -> Version:
        """Returns current schema version.

        Returns
        -------
        version : `int` or ``Uninitialized``
            The current schema version.
        """

        # If the database does not exist then it's Uninitialized.
        if not self.database_exists(database):
            return Version(Uninitialized)

        # Initial database schema implementation did not have version number stored at all,
        # and we call this version 0. Since version=1 version number is stored in
        # QMetadata table with key="version"
        if not self.table_exists(database, "QMetadata"):
            return 0

        self.connection.database = database
        cursor = self.connection.cursor()
        cursor.execute("SELECT value FROM QMetadata WHERE metakey = 'version'")
        result = cursor.fetchone()
        if not result:
            return Uninitialized
        return Version(int(result[0]))

    @backoff.on_exception(
        exception=(mysql.connector.errors.OperationalError, mysql.connector.errors.ProgrammingError),
        wait_gen=backoff.expo,
        on_backoff=on_backoff(log=_log),
        max_time=max_backoff_sec,
    )
    def _set_version(self, version: int) -> None:
        """Set the version number stored in QMetadata."""
        # make sure that current version is updated in the database
        self.connection.database = database
        cursor = self.connection.cursor()
        cursor.execute(f"UPDATE QMetadata SET value = {version} WHERE metakey = 'version'")
        self.connection.commit()

        # read it back and compare with expected
        current = self.current_version()
        if current != version:
            raise RuntimeError(
                f"Failed to update database {database} to {version}, current version is {current}"
            )

    def apply_migrations(self, migrations: Sequence[Migration]) -> Version:
        """Apply migrations.

        Parameters
        ----------
        migrations : `list` [``Migration``]
            Migrations to apply, in order.

        Returns
        -------
        version : `Version`
            The current version after applying migrations.
        """
        version = super().apply_migrations(migrations)
        self._set_version(version)
        return Version(version)


def make_migration_manager(connection: str, scripts_dir: str) -> SchemaMigMgr:
    """Factory method for admin schema migration manager

    This method is needed to support dynamic loading in `qserv-smig` script.

    Parameters
    ----------
    connection : `str`
        The uri to the module database.
    scripts_dir : `str`
        Path where migration scripts are located, this is system-level directory,
        per-module scripts are usually located in sub-directories.
    """
    return QMetaMigrationManager(connection, scripts_dir)
