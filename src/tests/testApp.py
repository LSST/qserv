#! /usr/bin/env python
# LSST Data Management System
# Copyright 2009-2013 LSST Corporation.
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
"""
This is an out-of-date test harness for the app.py functions in qserv/master.
"""

import unittest

import lsst.qserv.master as q_master


class AppTest(unittest.TestCase):
    """Tests... This is a catch-all for driving the query
    parse/generate/manipulate code.
    """

    def setUp(self):
        global _options

    def tearDown(self):
        pass

    def test_parse(self):
        query = " SELECT count(*) AS n, AVG(ra_PS), AVG(decl_PS), x_chunkId FROM Object GROUP BY x_chunkId;"

        q_config = self._prepare_config()
        sess = q_master.newSession(q_config)
        q_master.setupQuery(sess, query)
        cvec = q_master.getConstraints(1)

        def vec_gen(constr):
            s = constr.paramsSize()
            for i in range(s):
                yield constr.paramsGet(i)

        def vec_con_gen(cvec):
            sz = cvec.size()
            for i in range(sz):
                c = cvec.get(i)
                yield c.name + "-->" + ",".join(vec_gen(c))

        print("\n".join(vec_con_gen(cvec)))

        for i in range(3):
            self._add_chunk(1, i)

        self.assertEqual(1, 1)

    def _add_chunk(self, session, base):
        cs = q_master.ChunkSpec()
        cs.chunkId = 1000 + base
        for i in range(base * 10, 10 + (base * 10)):
            cs.addSubChunk(i)
        q_master.addChunk(session, cs)

    def _prepare_config(self):
        q_master.config.load()  # Init.
        cfg = q_master.config.getStringMap()
        cfg["table.defaultdb"] = "ddb"
        cfg["query.hints"] = "box,0,0,5,1;circle,1,1,1;"
        return cfg


def main():
    global _options
    unittest.main()


if __name__ == "__main__":
    main()
