#!/usr/bin/env python

# LSST Data Management System
# Copyright 2017 AURA/LSST.
#
# This product includes software developed by the
# LSST Project (http://www.lsst.org/).
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
# You should have received a copy of the LSST License Statement and
# the GNU General Public License along with this program.  If not,
# see <http://www.lsstcorp.org/LegalNotices/>.

"""Application which implements migration process for qserv databases."""

import argparse
import os
import sys

from lsst.qserv.schema import smig

_def_scripts = os.path.join(os.environ.get("QSERV_DIR", ""), "share/qserv/schema")


def main():
    parser = argparse.ArgumentParser(description="Qserv database schema migration.")

    parser.add_argument(
        "-v", "--verbose", default=0, action="count", help="Use one -v for INFO logging, two for DEBUG."
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "-m",
        "--do-migrate",
        default=False,
        action="store_true",
        help="Do migration, without this option script prints various info and exits.",
    )
    group.add_argument(
        "--check",
        default=False,
        action="store_true",
        help="Check that migration is needed, script returns 0 if schema is up-to-date, 1 otherwise.",
    )
    parser.add_argument(
        "-n",
        "--final",
        default=None,
        action="store",
        type=int,
        metavar="VERSION",
        help="Stop migration at given version, by default update to latest version.",
    )
    parser.add_argument(
        "--scripts",
        default=_def_scripts,
        action="store",
        metavar="PATH",
        help="Location for migration scripts, def: %(default)s.",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "-c",
        "--connection",
        metavar="CONNECTION",
        help="Connection string in format mysql://user:pass@host:port/database.",
    )
    group.add_argument(
        "-f",
        "--config-file",
        metavar="PATH",
        help="Name of configuration file in INI format with connection parameters.",
    )
    parser.add_argument(
        "-s", "--config-section", metavar="NAME", help="Name of configuration section in configuration file."
    )

    parser.add_argument("module", help="Name of Qserv module for which to update schema, e.g. qmeta.")

    args = parser.parse_args()
    smig(**args)


if __name__ == "__main__":
    sys.exit(main())
