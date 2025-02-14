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


"""Unit tests for the option_options_file CLI option."""

import tempfile
import unittest
from unittest.mock import MagicMock

import click
import yaml
from click.testing import CliRunner
from lsst.qserv.admin.cli.options import option_options_file

defaultStrFoo = "bar"
mock = MagicMock()


@click.command()
@click.option("--foo", default=defaultStrFoo)
@option_options_file()
def cmd_str(foo):
    mock(foo)


@click.command()
@click.option("--foo", type=int)
@option_options_file()
def cmd_int(foo):
    mock(foo)


@click.command()
@click.option("--foo", type=bool)
@option_options_file()
def cmd_bool(foo):
    mock(foo)


class OptionsFileOptionTestCase(unittest.TestCase):
    def setUp(self) -> None:
        mock.reset_mock()

    def test_passedVal(self):
        runner = CliRunner()
        runner.invoke(cmd_str, ["--foo", val := "abc123"])
        mock.assert_called_once_with(val)

    def test_defaultVal(self):
        runner = CliRunner()
        runner.invoke(cmd_str)
        mock.assert_called_once_with(defaultStrFoo)

    def test_fileOverrideStr(self):
        for cmd, cmd_name, flag, val, options_file_flag in (
            (cmd_str, "cmd-str", "foo", "baz", "-@"),
            (cmd_int, "cmd-int", "foo", 42, "--options-file"),
            (cmd_bool, "cmd-bool", "foo", True, "-@"),
        ):
            mock.reset_mock()
            with tempfile.NamedTemporaryFile() as options_file:
                with open(options_file.name, "w") as f:
                    f.write(yaml.dump({cmd_name: {flag: val}}))
                runner = CliRunner()
                runner.invoke(cmd, [options_file_flag, options_file.name])
                mock.assert_called_once_with(val)


if __name__ == "__main__":
    unittest.main()
