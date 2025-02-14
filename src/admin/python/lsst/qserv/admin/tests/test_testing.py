#!/usr/bin/env python

# LSST Data Management System
# Copyright 2020 SLAC.
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

"""This is a unittest for admin.testing package"""

import io
import logging
import os
import tempfile
import unittest

from lsst.qserv.testing import config, mock_db, monitor, query_runner, runner_mgr


logging.basicConfig(level=logging.INFO)

_CFG1 = """
queryClasses:
  LV:
    concurrentQueries: 80
    targetTime: 10s
    maxRate: 100
    arraysize: 1000
  FTSObj:
    concurrentQueries: 12
    targetTimeSec: 1h
    maxRate: null
queries:
  LV:
    q1:
      template: >
        SELECT ra, decl, raVar, declVar, radeclCov, u_psfFlux, u_psfFluxSigma, u_apFlux
        FROM Object
        WHERE deepSourceId = {objectId}
      variables:
        objectId:
          path: /dev/null
          mode: random
    q2:
      template: >
        SELECT ra, decl, raVar, declVar, radeclCov, u_psfFlux, u_psfFluxSigma, u_apFlux
        FROM Object
        WHERE qserv_areaspec_box({raMin}, {declMin}, {raMin}+{raDist}, {declMin}+{declDist})
      variables:
        raMin: {distribution: uniform, min: 0, max: 350}
        declMin: {distribution: uniform, min: -87, max: 45}
        raDist: {distribution: uniform, min: 0.01, max: 0.2}
        declDist: {distribution: uniform, min: 0.01, max: 0.2}
  FTSObj:
    q1: "SELECT COUNT(*) FROM Object WHERE y_instFlux > 0.05"
    q2: "SELECT ra, decl, u_psfFlux, g_psfFlux, r_psfFlux FROM Object WHERE y_shapeIxx BETWEEN 20 AND 40"
    q3: "SELECT COUNT(*) FROM Object WHERE y_instFlux > u_instFlux"
"""

_CFG2 = """
queryClasses:
  LV:
    concurrentQueries: 100
  FTSObj:
    maxRate: 1
"""


class TestConfig(unittest.TestCase):
    def test_merge(self):
        cfg1 = {"a": 1, "b": 2, "c": {"d": 3, "0": 0}}
        cfg2 = {"a": 3, "e": 4, "c": {"0": [], "x": None}}
        expect = {"a": 3, "b": 2, "e": 4, "c": {"d": 3, "x": None, "0": []}}
        cfg = config.Config._merge(cfg1, cfg2)
        self.assertEqual(cfg, expect)

    def test_construct_empty(self):
        with self.assertRaises(ValueError):
            config.Config([])

    def test_construct_yaml_one(self):
        # construct with single YAML
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])
        self.assertEqual(set(cfg._config.keys()), {"queryClasses", "queries"})
        self.assertEqual(set(cfg._config["queryClasses"].keys()), {"LV", "FTSObj"})
        self.assertEqual(set(cfg._config["queries"].keys()), {"LV", "FTSObj"})
        self.assertEqual(set(cfg._queries.keys()), {"LV", "FTSObj"})

        self.assertEqual(cfg.classes(), {"LV", "FTSObj"})
        self.assertEqual(len(cfg.queries("LV")), 2)
        self.assertEqual(len(cfg.queries("FTSObj")), 3)
        self.assertEqual(cfg.concurrentQueries("LV"), 80)
        self.assertEqual(cfg.concurrentQueries("FTSObj"), 12)
        self.assertEqual(cfg.maxRate("LV"), 100)
        self.assertIsNone(cfg.maxRate("FTSObj"))

    def test_construct_yaml_two(self):
        # construct with overrides
        cfg = config.Config.from_yaml([io.StringIO(_CFG1), io.StringIO(_CFG2)])
        self.assertEqual(set(cfg._config.keys()), {"queryClasses", "queries"})
        self.assertEqual(set(cfg._config["queryClasses"].keys()), {"LV", "FTSObj"})
        self.assertEqual(set(cfg._config["queries"].keys()), {"LV", "FTSObj"})
        self.assertEqual(set(cfg._queries.keys()), {"LV", "FTSObj"})

        self.assertEqual(cfg.classes(), {"LV", "FTSObj"})
        self.assertEqual(len(cfg.queries("LV")), 2)
        self.assertEqual(len(cfg.queries("FTSObj")), 3)
        self.assertEqual(cfg.concurrentQueries("LV"), 100)
        self.assertEqual(cfg.concurrentQueries("FTSObj"), 12)
        self.assertEqual(cfg.maxRate("LV"), 100)
        self.assertEqual(cfg.maxRate("FTSObj"), 1)

    def test_split(self):
        # construct with single YAML
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])

        self.assertEqual(cfg.concurrentQueries("LV"), 80)
        self.assertEqual(cfg.concurrentQueries("FTSObj"), 12)
        self.assertEqual(cfg.maxRate("LV"), 100)
        self.assertIsNone(cfg.maxRate("FTSObj"))

        cfg0 = cfg.split(3, 0)
        cfg1 = cfg.split(3, 1)
        cfg2 = cfg.split(3, 2)
        self.assertEqual(cfg0.concurrentQueries("LV"), 27)
        self.assertEqual(cfg1.concurrentQueries("LV"), 27)
        self.assertEqual(cfg2.concurrentQueries("LV"), 26)
        self.assertEqual(cfg0.maxRate("LV"), 100.0)
        self.assertEqual(cfg1.maxRate("LV"), 100.0)
        self.assertEqual(cfg2.maxRate("LV"), 100.0)
        self.assertEqual(cfg0.concurrentQueries("FTSObj"), 4)
        self.assertEqual(cfg1.concurrentQueries("FTSObj"), 4)
        self.assertEqual(cfg2.concurrentQueries("FTSObj"), 4)
        self.assertIsNone(cfg0.maxRate("FTSObj"))
        self.assertIsNone(cfg1.maxRate("FTSObj"))
        self.assertIsNone(cfg2.maxRate("FTSObj"))

    def test_ValueRandomUniform(self):
        gen = config._ValueRandomUniform(1.0, 42.0)
        for i in range(100):
            val = gen()
            self.assertGreaterEqual(val, 1.0)
            self.assertLessEqual(val, 42.0)

    def test_ValueIntFromFile(self):
        with self.assertRaises(AssertionError):
            # bad mode
            config._ValueIntFromFile("/dev/null", "non-random")

        # /dev/null makes all zeros
        gen = config._ValueIntFromFile("/dev/null")
        values = set(gen() for i in range(100))
        self.assertEqual(values, {0})

        with self.assertRaises(FileNotFoundError):
            config._ValueIntFromFile("/def/none")

        # make file with some data
        with tempfile.TemporaryDirectory() as tmpdir:
            fname = os.path.join(tmpdir, "data.txt")
            with open(fname, "w") as f:
                f.write("1 10\n 20\t 30\n42\n")

            # random mode
            gen = config._ValueIntFromFile(fname)
            values = set(gen() for i in range(100))

            for val in values:
                self.assertIn(val, {1, 10, 20, 30, 42})

            # sequential mode
            gen = config._ValueIntFromFile(fname, "sequential")
            values = [gen() for i in range(10)]
            self.assertEqual(values, [1, 10, 20, 30, 42, 1, 10, 20, 30, 42])

    def test_QueryFactory(self):
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])

        for qclass in cfg.classes():
            queries = cfg.queries(qclass)
            for qid, factory in queries.items():
                for i in range(10):
                    query = factory.query()
                    self.assertIsInstance(query, str)


class TestMockDb(unittest.TestCase):
    def test_mock_db_fetchall(self):
        # can take any args
        conn = mock_db.connect(host="host", port=4040, user="qsmaster", passwd="", db="LSST")

        cursor = conn.cursor()

        query = "SELECT * from Object"
        cursor.execute(query)
        rows = cursor.fetchall()
        self.assertEqual(len(rows), 2)

        query = "SELECT * from Object where O = 5"
        cursor.execute(query)
        rows = cursor.fetchall()
        self.assertEqual(len(rows), 5)

        query = "SELECT * from Ob22ject"
        cursor.execute(query)
        rows = cursor.fetchall()
        self.assertEqual(len(rows), 22)

    def test_mock_db_fetchmany(self):
        # can take any args
        conn = mock_db.connect(host="host", port=4040, user="qsmaster", passwd="", db="LSST")

        cursor = conn.cursor()

        query = "SELECT * from Object"
        cursor.execute(query)
        rows = cursor.fetchmany()
        self.assertEqual(len(rows), 1)
        rows = cursor.fetchmany()
        self.assertEqual(len(rows), 1)
        rows = cursor.fetchmany()
        self.assertEqual(len(rows), 0)

        query = "SELECT * from Object where O = 5"
        cursor.execute(query)
        rows = cursor.fetchmany(4)
        self.assertEqual(len(rows), 4)
        rows = cursor.fetchmany(4)
        self.assertEqual(len(rows), 1)
        rows = cursor.fetchmany(4)
        self.assertEqual(len(rows), 0)

        query = "SELECT * from Ob22ject"
        cursor.execute(query)
        rows = cursor.fetchmany(1000)
        self.assertEqual(len(rows), 22)
        rows = cursor.fetchmany(1000)
        self.assertEqual(len(rows), 0)


class TestQueryRunner(unittest.TestCase):
    def test_query_runner_count(self):
        """Test for queryCountLimit parameter.

        The test is for that runner eventually stops, there are no usual
        asserts below.
        """
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])
        queries = cfg.queries("LV")
        monit = monitor.LogMonitor("testmonit")

        connectionFactory = mock_db.connect
        runner = query_runner.QueryRunner(
            queries=queries,
            maxRate=None,
            connectionFactory=connectionFactory,
            runnerId="runner1",
            arraysize=1000,
            queryCountLimit=10,
            runTimeLimit=None,
            monitor=monit,
        )
        runner()

    def test_query_runner_time(self):
        """Test for runTimeLimit parameter.

        The test is for that runner eventually stops, there are no usual
        asserts below.
        """
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])
        queries = cfg.queries("FTSObj")

        connectionFactory = mock_db.connect
        runner = query_runner.QueryRunner(
            queries=queries,
            maxRate=None,
            connectionFactory=connectionFactory,
            runnerId="runner1",
            arraysize=1000,
            queryCountLimit=None,
            runTimeLimit=1.5,
            monitor=None,
        )
        runner()


class TestRunnerManager(unittest.TestCase):
    def test_slot_none(self):
        """Test for RunnerManager execution with slot=None.

        The test is for that runner eventually stops due to run time limit,
        there are no usual asserts below but looking at the log output may
        be useful.
        """
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])
        connectionFactory = mock_db.connect
        slot = None
        mgr = runner_mgr.RunnerManager(cfg, connectionFactory, slot, runTimeLimit=1.5, monitor=None)
        mgr.run()

    def test_slot_7(self):
        """Test for RunnerManager execution with slot=7.

        The test is for that runner eventually stops due to run time limit,
        there are no usual asserts below but looking at the log output may
        be useful (it should include "slot=7").
        """
        cfg = config.Config.from_yaml([io.StringIO(_CFG1)])
        connectionFactory = mock_db.connect
        slot = 7
        mgr = runner_mgr.RunnerManager(cfg, connectionFactory, slot, runTimeLimit=1.5, monitor=None)
        mgr.run()


class TestMonitor(unittest.TestCase):
    def test_monitor_influxdb_file(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            fname = os.path.join(tmpdir, "monit1-%T.dat")
            monit = monitor.InfluxDBFileMonitor(fname, None)

            monit.add_metrics("metrics1", value=1)
            monit.add_metrics("metrics2", value1=1, value2=2, tags={"tag1": "tag", "tag2": 100})

            path = monit.path
            self.assertTrue(path.endswith(".dat"))

            monit.close()

            with open(path) as f:
                data = f.read().split("\n")
                self.assertEqual(len(data), 4)
                self.assertEqual(data[0], "# DML")
                # ignores timestamps
                self.assertTrue(data[1].startswith("metrics1 value=1 "))
                self.assertTrue(data[2].startswith("metrics2,tag1=tag,tag2=100 value1=1,value2=2 "))

            # now add database name
            fname = os.path.join(tmpdir, "monit2.dat")
            monit = monitor.InfluxDBFileMonitor(fname, 1000000, "metricsdb")

            monit.add_metrics("metrics1", value=1)
            monit.add_metrics("metrics2", value1=1, value2=2, tags={"tag1": "tag", "tag2": 100})

            path = monit.path
            # name should end with timestamp
            self.assertFalse(path.endswith(".dat"))

            monit.close()

            with open(path) as f:
                data = f.read().split("\n")
                self.assertEqual(len(data), 5)
                self.assertEqual(data[0], "# DML")
                self.assertEqual(data[1], "# CONTEXT-DATABASE: metricsdb")
                # ignores timestamps
                self.assertTrue(data[2].startswith("metrics1 value=1 "))
                self.assertTrue(data[3].startswith("metrics2,tag1=tag,tag2=100 value1=1,value2=2 "))

    def test_monitor_extratags(self):
        """Test for extra tags passed to constructor."""

        with tempfile.TemporaryDirectory() as tmpdir:
            fname = os.path.join(tmpdir, "monit1-%T.dat")
            monit = monitor.InfluxDBFileMonitor(fname, None, tags={"tag3": "supertag"})

            monit.add_metrics("metrics1", value=1)
            monit.add_metrics(
                "metrics2", value1=1, value2=2, tags={"tag1": "tag", "tag2": 100, "tag3": 1000000}
            )

            path = monit.path
            monit.close()
            with open(path) as f:
                data = f.read().split("\n")
                self.assertEqual(len(data), 4)
                self.assertEqual(data[0], "# DML")
                # ignores timestamps
                self.assertTrue(data[1].startswith("metrics1,tag3=supertag value=1 "))
                self.assertTrue(
                    data[2].startswith("metrics2,tag3=1000000,tag1=tag,tag2=100 value1=1,value2=2 ")
                )


if __name__ == "__main__":
    unittest.main()
