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


def makeFiles(directory, *names):
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

        noneToTwo = "migrate-None-to-2.sql"
        oneToTwo = "migrate-1-to-2.sql"
        # 2-to-2 tests the `v_from == v_to` condition in find_scripts
        twoToTwo = "migrate-2-to-2.sql"

        with TemporaryDirectory() as tmpDir:
            makeFiles(tmpDir, noneToTwo, oneToTwo, twoToTwo, "unused.sql")
            migMgr = TestMigMgr(tmpDir, "connection-uri")
            self.assertEqual(
                set(migMgr.migrations),
                set(
                    (
                        Migration(Uninitialized, 2, noneToTwo, os.path.join(tmpDir, noneToTwo)),
                        Migration(1, 2, oneToTwo, os.path.join(tmpDir, oneToTwo)),
                    )
                ),
            )

    def test_migration_latest_version(self):
        """Verify the Migration less than operator works as expected by
        verifying that the max version gets set correctly."""
        uninitToThree = Migration(Uninitialized, 3, "foo", "/path/to/foo")
        twoToThree = Migration(2, 3, "foo", "/path/to/foo")
        uninitToTwo = Migration(Uninitialized, 2, "foo", "/path/to/foo")

        class TestMigMgr(SchemaMigMgr):
            def __init__(self, migrations):
                self.migrations = migrations

            def current_version(self):
                pass

            def apply_migrations(self, migrations):
                pass

        migMgr = TestMigMgr([uninitToThree, twoToThree, uninitToTwo])
        self.assertEqual(migMgr.max_migration_version, 3)

    def test_migration_path(self):
        """Verify the algorithm in migration_path returns the shortest legal
        path between versions."""
        uninitToOne = Migration(Uninitialized, 1, "unused", "unused")
        uninitToTwo = Migration(Uninitialized, 2, "unused", "unused")
        uninitToFour = Migration(Uninitialized, 4, "unused", "unused")
        twoToFour = Migration(2, 4, "unused", "unused")

        # test uninit to 4 alone
        self.assertEqual(
            SchemaMigMgr.migration_path(Version(Uninitialized), Version(4), [uninitToFour]),
            [uninitToFour],
        )
        # test uninit to 2, 2 to 4.
        self.assertEqual(
            SchemaMigMgr.migration_path(Version(Uninitialized), Version(4), [uninitToTwo, twoToFour]),
            [uninitToTwo, twoToFour],
        )
        # test that uninit to 4 is chosen over uninit to 2 with 2 to 4
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(Uninitialized), Version(4), [uninitToTwo, twoToFour, uninitToFour]
            ),
            [uninitToFour],
        )
        # test that uninit to 2, 2 to 4 is chosen over uninit to 1, 1 to 2, 2 to 4.
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(Uninitialized), Version(4), [uninitToOne, uninitToTwo, twoToFour]
            ),
            [uninitToTwo, twoToFour],
        )
        # test 2 to 4 out of the crowded field.
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(2), Version(4), [uninitToOne, uninitToTwo, twoToFour, uninitToFour]
            ),
            [twoToFour],
        )
        # test going to an unsupported version.
        self.assertEqual(
            SchemaMigMgr.migration_path(
                Version(2), Version(5), [uninitToOne, uninitToTwo, twoToFour, uninitToFour]
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

        with TemporaryDirectory() as scriptsDir:
            noneToThree = Migration(
                Uninitialized,
                3,
                "migrate-None-to-3.sql",
                os.path.join(scriptsDir, "migrate-None-to-3.sql"),
            )
            noneToOne = Migration(
                Uninitialized,
                1,
                "migrate-None-to-1.sql",
                os.path.join(scriptsDir, "migrate-None-to-1.sql"),
            )
            oneToTwo = Migration(
                1,
                2,
                "migrate-1-to-2.sql",
                os.path.join(scriptsDir, "migrate-1-to-2.sql"),
            )
            twoToThree = Migration(
                2,
                3,
                "migrate-2-to-3.sql",
                os.path.join(scriptsDir, "migrate-2-to-3.sql"),
            )
            makeFiles(scriptsDir, *[m.filepath for m in [noneToThree, noneToOne, oneToTwo, twoToThree]])

            mgr = TestMigMgr(Uninitialized, scriptsDir)
            mgr.migrate(to_version=3, do_migrate=True)
            mock.assert_called_once_with([noneToThree])
            mock.reset_mock()

            mgr = TestMigMgr(Uninitialized, scriptsDir)
            mgr.migrate(to_version=2, do_migrate=True)
            mock.assert_called_once_with([noneToOne, oneToTwo])
            mock.reset_mock()

            mgr = TestMigMgr(1, scriptsDir)
            mgr.migrate(to_version=3, do_migrate=True)
            mock.assert_called_once_with([oneToTwo, twoToThree])
            mock.reset_mock()

            # remove the short None-to-3:
            os.remove(noneToThree.filepath)
            # add a short 1-to-3
            oneToThree = Migration(
                1,
                3,
                "migrate-1-to-3.sql",
                os.path.join(scriptsDir, "migrate-1-to-3.sql"),
            )
            makeFiles(scriptsDir, oneToThree.filepath)
            # verify it skips 1-to-2 + 2-to-3, and uses 1-to-3
            mgr = TestMigMgr(Uninitialized, scriptsDir)
            mgr.migrate(to_version=3, do_migrate=True)
            mock.assert_called_once_with([noneToOne, oneToThree])
            mock.reset_mock()

            # remove the ways to proceed past one, and verify an error
            os.remove(oneToTwo.filepath)
            os.remove(oneToThree.filepath)
            mgr = TestMigMgr(Uninitialized, scriptsDir)
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
