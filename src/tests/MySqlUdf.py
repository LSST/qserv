#! /usr/bin/env python

import getpass
import math
import optparse
import random
import sys
import unittest

from lsst.db.engineFactory import getEngineFromArgs


def dbparam(x):
    if x is None:
        return "NULL"
    elif isinstance(x, str):
        return "'" + x + "'"
    else:
        return repr(x)


def ang_sep(ra1, dec1, ra2, dec2):
    sdt = math.sin(math.radians(ra1 - ra2) * 0.5)
    sdp = math.sin(math.radians(dec1 - dec2) * 0.5)
    cc = math.cos(math.radians(dec1)) * math.cos(math.radians(dec2))
    s = math.sqrt(sdp * sdp + cc * sdt * sdt)
    if s > 1.0:
        return 180.0
    else:
        return 2.0 * math.degrees(math.asin(s))


def pt_in_sph_ellipse(ra, dec, ra_cen, dec_cen, smaa, smia, ang):
    ra = math.radians(ra)
    dec = math.radians(dec)
    v = (math.cos(ra) * math.cos(dec), math.sin(ra) * math.cos(dec), math.sin(dec))
    lon = math.radians(ra_cen)
    lat = math.radians(dec_cen)
    ang = math.radians(ang)
    sin_lon = math.sin(lon)
    cod_lon = math.cos(lon)
    sin_lat = math.sin(lat)
    cos_lat = math.cos(lat)
    sin_ang = math.sin(ang)
    cos_ang = math.cos(ang)
    # get coords of input point in (N,E) basis
    n = cos_lat * v[2] - sin_lat * (sin_lon * v[1] + cod_lon * v[0])
    e = cod_lon * v[1] - sin_lon * v[0]
    # rotate by negated major axis angle
    x = sin_ang * e + cos_ang * n
    y = cos_ang * e - sin_ang * n
    # scale by inverse of semi-axis-lengths
    x /= math.radians(smaa / 3600.0)
    y /= math.radians(smia / 3600.0)
    # Apply point in circle test for the unit circle centered at the origin
    r = x * x + y * y
    if r < 1.0 - 1e-11:
        return True
    elif r > 1.0 + 1e-11:
        return False
    return None


def flatten(sequence_list, ltypes=(list, tuple)):
    ltype = type(sequence_list)
    sequence_list = list(sequence_list)
    i = 0
    while i < len(sequence_list):
        while isinstance(sequence_list[i], ltypes):
            if not sequence_list[i]:
                sequence_list.pop(i)
                i -= 1
                break
            else:
                sequence_list[i : i + 1] = sequence_list[i]
        i += 1
    return ltype(sequence_list)


class MySqlUdfTestCase(unittest.TestCase):
    """Tests MySQL UDFs."""

    def setUp(self):
        global _options
        engine = getEngineFromArgs(
            username=_options.user, password=_options.password, query={"unix_socket": _options.socketFile}
        )
        self._conn = engine.connect()

    def tearDown(self):
        del self._conn

    def _query(self, query, result):
        qresult = self._conn.execute(query)
        rows = qresult.fetchall()
        self.assertEqual(rows[0][0], result, query + f" did not return {dbparam(result)}.")

    def _ang_sep(self, result, *args):
        args = tuple(dbparam(arg) for arg in args)
        query = f"SELECT scisql_ang_sep({args[0]}, {args[1]}, {args[2]}, {args[3]})"
        qresult = self._conn.execute(query)
        rows = qresult.fetchall()
        if result is None:
            self.assertEqual(rows[0][0], result, query + " did not return NULL.")
        else:
            msg = query + f" not close enough to {dbparam(result)}"
            self.assertAlmostEqual(rows[0][0], result, 11, msg)

    def testang_sep(self):
        for i in range(4):
            a = [0.0] * 4
            a[i] = None
            self._ang_sep(None, *a)
        for d in (-91.0, 91.0):
            self._ang_sep(None, 0.0, d, 0.0, 0.0)
            self._ang_sep(None, 0.0, 0.0, 0.0, d)
        for d in (0.0, 90.0, -90.0):
            self._ang_sep(0.0, 0.0, d, 0.0, d)
        for _ in range(100):
            args = [
                random.uniform(0.0, 360.0),
                random.uniform(-90.0, 90.0),
                random.uniform(0.0, 360.0),
                random.uniform(-90.0, 90.0),
            ]
            self._ang_sep(ang_sep(*args), *args)

    def _pt_in_sph_box(self, result, *args):
        args = tuple(dbparam(arg) for arg in args)
        query = f"SELECT scisql_s2PtInBox({args[0]}, {args[1]}, {args[2]}, {args[3]}, {args[4]}, {args[5]})"
        self._query(query, result)

    def test_pt_in_sph_box(self):
        for i in range(6):
            a = [0.0] * 6
            a[i] = None
            self._pt_in_sph_box(0, *a)
        for d in (-91.0, 91.0):
            for i in (1, 3, 5):
                a = [0.0] * 6
                a[i] = d
                self._pt_in_sph_box(None, *a)
        for ra_min, ra_max in ((370.0, 10.0), (50.0, -90.0), (400.0, -400.0)):
            self._pt_in_sph_box(None, 0.0, 0.0, ra_min, 0.0, ra_max, 0.0)
        for ra, dec in ((360.0, 0.5), (720.0, 0.5), (5.0, 0.5), (355.0, 0.5)):
            self._pt_in_sph_box(1, ra, dec, 350.0, 0.0, 370.0, 1.0)
        for ra, dec in ((0.0, 1.1), (0.0, -0.1), (10.1, 0.5), (349.9, 0.5)):
            self._pt_in_sph_box(0, ra, dec, 350.0, 0.0, 370.0, 1.0)

    def _pt_in_sph_circle(self, result, *args):
        args = tuple(dbparam(arg) for arg in args)
        query = f"SELECT scisql_s2PtInCircle({args[0]}, {args[1]}, {args[2]}, {args[3]}, {args[4]})"
        self._query(query, result)

    def test_pt_in_sph_circle(self):
        for i in range(5):
            a = [0.0] * 5
            a[i] = None
            self._pt_in_sph_circle(0, *a)
        for d in (-91.0, 91.0):
            self._pt_in_sph_circle(None, 0.0, d, 0.0, 0.0, 0.0)
            self._pt_in_sph_circle(None, 0.0, 0.0, 0.0, d, 0.0)
        for r in (-1.0, 181.0):
            self._pt_in_sph_circle(None, 0.0, 0.0, 0.0, 0.0, r)
        for _i in range(10):
            ra_cen = random.uniform(0.0, 360.0)
            dec_cen = random.uniform(-90.0, 90.0)
            radius = random.uniform(0.0001, 10.0)
            for _j in range(100):
                delta = radius / math.cos(math.radians(dec_cen))
                ra = random.uniform(ra_cen - delta, ra_cen + delta)
                dec = random.uniform(max(dec_cen - radius, -90.0), min(dec_cen + radius, 90.0))
                r = ang_sep(ra_cen, dec_cen, ra, dec)
                if r < radius - 1e-9:
                    self._pt_in_sph_circle(1, ra, dec, ra_cen, dec_cen, radius)
                elif r > radius + 1e-9:
                    self._pt_in_sph_circle(0, ra, dec, ra_cen, dec_cen, radius)

    def _pt_in_sph_ellipse(self, result, *args):
        args = tuple(dbparam(arg) for arg in args)
        query = (
            f"SELECT scisql_s2PtInEllipse({args[0]}, {args[1]}, {args[2]}, {args[3]}, {args[4]}, "
            f"{args[5]}, {args[6]})"
        )
        self._query(query, result)

    def testpt_in_sph_ellipse(self):
        for i in range(7):
            a = [0.0] * 7
            a[i] = None
            self._pt_in_sph_ellipse(0, *a)
        for d in (-91.0, 91.0):
            self._pt_in_sph_ellipse(None, 0.0, d, 0.0, 0.0, 0.0, 0.0, 0.0)
            self._pt_in_sph_ellipse(None, 0.0, 0.0, 0.0, d, 0.0, 0.0, 0.0)
        self._pt_in_sph_ellipse(None, 0.0, 0.0, 0.0, 0.0, 1.0, 2.0, 0.0)
        self._pt_in_sph_ellipse(None, 0.0, 0.0, 0.0, 0.0, 2.0, -1.0, 0.0)
        self._pt_in_sph_ellipse(None, 0.0, 0.0, 0.0, 0.0, 36001.0, 1.0, 0.0)
        for _i in range(10):
            ra_cen = random.uniform(0.0, 360.0)
            dec_cen = random.uniform(-90.0, 90.0)
            smaa = random.uniform(0.0001, 36000.0)
            smia = random.uniform(0.00001, smaa)
            ang = random.uniform(-180.0, 180.0)
            for _j in range(100):
                smaa_deg = smaa / 3600.0
                delta = smaa_deg / math.cos(math.radians(dec_cen))
                ra = random.uniform(ra_cen - delta, ra_cen + delta)
                dec = random.uniform(max(dec_cen - smaa_deg, -90.0), min(dec_cen + smaa_deg, 90.0))
                r = pt_in_sph_ellipse(ra, dec, ra_cen, dec_cen, smaa, smia, ang)
                if r is True:
                    self._pt_in_sph_ellipse(1, ra, dec, ra_cen, dec_cen, smaa, smia, ang)
                elif r is False:
                    self._pt_in_sph_ellipse(0, ra, dec, ra_cen, dec_cen, smaa, smia, ang)

    def _pt_in_sph_poly(self, result, *args):
        args = ", ".join(dbparam(arg) for arg in args)
        query = f"SELECT scisql_s2PtInCPoly({args[0]})"
        self._query(query, result)

    def test_pt_in_sph_poly(self):
        # Test for NULL in any argument, returns 0
        for i in range(8):
            a = [0.0, 0.0, 0, 0, 90, 0, 0, 90]
            a[i] = None
            self._pt_in_sph_poly(0, *a)

        # test for decl outside allowed range, makes NULL
        for d in (-91, 91):
            self._pt_in_sph_poly(None, 0.0, d, 0, 0, 90, 0, 0, 90)
            self._pt_in_sph_poly(None, 0.0, 0.0, 0, 0, 90, 0, 0, d)

        # test for incorrect number of poly coordinates
        self.assertRaises(Exception, self._pt_in_sph_poly, None, 0.0, 0.0, 0, 0, 90, 0, 60, 45, 30)
        self.assertRaises(Exception, self._pt_in_sph_poly, None, 0.0, 0.0, 0, 0, 90, 0, 60)
        self.assertRaises(Exception, self._pt_in_sph_poly, None, 0.0, 0.0, 0, 0, 90, 0)

        # Test for non-exceptional cases
        x = (0, 0)
        nx = (180, 0)
        y = (90, 0)
        ny = (270, 0)
        z = (0, 90)
        nz = (0, -90)
        tris = [
            (x, y, z),
            (y, nx, z),
            (nx, ny, z),
            (ny, (360, 0), z),
            ((360, 0), ny, nz),
            (ny, nx, nz),
            (nx, y, nz),
            (y, x, nz),
        ]
        for t in tris:
            spec = flatten(t)
            for _ in range(100):
                ra = random.uniform(0.0, 360.0)
                dec = random.uniform(-90.0, 90.0)
                if (t[2][1] > 0 and (dec < 0.0 or ra < t[0][0] or ra > t[1][0])) or (
                    t[2][1] < 0 and (dec > 0.0 or ra < t[1][0] or ra > t[0][0])
                ):
                    self._pt_in_sph_poly(0, ra, dec, *spec)
                else:
                    self._pt_in_sph_poly(1, ra, dec, *spec)


def main():
    global _options

    parser = optparse.OptionParser()
    parser.add_option(
        "-S",
        "--socket",
        dest="socketFile",
        default="/tmp/smm.sock",
        help="Use socket file FILE to connect to mysql",
        metavar="FILE",
    )
    parser.add_option(
        "-u", "--user", dest="user", default="qsmaster", help="User for db login if not %default"
    )
    parser.add_option(
        "-p", "--password", dest="password", default="", help="Password for db login. ('-' prompts)"
    )
    (_options, args) = parser.parse_args()
    if _options.password == "-":
        _options.password = getpass.getpass()
    random.seed(123456789)
    unittest.main(argv=[sys.argv[0]])


if __name__ == "__main__":
    main()
