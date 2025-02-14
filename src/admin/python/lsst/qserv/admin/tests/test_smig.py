# This file is part of qserv.
#
# Developed for the LSST Data Management System.
# This product includes software developed by the LSST Project
# (http://www.lsst.org).
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
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""Unit tests for smig."""

import os
import unittest
from tempfile import TemporaryDirectory
from unittest.mock import patch

from lsst.qserv.admin.cli import script
from lsst.qserv.qmeta.schema_migration import QMetaMigrationManager
from lsst.qserv.schema import SchemaMigMgr, SchemaUpdateRequiredError, Uninitialized

migration_files = [
    "migrate-0-to-1.sql",
    "migrate-1-to-2.sql",
    "migrate-2-to-3.sql",
    "migrate-3-to-4.sql",
    "migrate-None-to-5.sql.jinja",
    "migrate-4-to-5.sql.jinja",
]

# This number must match the highest 'to' number in migration_files.
latest_qmeta_schema_version = 5
# This number must match the second highest 'to' number in migration_files.
previous_qmeta_schema_version = 4
# This is the file that migrates from Uninitialized to latest
qmeta_migrate_uninit_to_newest = migration_files[4]
# This is the file that migrates from previous to latest
qmeta_migrate_previous_to_newest = migration_files[5]


class SmigTestCase(unittest.TestCase):
    """Tests smig init."""

    def setUp(self):
        # Create a temporary directory and write files there that are named like
        # smig migration files. Later, tests will patch the smig dir environment
        # variable so those files are found by the qmeta migration lookup
        # (instead of the real migration files.)
        self.tempdir = TemporaryDirectory()
        self.qserv_smig_dir = self.tempdir.name
        folder = os.path.join(self.qserv_smig_dir, script.qmeta_smig_dir)
        os.makedirs(folder, exist_ok=True)
        for fname in migration_files:
            with open(os.path.join(folder, fname), "w") as f:
                f.write("foo")

    def tearDown(self):
        self.tempdir.cleanup()

    @patch.object(QMetaMigrationManager, "current_version", return_value=Uninitialized)
    @patch.object(QMetaMigrationManager, "apply_migrations", return_value=latest_qmeta_schema_version)
    @patch.object(SchemaMigMgr, "_connect", return_value=None)
    def test_proxy_init(self, connect_mock, apply_migrations_mock, current_version_mock):
        """Test using smig to initialize a new qserv instance.

        Mocks the qmeta "current version" and smig execution functions to verify
        that if a module's current version is Uninitialized that smig *does*
        update the database to the newest schema.
        """
        with patch.dict(os.environ, {script.smig_dir_env_var: self.qserv_smig_dir}):
            script._do_smig(
                module_smig_dir=script.qmeta_smig_dir,
                module="qmeta",
                connection="fake-connection-uri",
                update=False,
            )
        current_version_mock.assert_called()
        apply_migrations_mock.assert_called_once()
        # the 0 indexes are the first (only) arg, the first (only) time the mock was called.
        migration = apply_migrations_mock.call_args.args[0][0]
        self.assertEqual(migration.from_version, Uninitialized)
        self.assertEqual(migration.to_version, latest_qmeta_schema_version)
        self.assertEqual(migration.name, qmeta_migrate_uninit_to_newest)
        # Don't bother comparing the migration path, it assumes a certain install environment.

    @patch.object(QMetaMigrationManager, "current_version", return_value=1)
    @patch.object(SchemaMigMgr, "_connect", return_value=None)
    def test_proxy_needs_upgrade(self, connect_mock, current_version_mock):
        """Test that smig causes the entrypoint script to abort if a schema upgrade is needed.

        Mocks the qmeta "current version" function to verify that if the a
        module's current version is not Uninitialzed that the applciation raises
        an error and exits without running smig.
        """
        # UNIT_TEST prevents the backoff function from handling the
        # SchemaUpdateRequiredError exception and going into an infinite loop waiting
        # for someone else to upgrade the schema.
        with patch.dict(os.environ, {"UNIT_TEST": "1", script.smig_dir_env_var: self.qserv_smig_dir}):
            with self.assertRaises(SchemaUpdateRequiredError):
                script._do_smig(
                    module_smig_dir=script.qmeta_smig_dir,
                    module="qmeta",
                    connection="fake-connection-uri",
                    update=False,
                )
        current_version_mock.assert_called()

    @patch.object(QMetaMigrationManager, "current_version", return_value=latest_qmeta_schema_version)
    @patch.object(QMetaMigrationManager, "apply_migrations", return_value=latest_qmeta_schema_version)
    @patch.object(SchemaMigMgr, "_connect", return_value=None)
    def test_proxy_does_not_need_upgrade(self, connect_mock, apply_migrations_mock, current_version_mock):
        """Test that smig identifies that an upgrade is not needed and does not
        call the apply migrations function.
        """
        with patch.dict(os.environ, {script.smig_dir_env_var: self.qserv_smig_dir}):
            script._do_smig(
                module_smig_dir=script.qmeta_smig_dir,
                module="qmeta",
                connection="fake-connection-uri",
                update=True,
            )
        current_version_mock.assert_called()
        apply_migrations_mock.assert_not_called()

    @patch.object(QMetaMigrationManager, "current_version", return_value=previous_qmeta_schema_version)
    @patch.object(QMetaMigrationManager, "apply_migrations", return_value=latest_qmeta_schema_version)
    @patch.object(SchemaMigMgr, "_connect", return_value=None)
    def test_upgrade_proxy(self, connect_mock, apply_migrations_mock, current_version_mock):
        """Tests that the proxy is upgraded when the upgrade flags are passed."""
        with patch.dict(os.environ, {script.smig_dir_env_var: self.qserv_smig_dir}):
            script._do_smig(
                module_smig_dir=script.qmeta_smig_dir,
                module="qmeta",
                connection="fake-connection-uri",
                update=True,
            )
        current_version_mock.assert_called()
        apply_migrations_mock.assert_called_once()
        # the 0 indexes are the first (only) arg, the first (only) time the mock was called.
        migration = apply_migrations_mock.call_args.args[0][0]
        self.assertEqual(migration.from_version, previous_qmeta_schema_version)
        self.assertEqual(migration.to_version, latest_qmeta_schema_version)
        self.assertEqual(migration.name, qmeta_migrate_previous_to_newest)
        # Don't bother comparing the migration path, it assumes a certain install environment.


if __name__ == "__main__":
    unittest.main()
