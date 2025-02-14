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


"""Module defining SchemaMigMgr abstract base class."""

from __future__ import annotations

__all__ = ["SchemaMigMgr", "Uninitialized"]

import logging
import os
import re
from abc import ABCMeta, abstractmethod
from collections.abc import Callable
from contextlib import closing
from dataclasses import dataclass
from typing import cast

import backoff
import jinja2

# MySQLInterfaceError can get thrown, we need to catch it.
# It's not exposed as a public python object but *is* used in mysql.connector unit tests.
from _mysql_connector import MySQLInterfaceError
from sqlalchemy.engine.url import make_url

import mysql.connector

from ..admin.qserv_backoff import max_backoff_sec, on_backoff
from ..admin.template import get_template_cfg

_log = logging.getLogger(__name__)
database_error_connection_refused_code = 2003


MigMgrArgs = dict[str, Callable[[], None] | str | None]


class SchemaUpdateRequired(RuntimeError):
    """Error that indicates that a schema update is required.

    It may be raised when the update could not be performed because necessary
    arguments (e.g. update=True) were not passed.
    """

    pass


class Uninitialized:
    """Represents an uninitialized database, which has no integer version
    number.
    """

    def __init__(self) -> None:
        raise TypeError("UninitializedType is not callable")


class Version:
    """Represents a schema version. The value may be an integer version or
    `Uninitialized` if the schema is not yet initialized to any version.
    """

    def __init__(self, value: int | type[Uninitialized]):
        if isinstance(value, Version):
            self.value: int | type[Uninitialized] = value.value
            return
        if not isinstance(value, int) and value is not Uninitialized:
            raise RuntimeError(f"Version value must be an int or Uninitialized, not {value}")
        self.value = value

    @staticmethod
    def cast(other: int | type[Uninitialized] | Version) -> Version:
        """Ensure that the passed-in object is a Version instance; if it is
        just return it, if not create a Version and use it as the value.
        """
        if isinstance(other, Version):
            return other
        return Version(other)

    @staticmethod
    def _validateOther(other: object) -> bool:
        return isinstance(other, int | Version) or other is Uninitialized

    @staticmethod
    def _otherVal(other: object) -> int | type[Uninitialized]:
        """Get the value of the other value. May be a ``Version`` instance,
        ``Uninitialized``, or an ``int``.

        `self._validateOther` should be called before calling this function.
        """
        if isinstance(other, Version):
            return other.value
        if isinstance(other, int):
            return int(other)
        if other is Uninitialized:
            return Uninitialized
        raise NotImplementedError(f"_otherVal is not implemented for the type of {other}")

    def __lt__(self, other: object) -> bool:
        if not self._validateOther(other):
            return NotImplemented
        otherVal = self._otherVal(other)
        if self.value is Uninitialized and otherVal is not Uninitialized:
            return True
        if isinstance(self.value, int) and isinstance(otherVal, int):
            return self.value < otherVal
        return False

    def __gt__(self, other: object) -> bool:
        if not self._validateOther(other):
            return NotImplemented
        return not self.__lt__(other) and not self.__eq__(other)

    def __ge__(self, other: object) -> bool:
        if not self._validateOther(other):
            return NotImplemented
        return not self < other

    def __eq__(self, other: object) -> bool:
        if not self._validateOther(other):
            return NotImplemented
        other = self._otherVal(other)
        if self.value is Uninitialized and other is Uninitialized:
            return True
        if self.value is Uninitialized or other is Uninitialized:
            return False
        return self.value == other

    def __hash__(self) -> int:
        return hash(self.value)

    def __repr__(self) -> str:
        return str(self.value)

    def __str__(self) -> str:
        return "Uninitialized" if self.value is Uninitialized else str(self.value)


class Migration:
    """Contains information about a schema migration; the "from" and "to"
    versions, the name of the migration file, and the full path of the
    migration file.
    """

    @property
    def dirname(self) -> str:
        return os.path.dirname(self.filepath)

    def __init__(
        self,
        from_version: int | type[Uninitialized],
        to_version: int,
        name: str,
        filepath: str,
    ):
        self.from_version = Version(from_version)
        self.to_version = Version(to_version)
        self.name = name
        self.filepath = filepath  # path and filename

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Migration):
            return NotImplemented
        return (self.from_version, self.to_version, self.name, self.filepath) == (
            other.from_version,
            other.to_version,
            other.name,
            other.filepath,
        )

    def __lt__(self, other: Migration) -> bool:
        return (self.from_version, self.to_version, self.name, self.filepath) < (
            other.from_version,
            other.to_version,
            other.name,
            other.filepath,
        )

    def __hash__(self) -> int:
        return hash((self.from_version, self.to_version, self.name, self.filepath))

    def __repr__(self) -> str:
        return (
            f"Migration(from_version={self.from_version}, to_version={self.to_version}, "
            f"name={self.name}, filepath={self.filepath})"
        )


@dataclass
class MigMatch:
    """Helper for tracking schema migrations in SchemaMigMgr."""

    from_version: int | type[Uninitialized]
    to_version: int


class SchemaMigMgr(metaclass=ABCMeta):
    """Abstract base class for schema migration managers.

    Parameters
    ----------
    scripts_dir : `str`
        The folder in which to look for migration scripts.
    connection : `str`
        The uri to the module database.
    """

    @abstractmethod
    def current_version(self) -> int | type[Uninitialized]:
        """Returns current schema version.

        Returns
        -------
        version : `int` or ``Uninitialized``
            The current schema version.
        """
        pass

    # mig_name_re describes a common migration file name pattern. Subclasses
    # can override this. This pattern looks for name "migrate-N-to-M.sql" or
    # "migrate-N-to-M-something.sql", and it can have a ".jinja" suffix. N can
    # be an integer or "None" (without quotes). Note that to use a different re
    # with the SchemaMigMgr implementation of current_version, the re must
    # include named capture groups for the "from" and "to" migration versions.
    mig_name_re = re.compile(r"migrate-(?P<from>\d+|None)-to-(?P<to>\d+)(-.*)?.sql(\.jinja)?\Z")

    def __init__(self, scripts_dir: str, connection: str):
        # find all migration scripts, add full script path to each
        self.connection = self._connect(connection)
        self.migrations = self.find_scripts(scripts_dir, self.script_match)

    def close(self) -> None:
        if self.connection:
            self.connection.close()

    @property
    def max_migration_version(self) -> Version | None:
        """Get the migration Version with the highest 'to' version."""
        if not self.migrations:
            return None
        return max(m.to_version for m in self.migrations)

    @backoff.on_exception(
        exception=(
            mysql.connector.errors.DatabaseError,
            MySQLInterfaceError,
            mysql.connector.errors.InterfaceError,
            mysql.connector.errors.ProgrammingError,
        ),
        wait_gen=backoff.expo,
        on_backoff=on_backoff(log=_log),
        max_time=max_backoff_sec,
    )
    def _connect(self, connection: str) -> mysql.connector.abstracts.MySQLConnectionAbstract:
        url = make_url(connection)
        # The database is not always guaranteed to exist yet (sometimes we connect and then create it)
        # so cache it, and it can be set in the connection before use when it is known to exist.
        self.database = url.database
        kwargs = dict(
            user=url.username,
            password=url.password,
        )
        if "socket" in url.query:
            kwargs["unix_socket"] = url.query["socket"]
        else:
            kwargs.update(host=url.host, port=url.port)
        # Cast justified because no pool args passed here to connect() so cannot be PooledMySQLConnection
        return cast(
            mysql.connector.abstracts.MySQLConnectionAbstract,
            mysql.connector.connect(**kwargs),
        )

    @backoff.on_exception(
        exception=mysql.connector.errors.DatabaseError,
        wait_gen=backoff.expo,
        on_backoff=on_backoff(log=_log),
        # Do give up, unless error is that the connection was refused (assume db is starting up)
        max_time=max_backoff_sec,
        giveup=lambda e: isinstance(e, MySQLInterfaceError)
        and e.errno != database_error_connection_refused_code,
    )
    @backoff.on_exception(
        exception=mysql.connector.errors.OperationalError,
        wait_gen=backoff.expo,
        on_backoff=on_backoff(log=_log),
        max_time=max_backoff_sec,
        # Can not check errno for the failure reason, no codes are published in
        # docs. Assuming it's because the connection was rejected, and hoping that
        # retry will succeed.
    )
    def apply_migrations(self, migrations: list[Migration]) -> Version | None:
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
        if not migrations:
            return None
        with closing(self.connection.cursor()) as cursor:
            for migration in migrations:
                _log.info("--- Executing migration script %s", migration.name)
                stmt = None
                if migration.filepath.endswith(".jinja"):
                    env = jinja2.Environment(
                        loader=jinja2.FileSystemLoader(migration.dirname),
                        autoescape=jinja2.select_autoescape("sql"),
                        undefined=jinja2.StrictUndefined,
                    )
                    try:
                        stmt = env.get_template(migration.name).render(get_template_cfg())
                    except jinja2.exceptions.UndefinedError as e:
                        raise RuntimeError(
                            "A template parameter is missing from the configuration for "
                            f"{migration.filepath}: {e.message}"
                        ) from e
                else:
                    with open(migration.filepath) as f:
                        stmt = f.read()
                _log.debug(f"Migration statement: {stmt}")
                if stmt:
                    for result in cursor.execute(stmt, multi=True):  # type: ignore
                        # Cast here because MySQLCursorAbtract does not have with_rows for some reason, even
                        # though both of its subclasses do...
                        result = cast(
                            mysql.connector.cursor.MySQLCursor | mysql.connector.cursor_cext.CMySQLCursor,
                            result,
                        )
                        if result.with_rows:
                            result.fetchall()
                else:
                    _log.warn(
                        "Migration statement was empty, nothing to execute."
                    )  # empty migration files are used for testing.
                self.connection.commit()
                _log.info("+++ Script %s completed successfully", migration.name)
        return Version(migration.to_version.value)

    def migrate(self, to_version: int | None = None, do_migrate: bool = False) -> int | None:
        """Perform schema migration from current version to given version.

        Parameters
        ----------
        to_version : int or None
            If None then migrate to latest known version, otherwise only
            migrate to given version.
        do_migrate : bool
            If True perform the migration, otherwise only print steps that
            should be performed.

        Returns
        -------
        None if no migrations were performed or the version number at
        which migration has stopped.

        Raises
        ------
        Exception is raised for any migration errors. The state of the
        database is not guaranteed to be consistent after exception.
        """
        from_version = Version.cast(self.current_version())
        resolved_to_version = self.max_migration_version if to_version is None else Version(to_version)
        if resolved_to_version is None:
            _log.warn("to_version is None - no 'migrate to' version could be found?")
            return None
        # checks for requested resolved_to_version
        if from_version >= resolved_to_version:
            _log.debug(
                "current version (%s) is already same or newer as requested (%s)",
                from_version,
                resolved_to_version,
            )
            return None
        migrations = self.migration_path(from_version, resolved_to_version, self.migrations)
        if not migrations:
            raise ValueError(
                f"No known scripts to migrate from version {from_version} to version {resolved_to_version}."
            )
        if do_migrate:
            final_version = self.apply_migrations(migrations)
            if isinstance(final_version, int):
                return final_version
            return None
        else:
            # Todo can probably format migrations more nicely.
            print(f"Would apply migrations: {migrations}")
        return None

    @staticmethod
    def migration_path(
        from_version: Version,
        to_version: Version,
        migrations: list[Migration],
    ) -> list[Migration] | None:
        """Look in migrations and find the shortest possible path between
        versions.

        Parameters
        ----------
        from_version : `Version`
            The version to migrate from.
        to_version : `Version`
            The version to migrate to.
        migrations : `list` [`Migration`]
            The available migrations.

        Returns
        -------
        migrations: `list` [``Migration``] or `None`
            The shortest possible list of migrations from the two versions,
            `None` if there is not a possible way to migrate from the given
            versions.
        """
        # First see if any migrations exactly match our from and to versions.
        for migration in migrations:
            if migration.from_version == from_version and migration.to_version == to_version:
                return [migration]
        # See if there are intermedate migrations that go to our to_version and
        # start at a from_version that is higher than our desired from_version
        # (we have already checked for versions from our desired from_version).
        # If there are multiple paths from from_version to to_version, choose
        # the shortest one.
        mig = None
        for migration in migrations:
            if migration.to_version == to_version and migration.from_version > from_version:
                intermediate_mig = SchemaMigMgr.migration_path(
                    from_version, migration.from_version, migrations
                )
                if intermediate_mig and (mig is None or len(intermediate_mig) + 1 < len(mig)):
                    mig = intermediate_mig
                    mig.append(migration)
        return mig or []

    @classmethod
    def script_match(cls, fname: str) -> MigMatch | None:
        """Match script name against pattern.

        Returns
        -------
        versions : `None` or `MigMatch`
            None if no match. A `MigMatch` instance if there is a match, the
            from_version may be `Uninitialized` to indicate "from an uninitialized
            database", or an integer to indicate the version the script can
            upgrade from, and the second value will be an integer indicating the
            version the script will upgrade to.
        """
        match = cls.mig_name_re.match(fname)
        if match:
            v_from_ = match.group("from")
            v_from: int | type[Uninitialized] = Uninitialized if v_from_ == "None" else int(v_from_)
            v_to = int(match.group("to"))
            _log.debug("from %s to %s", v_from, v_to)
            return MigMatch(from_version=v_from, to_version=v_to)
        return None

    def latest_version(self) -> Version:
        """Returns latest known schema version.

        Returns
        -------
        version : `int`
            The latest known schema version.
        """
        version = self.max_migration_version
        _log.debug("latest version from migration scripts: %s", version)
        if version is None:
            # no migration scripts - current version is latest
            version = Version(self.current_version())
            _log.debug("no migration scripts, returning current version %s", version)
        return version

    @classmethod
    def find_scripts(cls, scripts_dir: str, match_fun: Callable[[str], MigMatch | None]) -> list[Migration]:
        """Helper function for finding migration scripts.

        Parameters
        ----------
        scripts_dir : str
            Path to a directory where scripts are located.
        match_fun : function
            Function which takes one argument which is a script name (without
            directory) and returns None if script is not useful or MigMatch
            (which is a dataclass containing from_version, to_version) with
            version numbers. If versions are identical then script is ignored.

        Returns
        -------
        migrations : `list` [``Migration``]
            List of Migration instances that describe possible migrations.

        Raises
        ------
        Exception is raised if directory does not exist or if `match_fun`
        raises exception or returns unexpected value.
        """
        migrations = []
        for fname in os.listdir(scripts_dir):
            _log.debug("checking script %s", fname)
            match = match_fun(fname)
            if match is not None:
                _log.debug("matching script %s: %s -> %s", fname, match.from_version, match.to_version)
                if match.from_version == match.to_version:
                    _log.warning(
                        "script %s has identical versions, skipping", os.path.join(scripts_dir, fname)
                    )
                else:
                    migrations.append(
                        Migration(
                            match.from_version, match.to_version, fname, os.path.join(scripts_dir, fname)
                        )
                    )
        return migrations

    def databaseExists(self, dbName: str) -> bool:
        """Determine if a database exists

        Parameters
        ----------
        dbName : `str`
            The name of the database.

        Returns
        -------
        exists : `bool`
            True if the database exists, else False.
        """
        with closing(self.connection.cursor()) as cursor:
            cursor.execute("SHOW DATABASES")
            if (dbName,) in cursor.fetchall():
                return True
        return False

    def tableExists(self, dbName: str, tableName: str) -> bool:
        """Determine if a table exists

        Parameters
        ----------
        dbName : `str`
            The name of the database.
        tableName : `str`
            The name of the table.

        Returns
        -------
        exists : `bool`
            True if the table exists, else False.
        """
        with closing(self.connection.cursor()) as cursor:
            cursor.execute(
                f"SELECT 1 FROM information_schema.TABLES "
                f"WHERE TABLE_SCHEMA = '{dbName}' "
                f"AND TABLE_NAME = '{tableName}'"
            )
            if not cursor.fetchone():
                return False
        return True
