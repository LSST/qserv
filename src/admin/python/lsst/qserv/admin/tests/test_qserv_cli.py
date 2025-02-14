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

"""Unit tests for qserv_cli."""

from click.testing import CliRunner
import os
import unittest
from unittest.mock import patch, ANY

from lsst.qserv.admin.qservCli import launch
from lsst.qserv.admin.qservCli.qserv_cli import qserv


def env_without_qserv(**kwargs):
    """Get the current environment variables with any QSERV environment
    variables removed.

    Parameters
    ----------
    **kwargs : Any
        keyword argumets to override existing environment variables, or add new
        variables if applicable.

    Returns
    -------
    environment_variables : `dict`
        A dict of environment variables that can be used with CliRunner().invoke()
    """
    vars = {k: v for k, v in os.environ.items() if not k.startswith("QSERV_")}
    vars.update(kwargs)
    return vars


def build_args(**kwargs):
    """Get a safely-mutable dict of the launch.build kwargs, with all the values set to ANY,
    for use with mock.assert_called_with().

    Parameters
    ----------
    **kwargs : dict [`str`: `Any`]
        keyword argumets to override existing argments, or add new arguments if
        applicable.

    Returns
    -------
    arguments : `dict` [`str` : Union[`unittest.mock.ANY`, `Any`]]
        A dict of arguments that can be changed with expected values.
    """
    args = dict(
        qserv_root=ANY,
        qserv_build_root=ANY,
        unit_test=ANY,
        dry=ANY,
        jobs=ANY,
        run_cmake=ANY,
        run_make=ANY,
        run_mypy=ANY,
        user_build_image=ANY,
        qserv_image=ANY,
        run_base_image=ANY,
        do_build_image=ANY,
        pull_image=ANY,
        push_image=ANY,
        user=ANY,
        clang_format_mode=ANY,
    )
    args.update(kwargs)
    return args


def itest_args(**kwargs):
    """Get a safely-mutable dict of the launch.launch kwargs, with all the values set to ANY,
    for use with mock.assert_called_with().

    Parameters
    ----------
    **kwargs : dict [`str`: `Any`]
        keyword argumets to override existing argments, or add new arguments if
        applicable.

    Returns
    -------
    arguments : `dict` [`str` : Union[`unittest.mock.ANY`, `Any`]]
        A dict of arguments that can be changed with expected values.
    """
    args = dict(
        qserv_root=ANY,
        mariadb_image=ANY,
        itest_container=ANY,
        itest_ref_container=ANY,
        qserv_image=ANY,
        bind=ANY,
        itest_file=ANY,
        dry=ANY,
        project=ANY,
        unload=ANY,
        load=ANY,
        reload=ANY,
        load_http=ANY,
        cases=ANY,
        run_tests=ANY,
        tests_yaml=ANY,
        compare_results=ANY,
        wait=ANY,
        remove=ANY,
    )
    args.update(kwargs)
    return args


def prepare_data_args(**kwargs):
    """Get a safely-mutable dict of the launch.launch kwargs, with all the values set to ANY,
    for use with mock.assert_called_with().

    Parameters
    ----------
    **kwargs : dict [`str`: `Any`]
        keyword argumets to override existing argments, or add new arguments if
        applicable.

    Returns
    -------
    arguments : `dict` [`str` : Union[`unittest.mock.ANY`, `Any`]]
        A dict of arguments that can be changed with expected values.
    """
    args = dict(
        qserv_root=ANY,
        itest_container=ANY,
        qserv_image=ANY,
        itest_file=ANY,
        outdir=ANY,
        dry=ANY,
        project=ANY,
    )
    args.update(kwargs)
    return args


class QservCliTestCase(unittest.TestCase):
    """Tests features of the qserv command line interface."""

    def setUp(self):
        self.runner = CliRunner()

    @patch.object(launch, "build")
    def test_EnvVal_var(self, build_mock):
        """Verify that an `opt.FlagEnvVal` option can be set using the environment
        variable.
        """
        fake_root = "/foo/bar/qserv"
        res = self.runner.invoke(qserv, ["build"], env=env_without_qserv(QSERV_ROOT=fake_root))
        self.assertEqual(res.exit_code, 0)
        build_mock.assert_called_once()
        build_mock.assert_called_with(**build_args(qserv_root=fake_root))

    @patch.object(launch, "build")
    def test_EnvVal_flag(self, build_mock):
        """Verify that an `opt.FlagEnvVal` option can be set using the flag and it
        overrides the environment variable.
        """
        fake_root = "/foo/bar/qserv"
        flag_root = "/path/to/qserv"
        res = self.runner.invoke(
            qserv,
            ["build", "--qserv-root", flag_root],
            env=env_without_qserv(QSERV_ROOT=fake_root),
        )
        self.assertEqual(res.exit_code, 0)
        build_mock.assert_called_once()
        build_mock.assert_called_with(**build_args(qserv_root=flag_root))

    @patch.object(launch, "itest", return_value=0)
    def test_itest_OptDefault_default(self, itest_mock):
        """Verify that an `opt.OptDefault` option value can be inferred from its
        associated environment variable.
        """
        res = self.runner.invoke(qserv, ["itest"], env=env_without_qserv())
        self.assertEqual(res.exit_code, 0)
        itest_mock.assert_called_once()
        expected = os.path.abspath(
            # This is the location of the path *inside* the build container,
            # because that's where the unit tests run. This is from the install
            # location of the unit tests back up the tree to the root qserv
            # folder.
            os.path.join(__file__, "../../../../../../../..")
        )
        itest_mock.assert_called_with(**itest_args(qserv_root=expected))

    @patch.object(launch, "prepare_data", return_value=0)
    def test_prepare_data_OptDefault_default(self, prepare_data_mock):
        """Verify that an `opt.OptDefault` option value can be inferred from its
        associated environment variable.
        """
        res = self.runner.invoke(qserv, ["prepare-data"], env=env_without_qserv())
        self.assertEqual(res.exit_code, 0)
        prepare_data_mock.assert_called_once()
        expected = os.path.abspath(
            # This is the location of the path *inside* the build container,
            # because that's where the unit tests run. This is from the install
            # location of the unit tests back up the tree to the root qserv
            # folder.
            os.path.join(__file__, "../../../../../../../..")
        )
        prepare_data_mock.assert_called_with(**prepare_data_args(qserv_root=expected))

    def test_env(self):
        """Test that `qserv env` runs without throwing.

        Does not check for correctness of course, but at least checks that
        values can be gotten and reported.
        """
        res = self.runner.invoke(qserv, ["env"])
        self.assertEqual(res.exit_code, 0)


if __name__ == "__main__":
    unittest.main()
