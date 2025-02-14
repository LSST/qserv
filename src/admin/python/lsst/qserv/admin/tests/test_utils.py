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

"""Unit tests for the utils module."""

import os
import unittest
from tempfile import NamedTemporaryFile
from typing import Dict

import click
import yaml
from click.decorators import pass_context
from click.testing import CliRunner
from lsst.qserv.admin.cli import utils
from lsst.qserv.admin.cli.options import option_targs_file, options_targs


class SplitKvTestCase(unittest.TestCase):
    def test_split_kv(self):
        self.assertEqual(utils.split_kv([]), dict())
        self.assertEqual(utils.split_kv(["a=1"]), dict(a="1"))
        self.assertEqual(utils.split_kv(["a=1,b=2"]), dict(a="1", b="2"))
        self.assertEqual(utils.split_kv(["a=1,b=2", "c=3"]), dict(a="1", b="2", c="3"))

    def test_invalid_input(self):
        with self.assertRaises(RuntimeError):
            utils.split_kv(["a"])
        with self.assertRaises(RuntimeError):
            utils.split_kv(["a=b=c"])


targs_result = None


@click.command()
@pass_context
@click.option("--test-option1")
@click.option("--test-option2")
@click.option("--test-option3")
@options_targs()
@option_targs_file()
def testFunc(
    ctx: click.Context,
    test_option1: str,
    test_option2: str,
    test_option3: str,
    targs: Dict[str, str],
    targs_file: str,
) -> None:
    global targs_result
    targs_result = utils.targs(ctx)


class CliTargsTestCase(unittest.TestCase):
    """Tests for entrypoint CLI template arguments.

    Value priority, highest to lowest is:
    --targs, --targs-file, "well-known" click option arguments, environment variables.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.maxDiff = None

    def test_minimalValues(self):
        """Test utils.targs without setting very many values."""
        global targs_result
        targs_result = None
        runner = CliRunner()
        res = runner.invoke(testFunc)
        self.assertEqual(res.exit_code, 0, utils.clickResultMsg(res))
        expected = dict(os.environ)
        expected.update(dict(test_option1=None, test_option2=None, test_option3=None))
        self.assertEqual(targs_result, expected)

    def test_targ_overrides(self):
        """Test utils.targs, set values at each level and look for value overrides."""
        global targs_result
        targs_result = None
        runner = CliRunner()
        with NamedTemporaryFile("w") as f:
            yaml.dump({"test_option1": "abcdef", "test_option2": "ghijk"}, f)
            res = runner.invoke(
                testFunc,
                [
                    "--test-option1",
                    "foo",
                    "--test-option2",
                    "bar",
                    "--test-option3",
                    "baz",
                    "--targs",
                    "test_option1=1234",
                    "--targs-file",
                    f.name,
                ],
                env={"test_option3": "EnvCantGetNoRespect", "another_var_4": "thisOneGetsNoticed"},
            )
        self.assertEqual(res.exit_code, 0, utils.clickResultMsg(res))
        expected = dict(os.environ)
        # test_option1 is set by an option, overridden by the targs file and by targs; targs wins.
        # test_option2 is set by an option and overridden by the targs file, targs file wins.
        # test_option3 is set by an environment variable and an option, the option wins.
        # another_var_4 is set in the environment, and wins.
        expected.update(
            dict(
                test_option1="1234",
                test_option2="ghijk",
                test_option3="baz",
                another_var_4="thisOneGetsNoticed",
            )
        )
        self.assertEqual(targs_result, expected)

    def test_targ_split(self):
        """verify that --targs allows multiple calls, each containing exactly
        one key-value pair."""
        global targs_result
        runner = CliRunner()
        res = runner.invoke(
            testFunc,
            [
                "--test-option1",
                "foo",
                "--test-option2",
                "bar",
                "--targs",
                "test_option1=beans",
                "--targs",
                "test_option2=cheese,guac",
            ],
        )
        self.assertEqual(res.exit_code, 0, utils.clickResultMsg(res))
        expected = dict(os.environ)
        expected.update(dict(test_option1="beans", test_option2=["cheese", "guac"], test_option3=None))
        self.assertEqual(targs_result, expected)

        # more than one equal sign should fail
        res = runner.invoke(testFunc, "--targs", "test_option1=one=two")
        self.assertNotEqual(res.exit_code, 0, utils.clickResultMsg(res))

        # zero equal signs should fail
        res = runner.invoke(testFunc, "--targs", "test_option1")
        self.assertNotEqual(res.exit_code, 0, utils.clickResultMsg(res))


if __name__ == "__main__":
    unittest.main()
