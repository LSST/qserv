"""Mock for MySQL DBAPI."""

from collections import namedtuple
import logging
import re
import time


_LOG = logging.getLogger(__name__)

_num_re = re.compile(r"\d+")


ColDesriptor = namedtuple("ColDesriptor", "name type_code display_size internal_size precision scale null_ok")


class MockCursor:
    """Mock cursor class.

    For our test we only need couple of methods.
    """

    arraysize = 1

    def __init__(self):
        self._query = None
        self.n_rows = 0
        self.rows = []

    def execute(self, query):
        _LOG.debug("executing query: %s", query)
        self._query = query
        self.n_rows = 2
        if self._query:
            # some parameterization, look at the query, if it contains any
            # number then use that number for the count of rows returned
            match = _num_re.search(self._query)
            if match:
                self.n_rows = min(int(match[0]), 10000)
        self.rows = [(i, f"row{i}") for i in range(self.n_rows)]
        # spend at least few milliseconds in query
        time.sleep(0.01)

    def fetchall(self):
        rows = self.rows
        self.rows = []
        return rows

    def fetchmany(self, arraysize=None):
        if arraysize is None:
            arraysize = self.arraysize
        rows = self.rows[:arraysize]
        self.rows = self.rows[arraysize:]
        return rows

    @property
    def rowcount(self):
        return self.n_rows

    @property
    def description(self):
        # some randome codes
        return [
            ColDesriptor(
                name="ID", type_code=1, display_size=10, internal_size=4, precision=0, scale=1, null_ok=False
            ),
            ColDesriptor(
                name="name",
                type_code=15,
                display_size=32,
                internal_size=8,
                precision=0,
                scale=1,
                null_ok=True,
            ),
        ]


class MockConnection:
    def cursor(self):
        return MockCursor()


def connect(*args, **kwargs):
    """Can take any parameters so it can be used as replacement for
    MySQLdb.connect() method.
    """
    return MockConnection()
