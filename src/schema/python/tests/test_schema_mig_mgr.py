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


import os
import unittest
from tempfile import TemporaryDirectory
from unittest.mock import MagicMock

from lsst.qserv.schema import MigMatch, Migration, SchemaMigMgr, Uninitialized, Version


def make_files(directory, *names):
    for name in names:
        with open(os.path.join(directory, name), "w") as f:
            f.write("foo")


class SchemaMigMgrTestCase(unittest.TestCase):
    def test_script_match(self):
        """Verify script_match uses the regex function to match expected
        filename strings and not unexpected ones."""
        self.maxDiff = None
        self.assertEqual(
            SchemaMigMgr.script_match("migrate-0-to-1.sql"), MigMatch(from_version=0, to_version=1)
        )
        self.assertEqual(SchemaMigMgr.script_match("migrate-foo-to-1.sql"), None)
        self.assertEqual(SchemaMigMgr.script_match("migrate-None-to-4.sql"), MigMatch(Uninitialized, 4))
        self.assertEqual(
            SchemaMigMgr.script_match("migrate-None-to-4-somedescriptor.sql"),
            MigMatch(Uninitialized, 4),
        )

    def test_scripts(self):
        """Verify the default init creates the scripts member variable
        correctly from migration scripts found in the passed-in directory path.
        """

        class TestMigMgr(SchemaMigMgr):
            def current_version(self):
                pass

            def _connect(self, connection: str) -> None:
                self.connection = None

            def apply_migrations(self, migrations):
                pass

        none_to_two = "migrate-None-to-2.sql"
        one_to_two = "migrate-1-to-2.sql"
        # 2-to-2 tests the `v_from == v_to` condition in find_scripts
        two_to_two = "migrate-2-to-2.sql"

        with TemporaryDirectory() as tmpdir:
            make_files(tmpdir, none_to_two, one_to_two, two_to_two, "unused.sql")
            mgr = TestMigMgr(tmpdir, "connection-uri")
            self.assertEqual(
                set(mgr.migrations),
                set(
                    (
                        Migration(Uninitialized, 2, none_to_two, os.path.join(tmpdir, none_to_two)),
                        Migration(1, 2, one_to_two, os.path.join(tmpdir, one_to_two)),
                    )
                ),
            )

    def test_migration_latest_version(self):
        """Verify the Migration less than operator works as expected by
        verifying that the max version gets set correctly."""
        uninit_to_three = Migration(Uninitialized, 3, "foo", "/path/to/foo")
        two_to_three = Migration(2, 3, "foo", "/path/to/foo")
        uninit_to_two = Migration(Uninitialized, 2, "foo", "/path/to/foo")

        class TestMigMgr(SchemaMigMgr):
            def __init__(self, migrations):
                self.migrations = migrations

            def current_version(self):
                pass

            def apply_migrations(self, migrations):
                pass

        mgr = TestMigMgr([uninit_to_three, two_to_three, uninit_to_two])
        self.assertEqual(mgr.max_migration_version, 3)

    def test_migration_path(self):
        """Verify the algorithm in migration_path returns the shortest legal
        path between versions."""
        uninit_to_one = Migration(Uninitialized, 1, "unused", "unused")
        uninit_to_two = Migration(Uninitialized, 2, "unused", "unused")
        uninit_to_four = Migration(Uninitialized, 4, "unused", "unused")
        two_to_four = Migration(2, 4, "unused", "unused")

        # test uninit to 4 alone
        self.assertEqual(
            SchemaMigMgr.migration_path(Version(Uninitialized), Version(4), [uninit_to_four]),
            [uninit_to_four],
        )
        # test uninit to 2, 2 to 4.
        self.assertEqual(
            SchemaMigMgr.migration_path(Version(Uninitialized), Version(4), [uninit_to_two, two_to_four]),
            [uninit_to_two, two_to_four],
        )
        # test that uninit to 4 is chosen over uninit to 2 with 2 to 4
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(Uninitialized), Version(4), [uninit_to_two, two_to_four, uninit_to_four]
            ),
            [uninit_to_four],
        )
        # test that uninit to 2, 2 to 4 is chosen over uninit to 1, 1 to 2, 2 to 4.
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(Uninitialized), Version(4), [uninit_to_one, uninit_to_two, two_to_four]
            ),
            [uninit_to_two, two_to_four],
        )
        # test 2 to 4 out of the crowded field.
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(2), Version(4), [uninit_to_one, uninit_to_two, two_to_four, uninit_to_four]
            ),
            [two_to_four],
        )
        # test going to an unsupported version.
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(2), Version(5), [uninit_to_one, uninit_to_two, two_to_four, uninit_to_four]
            ),
            [],
        )

    def test_migrate(self):
        """Test the migrate function.

        This touches most of the other parts of the SchemaMigMgr.
        """
        mock = MagicMock()

        class TestMigMgr(SchemaMigMgr):
            def __init__(self, current_version, scripts_dir):
                self._current_version = current_version
                super().__init__(scripts_dir, "connection-uri")

            def _connect(self, connection: str) -> None:
                self.connection = None

            def current_version(self):
                return self._current_version

            def apply_migrations(self, migrations):
                mock(migrations)

        with TemporaryDirectory() as scriptsdir:
            none_to_three = Migration(
                Uninitialized,
                3,
                "migrate-None-to-3.sql",
                os.path.join(scriptsdir, "migrate-None-to-3.sql"),
            )
            none_to_one = Migration(
                Uninitialized,
                1,
                "migrate-None-to-1.sql",
                os.path.join(scriptsdir, "migrate-None-to-1.sql"),
            )
            one_to_two = Migration(
                1,
                2,
                "migrate-1-to-2.sql",
                os.path.join(scriptsdir, "migrate-1-to-2.sql"),
            )
            two_to_three = Migration(
                2,
                3,
                "migrate-2-to-3.sql",
                os.path.join(scriptsdir, "migrate-2-to-3.sql"),
            )
            make_files(
                scriptsdir, *[m.filepath for m in [none_to_three, none_to_one, one_to_two, two_to_three]]
            )

            mgr = TestMigMgr(Uninitialized, scriptsdir)
            mgr.migrate(to_version=3, do_migrate=True)
            mock.assert_called_once_with([none_to_three])
            mock.reset_mock()

            mgr = TestMigMgr(Uninitialized, scriptsdir)
            mgr.migrate(to_version=2, do_migrate=True)
            mock.assert_called_once_with([none_to_one, one_to_two])
            mock.reset_mock()

            mgr = TestMigMgr(1, scriptsdir)
            mgr.migrate(to_version=3, do_migrate=True)
            mock.assert_called_once_with([one_to_two, two_to_three])
            mock.reset_mock()

            # remove the short None-to-3:
            os.remove(none_to_three.filepath)
            # add a short 1-to-3
            one_to_three = Migration(
                1,
                3,
                "migrate-1-to-3.sql",
                os.path.join(scriptsdir, "migrate-1-to-3.sql"),
            )
            make_files(scriptsdir, one_to_three.filepath)
            # verify it skips 1-to-2 + 2-to-3, and uses 1-to-3
            mgr = TestMigMgr(Uninitialized, scriptsdir)
            mgr.migrate(to_version=3, do_migrate=True)
            mock.assert_called_once_with([none_to_one, one_to_three])
            mock.reset_mock()

            # remove the ways to proceed past one, and verify an error
            os.remove(one_to_two.filepath)
            os.remove(one_to_three.filepath)
            mgr = TestMigMgr(Uninitialized, scriptsdir)
            with self.assertRaises(ValueError):
                mgr.migrate(to_version=3, do_migrate=True)


class VersionTestCase(unittest.TestCase):
    def test_copy(self):
        """Test that __new__ works to create new types of Version."""
        a = Version(1)
        b = Version(a)
        self.assertEqual(b, 1)

    def test_comp(self):
        """Test comparison operators."""
        self.assertEqual(Version(1), 1)
        self.assertNotEqual(Version(2), 1)
        self.assertEqual(Version(Uninitialized), Uninitialized)
        self.assertNotEqual(Version(2), Uninitialized)
        self.assertTrue(Version(0) < Version(2))
        self.assertFalse(Version(0) > Version(2))
        self.assertTrue(Version(0) <= Version(0))
        self.assertTrue(Version(Uninitialized) < Version(2))

    def test_cast(self):
        """Test casting values to Version."""
        a = Version(1)
        self.assertTrue(a is Version.cast(a))
        b = 1
        self.assertTrue(b is not Version.cast(b))
        self.assertIsInstance(Version.cast(b), Version)


if __name__ == "__main__":
    unittest.main()
