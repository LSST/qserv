"""Class which manages concurrent execution of queries."""

__all__ = ["RunnerManager"]

import logging
import time
from multiprocessing import Process

from .monitor import MPMonitor
from .query_runner import QueryRunner

_LOG = logging.getLogger(__name__)


class RunnerManager:
    """Class which manages execution of multiple QueryRunners.

    Parameters
    ----------
    config : `Config`
        Configuration object.
    connectionFactory : callable
        Function or object that makes database connection, mock or real.
    slot : `int`, optional
        Slot number for this manager, can be None.
    queryCountLimit : `int`, option
        Limit number of queries to run, by default run indefinitely.
    runTimeLimit : `float`, option
        Limit for how long to run, in seconds, by default run indefinitely.
    monitor : `Monitor`
        Monitoring instance
    """

    def __init__(self, config, connection_factory, slot, runtime_limit=None, monitor=None):
        self._config = config
        self._connectionFactory = connection_factory
        self._slot = slot
        self._runTimeLimit = runtime_limit
        self._monitor = monitor

    def run(self):
        """Start all runners and wait until they are done."""

        monitor = None
        if self._monitor:
            monitor = MPMonitor(self._monitor)

        # instantiate all query runners
        runners = {}
        for qclass in self._config.classes():
            n_runners = self._config.concurrentQueries(qclass)
            queries = self._config.queries(qclass)
            _LOG.debug("%s runners and %s queries for class %s", n_runners, len(queries), qclass)
            max_rate = self._config.maxRate(qclass)
            arraysize = self._config.arraysize(qclass)
            for i_runner in range(n_runners):
                if self._slot is not None:
                    runner_id = f"{qclass}-{self._slot}-{i_runner}"
                else:
                    runner_id = f"{qclass}-{i_runner}"

                _LOG.debug("Creating runner %s", runner_id)
                runner = QueryRunner(
                    queries,
                    max_rate,
                    self._connectionFactory,
                    runner_id,
                    arraysize,
                    query_count_limit=None,
                    run_time_limit=self._runTimeLimit,
                    monitor=monitor.child_monitor() if monitor else None,
                )
                runners[runner_id] = runner

        # start all of them
        processes = {}
        for runner_id, runner in runners.items():
            _LOG.info("Starting runner %s", runner_id)
            proc = Process(target=runner, name=runner_id, daemon=True)
            proc.start()
            processes[runner_id] = proc

        # Now wait until they finish, there is a smarter ways to wait
        # but this is good enough for now.
        while processes:
            for runner_id, proc in processes.items():
                if not proc.is_alive():
                    _LOG.info("Runner %s finished", runner_id)
                    del processes[runner_id]
                    break
            else:
                if monitor:
                    monitor.process(1)
                else:
                    # just relax
                    time.sleep(1)

        if monitor:
            # process whatever may be remaining in the queue
            monitor.process(None)

        _LOG.info("All runners have finished")
