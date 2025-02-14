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

"""Module defining methods used for schema migration in the admin module."""

__all__ = ["make_migration_manager"]


from collections.abc import Sequence

from ..schema import Migration, SchemaMigMgr, Uninitialized


class AdminMigrationManager(SchemaMigMgr):
    """Class implementing schema migration for QMeta database."""

    def __init__(self, connection: str, scripts_dir: str):
        super().__init__(scripts_dir, connection)

    def current_version(self) -> int | None:
        """Returns current schema version.

        Returns
        -------
        version : `int` or ``Uninitialized``
            The current schema version.
        """
        # Currently the database has been initialized with user data or it has
        # not been initialized at all. Check to see if one of the expected
        # users exists
        stmt = "SELECT COUNT(*) FROM mysql.user WHERE user='qsmaster' AND host='localhost'"
        cursor = self.connection.cursor()
        cursor.execute(stmt)
        result = cursor.fetchone()
        try:
            count = result[0]
        except Exception:
            raise RuntimeError(f"Could not extract version from query result: {result}.")
        return Uninitialized if count == 0 else 0

    def apply_migrations(self, migrations: Sequence[Migration]) -> int:
        """Apply migrations.

        Parameters
        ----------
        migrations : `list` [``Migrations``]
            Migrations to apply, in order.

        Returns
        -------
        version : `int`
            The current version number after applying migrations.
        """
        super().apply_migrations(migrations)
        cur = self.current_version()
        if cur != migrations[-1].to_version:
            raise RuntimeError(f"Failed to update admin schema to version {migrations[-1]}.")
        return cur


def make_migration_manager(connection: str, scripts_dir: str) -> AdminMigrationManager:
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
    return AdminMigrationManager(connection, scripts_dir)
