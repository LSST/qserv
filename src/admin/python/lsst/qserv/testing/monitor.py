"""Classes for monitoring of test harness."""

import logging
import multiprocessing
import queue
import time
from abc import ABC, abstractmethod

_LOG = logging.getLogger(__name__)


class Monitor(ABC):
    """Interface definition for monitoring."""

    @abstractmethod
    def add_metrics(self, name, tags={}, _ts=None, **kw):
        """Add more metrics to the monitor.

        Parameters
        ----------
        name : `str`
            Metric name.
        tags : `dict` [`str`, `Any`]
            Dictionary with tags and tag values for the new measurement, tag
            is a name, it's better to have tag names in the form of regular
            identifiers. Tag value can be a string or an integer number.
        _ts : `int`, optional
            Timestamp associated with the metrics in microseconds since epoch,
            if `None` then current time will be used.
        **kw :
            All measurements as dictionary of measurement name and its value.
        """
        raise NotImplementedError()

    @abstractmethod
    def close(self):
        """Flush all data and close destination."""
        raise NotImplementedError()


class LogMonitor(Monitor):
    """Monitor which dumps everything to a specified logger.

    Parameters
    ----------
    logger : `str` or `logging.Logger`
        Name of the logger or logger instance.
    level : `int`, optional
        Logging level for messages, default is INFO.
    prefix : `str`, optional
        Text to add to each message.
    tags : `dict` [`str`, `Any`], optional
        Dictionary with tags and tag values to add to all metrics.
    """

    def __init__(self, logger, level=logging.INFO, prefix="monitor", tags=None):
        if not isinstance(logger, logging.Logger):
            logger = logging.getLogger(logger)
        self._logger = logger
        self._level = level
        self._prefix = prefix
        self._tags = tags

    def add_metrics(self, name, tags={}, _ts=None, **kw):
        # Docstring inherited from Monitor.
        if _ts is None:
            _ts = int(round(time.time() * 1e6))

        # add static tags
        if self._tags:
            alltags = self._tags.copy()
            alltags.update(tags)
            tags = alltags

        values = ", ".join(f"{k}={v}" for k, v in kw.items())
        tags = ", ".join(f"{k}={v}" for k, v in tags.items())

        self._logger.log(self._level, "%s: %s {%s} tags={%s} time=%s", self._prefix, name, values, tags, _ts)

    def close(self):
        # Docstring inherited from Monitor.
        pass


class InfluxDBFileMonitor(Monitor):
    """Monitor which dumps everything to a file in InfluxDB format.

    It supports rollover after specified period of time.

    Parameters
    ----------
    path : `str`
        Name of the output file which can contain "%T" pattern to be replaced
        with the actual timestamp in the format YYMMDDTHHMMSS. If ``perodSec``
        is not None and "%T" is missing from the path then it will be
        implicitly added as a suffix of the file name.
    periodSec : `int`, optional
        Rollover period in seconds. If not specified then there is no
        rollover.
    dbname : `str`, optional
        Name of InfluxDB database to add to the files.
    tags : `dict` [`str`, `Any`], optional
        Dictionary with tags and tag values to add to all metrics.
    """

    def __init__(self, path, period_sec=None, dbname=None, tags=None):
        self._path = path
        self._period = period_sec
        self._dbname = dbname
        self._tags = tags

        # add %T if needed
        if self._period is not None and "%T" not in self._path:
            self._path += ".%T"

        self._openTime = None
        self._file = None
        self.path = None

        self._open()

    def add_metrics(self, name, tags={}, _ts=None, **kw):
        # Docstring inherited from Monitor.
        now = time.time()

        # rollover to a new file if needed
        if self._period is not None and now - self._openTime >= self._period:
            self._open()

        # add static tags
        if self._tags:
            alltags = self._tags.copy()
            alltags.update(tags)
            tags = alltags

        if _ts is None:
            _ts = int(now * 1e6)

        tags = [name] + [f"{k}={v}" for k, v in tags.items()]
        tags = ",".join(tags)
        values = ",".join(f"{k}={v}" for k, v in kw.items())
        print(f"{tags} {values} {_ts}", file=self._file)

    def close(self):
        # Docstring inherited from Monitor.
        if self._file:
            self._file.close()

    def _open(self):
        """Open next file"""
        if self._file:
            self._file.close()

        self._openTime = time.time()
        self.path = self._path
        if "%T" in self.path:
            ts = time.strftime("%Y%m%dT%H%M%S", time.localtime(self._openTime))
            self.path = self.path.replace("%T", ts)

        _LOG.debug("Opening InfluxDB output file %s", self.path)
        self._file = open(self.path, "w")

        # starts with header
        print("# DML", file=self._file)
        if self._dbname:
            print(f"# CONTEXT-DATABASE: {self._dbname}", file=self._file)


class MPMonitor:
    """Class providing support for monitoring in multiprocessing apps.

    Sub-processes cannot always write to the same output file (e.g. with
    rolling files after re-opening). It may be possible to define unique
    output file for each subprocess but number of file will then expode. The
    solution implemented by this class is to collect metrics from all children
    in main process and send it to a single destination (there will be one
    file per slot still but that is manageable).

    This class does not implement Monitor interface and it cannot not be used
    to publish monitoring data directly. Instad it can be used asa factory of
    monitors to be passed to child processes (ChildMonitor class). Parent
    process then needs to call `process()` method periodically to retrive
    metrics published by subprocesses and forward them to actual monitor.

    Parameters
    ----------
    monitor : `Monitor`
        Monitor to forward all metrics to.
    """

    class ChildMonitor(Monitor):
        """Implementation of Monitor passed to child process.

        One has to explicitly call `close()` after all publishing stopped to
        flush the buffer.

        Parameters
        ----------
        queue : `multiprocessing.Queue`
            Queue instance where metrics will be pushed by sub-process.
        buffer_size : `int`
            Subprocess will buffer multiple metrics together to reduce queue
            polling by parent process. This number specifies the size of the
            buffer.
        """

        def __init__(self, queue, buffer_size=100):
            self._queue = queue
            self._buffer_size = buffer_size
            self._buffer = []

        def add_metrics(self, name, tags={}, _ts=None, **kw):
            # Docstring inherited from Monitor.
            if _ts is None:
                _ts = int(round(time.time() * 1e6))
            self._buffer.append((name, tags, _ts, kw))
            if len(self._buffer) >= self._buffer_size:
                self.flush()

        def flush(self):
            self._queue.put(self._buffer)
            self._buffer = []

        def close(self):
            # Docstring inherited from Monitor.
            self.flush()
            self._queue.close()

    def __init__(self, monitor):
        self._monitor = monitor
        # make a queue for monitoring data
        self._queue = multiprocessing.Queue()

    def child_monitor(self):
        """Make instance of Monitor for use by sub-process"""
        return MPMonitor.ChildMonitor(self._queue)

    def process(self, period):
        """Process metrics from queue.

        Parameters
        ----------
        period : `float` or `None`
            Time period in seconds to run. If it is None just process queue
            until it's empty and return.
        """

        def _forward_metrics(items):
            for item in items:
                name, tags, _ts, kw = item
                self._monitor.add_metrics(name, tags=tags, _ts=_ts, **kw)

        if period is None:
            # read until empty
            while True:
                try:
                    items = self._queue.get(False)
                    _forward_metrics(items)
                except queue.Empty:
                    break
        else:
            # read until max time is reached
            t = time.time()
            end = t + period
            while t < end:
                try:
                    items = self._queue.get(True, end - t)
                    _forward_metrics(items)
                except queue.Empty:
                    break
                t = time.time()
