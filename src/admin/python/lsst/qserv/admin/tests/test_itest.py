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

"""Unit tests for test_watcher."""

import unittest

from lsst.qserv.admin import itest


class ITestTestCase(unittest.TestCase):
    """Tests the itest module."""

    def test_get_pragmas(self):
        """Verify that the _get_pragmas function extracts pragmas from the query
        file text.

        Check that
        1. The lines not starting with "-- pragma" are ignored
        2. The default value where the pragma does not have "=" or a second value is `None`
        3. And empty pragma is ingored
        4. if the pragma key has an equal sign it is split into a kv pair, and
           further items are a separate pragma.
        """
        querytext = """-- Tests that the having clause is handled properly
-- pragma sortresult
-- pragma
-- pragma foo=bar
-- pragma baz=boz qux
-- pragma quux garply

SELECT objectId,
    MAX(raFlux) - MIN(raFlux)
FROM Source
GROUP BY objectId
HAVING MAX(raFlux)-MIN(raFlux) > 5;
        """
        self.assertEqual(
            itest.ITestQuery._get_pragmas(querytext),
            {
                "sortresult": None,
                "foo": "bar",
                "baz": "boz",
                "qux": None,
                "quux": None,
                "garply": None,
            },
        )


if __name__ == "__main__":
    unittest.main()
