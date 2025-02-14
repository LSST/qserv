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

import lsst.qserv.master as qMaster


class AppTest(unittest.TestCase):
    """Tests... This is a catch-all for driving the query
    parse/generate/manipulate code.
    """

    def setUp(self):
        global _options
        pass

    def tearDown(self):
        pass

    def test_parse(self):
        query = " SELECT count(*) AS n, AVG(ra_PS), AVG(decl_PS), x_chunkId FROM Object GROUP BY x_chunkId;"

        qConfig = self._prepareConfig()
        sess = qMaster.newSession(qConfig)
        qMaster.setupQuery(sess, query)
        cvec = qMaster.getConstraints(1)

        def vecGen(constr):
            s = constr.paramsSize()
            for i in range(s):
                yield constr.paramsGet(i)
            pass

        def vecConGen(cvec):
            sz = cvec.size()
            for i in range(sz):
                c = cvec.get(i)
                yield c.name + "-->" + ",".join(vecGen(c))

        print("\n".join(vecConGen(cvec)))

        for i in range(3):
            self._addChunk(1, i)

        pass
        self.assertEqual(1, 1)

    def _addChunk(self, session, base):
        cs = qMaster.ChunkSpec()
        cs.chunkId = 1000 + base
        for i in range(base * 10, 10 + (base * 10)):
            cs.addSubChunk(i)
        qMaster.addChunk(session, cs)

    def _prepareConfig(self):
        qMaster.config.load()  # Init.
        cfg = qMaster.config.getStringMap()
        cfg["table.defaultdb"] = "ddb"
        cfg["query.hints"] = "box,0,0,5,1;circle,1,1,1;"
        return cfg


def main():
    global _options
    unittest.main()


if __name__ == "__main__":
    main()
