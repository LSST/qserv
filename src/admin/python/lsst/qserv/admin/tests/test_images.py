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

"""Unit tests for the images module."""

import unittest
from unittest.mock import patch

import images

# from images import ImageTagger


class GetMostRecentTestCase(unittest.TestCase):
    # Our git_log function calls `git log a..b`, so if b is newer than a it will
    # return a list of shas, and if a is newer then it will return nothing.
    # Simulate that here by treating a and b alphabeticlly.
    @patch.object(images, "git_log", lambda a, b: [a, b] if a < b else [])
    def test_get_newest(self):
        """Test the algorithm for getting the newest sha from a list of shas."""
        self.assertEqual(images.get_most_recent(["abc"]), "abc")
        self.assertEqual(images.get_most_recent(["abc", "def"]), "def")
        self.assertEqual(images.get_most_recent(["def", "abc"]), "def")
        self.assertEqual(images.get_most_recent(["abc", "abc"]), "abc")
        self.assertEqual(images.get_most_recent(["def", "bcd", "abc"]), "def")
        self.assertEqual(images.get_most_recent(["abc", "def", "bcd"]), "def")


if __name__ == "__main__":
    unittest.main()
