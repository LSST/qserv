# This file is part of daf_butler.
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

import unittest

from lsst.qserv.admin.qservCli.qserv_log import (
    default_log_level,
    invalid_value_msg,
    log_level_from_args,
    log_level_choices,
    missing_argument_msg,
)


class QservLogTestCase(unittest.TestCase):
    """Tests features of the qserv_log helper function(s)."""

    def test_log_level_from_args(self):
        self.assertEqual(log_level_from_args(["--foo", "--log-level", "DEBUG", "--bar=baz"]), (True, "DEBUG"))
        self.assertEqual(log_level_from_args(["--log-level", "debug"]), (True, "DEBUG"))
        self.assertEqual(log_level_from_args(["--log-level=debug"]), (True, "DEBUG"))
        self.assertEqual(log_level_from_args(["--log-level=DEBUG"]), (True, "DEBUG"))
        self.assertEqual(log_level_from_args(["--log-level", "debugg"]), (False, invalid_value_msg))
        self.assertEqual(log_level_from_args(["--log-level=debugg"]), (False, invalid_value_msg))
        for choice in log_level_choices:
            self.assertEqual(log_level_from_args([f"--log-level={choice}"]), (True, choice))
            self.assertEqual(log_level_from_args([f"--log-level={choice.lower()}"]), (True, choice))
            self.assertEqual(log_level_from_args(["--log-level", choice]), (True, choice))
            self.assertEqual(log_level_from_args(["--log-level", choice.lower()]), (True, choice))
        self.assertEqual(log_level_from_args(["qserv", "up"]), (True, default_log_level))
        self.assertEqual(log_level_from_args(["--log-level"]), (False, missing_argument_msg))


if __name__ == "__main__":
    unittest.main()
