"""Class which runs queries sequentially."""

__all__ = ["QueryRunner"]

import logging
import random
import time

_LOG = logging.getLogger(__name__)


class QueryRunner:
    """This class will be passed to a subprocess and will be running
    queries sequentially.

    Parameters
    ----------
    queries : `dict` [`str`, `QueryFactory`]
        Factories for making query text, key is a short identifier useful
        for monitoring.
    maxRate : `float`, optional
        Max. query submittion rate in Hz or ``None`` to avoid limiting it.
    connectionFactory : callable
        Function or object that makes database connection, mock or real.
    runnerId : `str`
        String identifier for this runner.
    arraysize : `int`, optional
        Array size for fetchmany()
    queryCountLimit : `int`, option
        Limit number of queries to run, by default run indefinitely.
    runTimeLimit : `float`, option
        Limit for how long to run, in seconds, by default run indefinitely.
    monitor : `Monitor`
        Monitoring instance
    """

    def __init__(
        self,
        queries,
        max_rate,
        connection_factory,
        runner_id,
        arraysize=None,
        query_count_limit=None,
        run_time_limit=None,
        monitor=None,
    ):
        self._queries = queries
        self._queryKeys = list(queries.keys())
        self._maxRate = max_rate
        self._connectionFactory = connection_factory
        self._runnerId = runner_id
        self._arraysize = arraysize
        self._queryCountLimit = query_count_limit
        self._runTimeLimit = run_time_limit
        self._monitor = monitor

    def __call__(self):
        conn = self._connectionFactory()
        cursor = conn.cursor()

        n_queries = 0
        exec_time_cum = 0.0
        result_time_cum = 0.0
        query_time_cum = 0.0
        sleep_time_cum = 0.0

        t_start = time.time()

        while True:
            if self._queryCountLimit is not None and n_queries > self._queryCountLimit:
                # stop right here
                _LOG.debug(
                    "runner=%s: stopping due to query count limit: %s", self._runnerId, self._queryCountLimit
                )
                break

            if self._runTimeLimit is not None:
                if time.time() - t_start > self._runTimeLimit:
                    _LOG.debug(
                        "runner=%s: stopping due to run time limit: %s sec",
                        self._runnerId,
                        self._runTimeLimit,
                    )
                    break

            # chose one query randomly
            qkey = random.choice(self._queryKeys)
            query = self._queries[qkey].query()

            # may need to throttle it
            sleep_time = 0
            if self._maxRate is not None:
                now = time.time()
                t_next = t_start + n_queries / self._maxRate
                sleep_time = max(t_next - now, 0.0)
                if sleep_time > 0:
                    _LOG.debug("runner=%s: sleeping for %s seconds", self._runnerId, sleep_time)
                    time.sleep(sleep_time)

            if self._monitor:
                self._monitor.add_metrics(
                    "queries",
                    count=1,
                    tags={
                        "qid": qkey,
                        "runner": self._runnerId,
                    },
                )

            t_qstart = time.time()

            _LOG.debug("runner=%s: executing query %s: %s", self._runnerId, qkey, query)
            cursor.execute(query)

            t_executed = time.time()

            # fetch all data, do not fill memory with fetchall(), better
            # to iterate, hopefully it retrieves more than one row a a time
            rows = 42
            while rows:
                if self._arraysize:
                    rows = cursor.fetchmany(self._arraysize)
                else:
                    rows = cursor.fetchmany()

            if self._monitor:
                self._monitor.add_metrics(
                    "queries",
                    count=0,
                    tags={
                        "qid": qkey,
                        "runner": self._runnerId,
                    },
                )

            n_rows = cursor.rowcount

            t_qend = time.time()

            # size is a guess
            row_size = 0
            for column in cursor.description:
                if column.internal_size is not None:
                    row_size += column.internal_size
                else:
                    # order of magnitude correct for qserv data
                    row_size + 4

            result_size = row_size * n_rows

            _LOG.debug(
                "runner=%s: query execution time = %s sec, result fetch time = %s sec",
                self._runnerId,
                t_executed - t_qstart,
                t_qend - t_executed,
            )
            _LOG.debug(
                "runner=%s: row count = %s, result size = %s (approx)", self._runnerId, n_rows, result_size
            )

            n_queries += 1

            total_time = time.time() - t_start
            rate = total_time / n_queries

            exec_time_cum += t_executed - t_qstart
            result_time_cum += t_qend - t_executed
            query_time_cum += t_qend - t_qstart
            sleep_time_cum += sleep_time

            if self._monitor:
                # current query timing metrics
                self._monitor.add_metrics(
                    "query_time",
                    exec=t_executed - t_qstart,
                    result=t_qend - t_executed,
                    query=t_qend - t_qstart,
                    sleep=sleep_time,
                    tags={
                        "qid": qkey,
                        "runner": self._runnerId,
                    },
                )

                # cumulative time metrics
                self._monitor.add_metrics(
                    "query_time_sum",
                    exec=exec_time_cum,
                    result=result_time_cum,
                    query=query_time_cum,
                    sleep=sleep_time_cum,
                    tags={
                        "runner": self._runnerId,
                    },
                )

                # query rate
                self._monitor.add_metrics(
                    "rate",
                    value=rate,
                    tags={
                        "runner": self._runnerId,
                    },
                )

                # total query count
                self._monitor.add_metrics(
                    "total_queries",
                    count=n_queries,
                    tags={
                        "runner": self._runnerId,
                    },
                )

                # total time (just linear function)
                self._monitor.add_metrics(
                    "total_time",
                    value=total_time,
                    tags={
                        "runner": self._runnerId,
                    },
                )

        # because this runs in a subprocess we need to cleanup
        if self._monitor:
            self._monitor.close()
