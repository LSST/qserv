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


"""Module defining methods used in schema migration of results processing database."""

__all__ = ["make_migration_manager"]


from lsst.qserv.schema import SchemaMigMgr, Uninitialized, Version


database = "qservResult"


class RprocMigrationManager(SchemaMigMgr):
    """Class implementing schema migration for results processing database."""

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
        if not self.databaseExists(database):
            return Uninitialized

        # The database does not have multiple versions yet, so if it doesn't exists
        # then assume version 0.
        return Version(0)


def make_migration_manager(connection: str, scripts_dir: str) -> SchemaMigMgr:
    """Factory method for schema migration manager

    This method is needed to support dynamic loading in `qserv-smig` script.

    Parameters
    ----------
    connection : `str`
        The uri to the module database.
    scripts_dir : `str`
        Path where migration scripts are located, this is system-level directory,
        per-module scripts are usually located in sub-directories.
    """
    return RprocMigrationManager(connection, scripts_dir)
