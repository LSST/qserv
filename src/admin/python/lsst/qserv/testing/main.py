"""Command line script for running KPM tests.

To run it just make a small wrapper script (or use Python
setup process to define an entry point).
"""

import argparse
import functools
import logging
import os

import MySQLdb

from . import mock_db
from .config import Config
from .monitor import InfluxDBFileMonitor, LogMonitor
from .runner_mgr import RunnerManager

_LOG = logging.getLogger(__name__)


def _logConfig(level, slot):
    levels = {0: logging.WARNING, 1: logging.INFO, 2: logging.DEBUG}
    if slot is None:
        simple_format = "%(asctime)s %(levelname)7s %(name)s -- %(message)s"
    else:
        simple_format = f"%(asctime)s %(levelname)7s %(name)s [slot:{slot}] -- %(message)s"
    level = levels.get(level, logging.DEBUG)
    logging.basicConfig(format=simple_format, level=level)


def main():
    parser = argparse.ArgumentParser(description="Test harness to generate load for QServ")

    parser.add_argument(
        "-v", "--verbose", default=0, action="count", help="More verbose output, can use several times."
    )

    agroup = parser.add_argument_group("Database connection options")
    agroup.add_argument(
        "--dummy-db",
        action="store_true",
        default=False,
        help="Use dummy implementation of database connection, for testing.",
    )
    agroup.add_argument("--host", default=None, metavar="HOST", help="Host name for qserv connection.")
    agroup.add_argument(
        "--port",
        default=4040,
        metavar="NUMBER",
        help="Port number for qserv connection, default: %(default)s.",
    )
    agroup.add_argument(
        "--user",
        default="qsmaster",
        metavar="STRING",
        help="User name for qserv connection, default: %(default)s.",
    )
    agroup.add_argument("--password", default=None, metavar="STRING", help="Password for qserv connection.")
    agroup.add_argument("--db", default="LSST", metavar="STRING", help="Database name, default: %(default)s.")

    agroup = parser.add_argument_group("Execution options")
    agroup.add_argument(
        "-n",
        "--num-slots",
        type=int,
        default=None,
        metavar="NUMBER",
        help="Number of slots to divide the whole workload into.",
    )
    agroup.add_argument(
        "-s",
        "--slot",
        type=int,
        default=None,
        metavar="NUMBER",
        help="Slot number for this process, in range [0, num-slots)."
        " --num-slots and --slot must be specified together.",
    )
    agroup.add_argument(
        "-t",
        "--time-limit",
        type=int,
        default=None,
        metavar="SECONDS",
        help="Run for maximum number of seconds.",
    )
    agroup.add_argument(
        "--slurm", action="store_true", default=False, help="Obtain slot information from slurm."
    )

    agroup = parser.add_argument_group("Monitoring options")
    agroup.add_argument(
        "-m",
        "--monitor",
        choices=["log", "influxdb-file"],
        help="Type for monitoring output, one of %(choices)s, default is no output.",
    )
    agroup.add_argument(
        "-r",
        "--monitor-rollover",
        type=int,
        default=3600,
        metavar="SECONDS",
        help="Number of seconds between rollovers for influxdb-file monitor, default: %(default)s",
    )
    agroup.add_argument(
        "--influxdb-file-name",
        default="qserv-kraken-mon-%S-%T.dat",
        metavar="PATH",
        help="File name template for influxdb-file monitor, default: %(default)s.",
    )
    agroup.add_argument(
        "--influxdb-db",
        default="qserv_kraken",
        metavar="DATABASE",
        help="InfluxDB database name, default: %(default)s.",
    )

    parser.add_argument(
        "--dump-config", action="store_true", default=False, help="Dump resulting configuration."
    )

    parser.add_argument(
        "config",
        nargs="+",
        type=argparse.FileType(),
        help="Configuration file name, at least one is required.",
    )
    args = parser.parse_args()

    if (args.num_slots, args.slot).count(None) == 1:
        parser.error("options --num-slots and --slot must be specified together")

    if args.slurm:
        if args.num_slots is not None or args.slot is not None:
            parser.error("cannot use --slurm together with --num-slots or --slot")
            return 2
        num_slots = os.environ.get("SLURM_NTASKS")
        slot = os.environ.get("SLURM_NODEID")
        if num_slots is None or slot is None:
            parser.error("cannot determine slurm configuration, envvar is not set")
            return 2
        num_slots = int(num_slots)
        if num_slots > 1:
            args.num_slots = num_slots
            args.slot = int(slot)

    _logConfig(args.verbose, args.slot)

    # build/split config
    cfg = Config.from_yaml(args.config)
    if args.num_slots is not None:
        cfg = cfg.split(args.num_slots, args.slot)
    if args.dump_config:
        print(f"Configuration for this process (n_slots={args.num_slots}, slot={args.slot}):")
        print(cfg.to_yaml())

    # connection factory
    if args.dummy_db:
        connFactory = mock_db.connect
    else:
        connFactory = functools.partial(
            MySQLdb.connect, host=args.host, port=args.port, user=args.user, passwd=args.password, db=args.db
        )

    # monitor
    tags = None if args.slot is None else {"slot": args.slot}
    monitor = None
    if args.monitor == "log":
        monitor = LogMonitor(logging.getLogger("metrics"), tags=tags)
    elif args.monitor == "influxdb-file":
        slot = "" if args.slot is None else str(args.slot)
        fname = args.influxdb_file_name.replace("%S", slot)
        monitor = InfluxDBFileMonitor(
            fname, periodSec=args.monitor_rollover, dbname=args.influxdb_db, tags=tags
        )

    mgr = RunnerManager(cfg, connFactory, args.slot, runTimeLimit=args.time_limit, monitor=monitor)
    mgr.run()

    if monitor:
        monitor.close()


if __name__ == "__main__":
    main()
