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

"""Module defining methods used in schema migration of the Replication system's database."""

__all__ = ["make_migration_manager"]

import logging
from contextlib import closing

from lsst.qserv.schema import (
    Migration,
    SchemaMigMgr,
    Uninitialized,
    Version,
)
from sqlalchemy.engine.url import make_url

_log = logging.getLogger(__name__)

database = "qservReplica"


class MasterReplicationMigrationManager(SchemaMigMgr):
    """Class implementing schema migration for the master replication
    controller database.

    Parameters
    ----------
    Same as make_migration_manager
    """

    def __init__(
        self,
        connection: str,
        scripts_dir: str,
        repl_connection: str | None,
    ):
        self.repl_connection = repl_connection
        super().__init__(scripts_dir, connection)

    def current_version(self) -> Version:
        """Returns current schema version.

        Returns
        -------
        version : `int` or ``Uninitialized``
            The current schema version.
        """
        # If the database does not exist then the version is `Uninitialized`.
        if not self.databaseExists(database):
            return Version(Uninitialized)

        # Initial database schema implementation did not have version number stored at all,
        # and we call this version 0. Since version=1 version number is stored in
        # QMetadata table with key="version"
        if not self.tableExists(database, "QMetadata"):
            return 0

        self.connection.database = database
        with closing(self.connection.cursor()) as cursor:
            cursor.execute("SELECT value FROM QMetadata WHERE metakey = 'version'")
            result = cursor.fetchone()
        if not result:
            return Version(Uninitialized)
        return Version(int(result[0]))

    def _set_version(self, version: int) -> None:
        """Set the version number stored in QMetadata."""
        # Make sure that current version is updated in database.
        self.connection.database = database
        with closing(self.connection.cursor()) as cursor:
            cursor.execute(f"UPDATE QMetadata SET value = {version} WHERE metakey = 'version'")
            warnings = cursor.fetchwarnings()
            if warnings:
                _log.warn("Warnings were issued when updating version to %s", version)
        _log.info(f"Set replica schema version to {version}.")
        self.connection.commit()

        # Read it back and compare with expected.
        current = self.current_version()
        if current != version:
            raise RuntimeError(
                f"Failed to update version of database {database} to {version}, current version is {current}."
            )

    def _create_database(self) -> None:
        """Create the replication controller database."""
        with closing(self.connection.cursor()) as cursor:
            cursor.execute(f"CREATE DATABASE {database};")
            warnings = cursor.fetchwarnings()
            if warnings:
                _log.warn("Warnings were creating database %s.", database)
        _log.info("Created database %s.", database)
        self.connection.commit()

    def _create_users(self) -> None:
        """Create the users for the replication controller database."""
        if not self.repl_connection:
            raise RuntimeError(
                "A non-admin replication database connection uri must be provided to initialize the "
                "replication database."
            )
        user = make_url(self.repl_connection).username
        if not user:
            raise RuntimeError(
                "To initialize the replication database, the non-admin connection uri must contain a user "
                "name."
            )
        for stmt in [
            f"CREATE USER IF NOT EXISTS {user}@localhost;",
            f"CREATE USER IF NOT EXISTS {user}@'%';",
            f"GRANT ALL ON {database}.* TO  {user}@localhost;",
            f"GRANT ALL ON {database}.* TO  {user}@'%';",
            "FLUSH PRIVILEGES;",
        ]:
            with closing(self.connection.cursor()) as cursor:
                _log.info(f"executing: {stmt}")
                cursor.execute(stmt)
                warnings = cursor.fetchwarnings()
                if warnings:
                    _log.warn("Warnings were issued when creating user %s.", user)
        self.connection.commit()

    def apply_migrations(self, migrations: list[Migration]) -> Version:
        """Apply migrations.

        Parameters
        ----------
        migrations : `list` [``Migrations``]
            Migrations to apply, in order.

        Returns
        -------
        version : `Version`
            The current version number after applying migrations.
        """
        current_version = self.current_version()
        # The replica migrate-from-None scripts do not create the database or
        # the non-admin user, so if we are initializing the replica database
        # schema, first create them.
        if current_version == Uninitialized:
            self._create_database()
            self._create_users()
        self.connection.database = database
        to_version = super().apply_migrations(migrations)
        self._set_version(to_version)
        return Version(to_version)


def make_migration_manager(
    connection: str,
    scripts_dir: str,
    repl_connection: str | None = None,
) -> SchemaMigMgr:
    """Factory method for master replication controller schema migration
    manager

    This method is needed to support dynamic loading in `qserv-smig` script.

    Parameters
    ----------
    connection : `str`
        The uri to the module database.
    scripts_dir : `str`
        Path where migration scripts are located, this is system-level directory,
        per-module scripts are usually located in sub-directories.
    repl_connection : `str`
        Database connection string for the non-admin user.
    """
    return MasterReplicationMigrationManager(
        connection,
        scripts_dir,
        repl_connection,
    )
