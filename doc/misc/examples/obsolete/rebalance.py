#!/usr/bin/env python

#
# LSST Data Management System
# Copyright 2008, 2009, 2010 LSST Corporation.
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
#

# rebalance.py
#
# Rebalance Object*_nnn.csv files among K directories.
# Motivation:
# Serge's partitioning script puts csv files in stripe_xxx
# directories.  We want to balance them among the nodes we are
# using. So, we redistribute files among node_xx directories.

# This reuses some (large?) portions from Serge's loader.py
import optparse
import os
from itertools import chain, imap, repeat
from textwrap import dedent

import loader  # Assume loader.py is co-located. :)


class RebalanceProgram:
    usage = dedent("""\
    usage: %prog [options] <path> [<path> [<path> ...]]

    Program that takes a bunch of Object*.csv files and rebalances
    them among a number of directories.

    <path>: A path where the program can find Object*.csv files.
    """)
    parser = optparse.OptionParser(usage)
    parser.add_option(
        "-d",
        "--dirs",
        dest="dirs",
        type="int",
        default=22,
        help=dedent("""\
        Number of directories among which to balance (default=%default)."""),
    )
    parser.add_option(
        "-p",
        "--prefix",
        dest="prefix",
        default="Object",
        help=dedent("""\
        Prefix of files to rebalance (default=%default)."""),
    )
    parser.add_option(
        "-t",
        "--target",
        dest="target",
        default="node",
        help=dedent("""\
        Target path prefix to which the files will be moved.
        (default=%default)."""),
    )

    def __init__(self):
        self._setup()

    def _setup(self):
        parser = RebalanceProgram.parser
        (opts, args) = parser.parse_args()
        if len(args) < 1:
            parser.error("Must specify at least one path. (How about . ?)")
        self._pathList = args
        self._opts = opts

    def _gather_chunk_files(self):
        chunk_files = chain(*imap(loader.find_chunk_files, self._pathList, repeat(self._opts.prefix)))
        self._chunkFiles = [x for x in chunk_files]

    def _check_gathering(self):
        check_dupe = set()
        for fl in self._chunkFiles:
            for f in fl:
                _, name = os.path.split(f)
                if name in check_dupe:
                    raise RuntimeError(f"Found 2 identically named chunk files named {name}")
                check_dupe.add(name)

    def _move(self):
        chunk_count = len(self._chunkFiles)
        nominal, extra = divmod(chunk_count, self._opts.dirs)
        print(chunk_count, "in dirs with at least", nominal)
        start = 0

        for i in range(self._opts.dirs):
            length = nominal
            if extra:
                length += 1
                extra -= 1
            stop = start + length
            # print i, "with", stop-start,"files"
            if stop > chunk_count:
                stop = chunk_count
            targetdir = self._opts.target + str(i)
            try:
                os.listdir(targetdir)
            except OSError:
                os.makedirs(targetdir)

            for j in range(start, stop):
                for f in self._chunkFiles[j]:
                    _, name = os.path.split(f)
                    print("rename ", f, os.path.join(targetdir, name))
                    os.rename(f, os.path.join(targetdir, name))
            start = stop

    def run(self):
        self._gather_chunk_files()
        self._check_gathering()
        self._move()


if __name__ == "__main__":
    p = RebalanceProgram()
    p.run()
