# This file is part of qserv.
#
# Developed for the LSST Data Management System.
# This product includes software developed by the LSST Project
# (https://www.lsst.org).
# See the COPYRIGHT file at the top-level directory of this distribution
# for details of code ownership.
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
# You should have received a copy of the GNU General Public License

"""Shared options for CLI functions in qserv."""

from abc import abstractmethod
from functools import partial
from collections.abc import Callable

import click

from .utils import process_targs, yaml_presets


class OptionGroup:
    """Base class for an option group decorator. Requires the option group
    subclass to have a property called `decorator`."""

    @property
    @abstractmethod
    def decorators(self) -> list[Callable]:
        pass

    def __call__(self, f: Callable) -> Callable:
        for decorator in reversed(self.decorators):
            f = decorator(f)
        return f


option_cmsd_manager_name = partial(
    click.option,
    "--cmsd-manager-name",
    help="""The domain name of the cmsd manager node(s).""",
)


option_czar_connection = partial(
    click.option,
    "--czar-connection",
    help="The czar db connection in format mysql://user:pass@host:port/database.",
)


option_worker_connection = partial(
    click.option,
    "--worker-connection",
    "worker_connections",
    help="The worker db connection(s) in format mysql://user:pass@host:port/database.",
    multiple=True,
)


option_db_qserv_user = partial(
    click.option,
    "--db-qserv-user",
    default="qsmaster",
    help="The user to use for mysql database.",
)


option_debug = partial(
    click.option,
    "--debug",
    "debug_port",
    help="Run gdbserver with the application arguments.",
)


option_instance_id = partial(
    click.option,
    "--instance-id",
    help="The unique identifier of a Qserv instance served by the Replication System.",
    required=True,
)


logLevelChoices = ["CRITICAL", "ERROR", "WARNING", "INFO", "DEBUG"]
option_log_level = partial(
    click.option,
    "--log-level",
    default="INFO",
    type=click.Choice(logLevelChoices, case_sensitive=False),
    callback=lambda ctx, par, val: val.upper(),
    help=f"The logging level for the entrypoint tool. Supported levels are [{'|'.join(logLevelChoices)}]",
)


option_mysql_monitor_password = partial(
    click.option,
    "--mysql-monitor-password",
    help="The password for monitoring applications.",
)


option_repl_connection = partial(
    click.option,
    "--repl-connection",
    help="The connection string for the replication database in format mysql://user:pass@host:port/database",
)


option_repl_instance_id = partial(
    click.option,
    "--repl-instance-id",
    help="The unique identifier of the current Replication System's domain. "
    "The identifier is a part of the security context preventing accidental 'cross-talks' "
    "between unrelated domains.",
    default="",
    show_default=True,
)


option_repl_auth_key = partial(
    click.option,
    "--repl-auth-key",
    help="The authorization key. The key is a part of the security context "
    "preventing unauthorized operations witin the current Replication System's "
    "domain.",
    default="",
    show_default=True,
)

option_repl_admin_auth_key = partial(
    click.option,
    "--repl-admin-auth-key",
    help="The admin authorizaiton key for the replication-ingest system.",
    default="",
    show_default=True,
)


option_repl_registry_host = partial(
    click.option,
    "--repl-registry-host",
    help="The FQDN of a host where the Replication System's Registry service is run.",
)


option_repl_registry_port = partial(
    click.option,
    "--repl-registry-port",
    help="The port number of the Replication System's Registry service.",
    default=8080,
    show_default=True,
)


option_repl_http_port = partial(
    click.option,
    "--repl-http-port",
    help="The port number of the of the worker control service used by the Replication System "
    "and worker monitoring applications.",
    default=0,
    show_default=True,
)


option_results_dirname = partial(
    click.option,
    "--results-dirname",
    help="Path to a folder where worker stores result sets of queries.",
    default="/qserv/data/results",
    show_default=True,
)


resultProtocolChoices = ["XROOT", "HTTP"]
option_results_protocol = partial(
    click.option,
    "--results-protocol",
    help=f"Result delivery protocol. Allowed options are [{'|'.join(resultProtocolChoices)}]",
    default="HTTP",
    type=click.Choice(resultProtocolChoices, case_sensitive=False),
    show_default=True,
)


option_run = partial(
    click.option,
    "--run/--no-run",
    is_flag=True,
    default=True,
    help="Run the subcommand that is executed by entrypoint."
    "For --no-run, will print the command and arguments that would have "
    "been run.",
)


option_vnid_config = partial(
    click.option,
    "--vnid-config",
    help="The config parameters used by the qserv cmsd to get the vnid from the specified "
    " source (static string, a file or worker database).",
)


option_xrootd_manager = partial(
    click.option,
    "--xrootd-manager",
    help="The host name of the xrootd manager.",
)


option_tests_yaml = partial(
    click.option,
    "--tests-yaml",
    help="Path to the yaml that contains settings for integration test execution.",
    default="/usr/local/etc/integration_tests.yaml",
    show_default=True,
)


option_case = partial(
    click.option,
    "--case",
    "cases",
    help="Run this/these test cases only. If omitted will run all the cases.",
    multiple=True,
)


option_load = partial(
    click.option,
    "--load/--no-load",
    help=(
        "Force qserv_testdata to be loaded or not loaded into qserv and the reference database. "
        "Will handle --unload first. "
        "By default, if --unload is passed will not load databases, "
        "otherwise will load test databases that are not loaded yet."
    ),
    is_flag=True,
    default=None,
)


option_unload = partial(
    click.option,
    "--unload",
    help="Unload qserv_testdata from qserv and the reference database. Will be handled before --load.",
    is_flag=True,
)


option_reload = partial(
    click.option,
    "--reload",
    help="Remove and reload testdata. Same as passing --unload --load.",
    is_flag=True,
)

option_load_http = partial(
    click.option,
    "--load-http",
    help="HTTP-based table loading protocol. Used with --load and --reload",
    is_flag=True,
)

option_run_tests = partial(
    click.option,
    "--run-tests/--no-run-tests",
    help="Run or skip test execution. Defaults to --run-tests.",
    default=True,
)

option_keep_results = partial(
    click.option,
    "--keep-results/--no-keep-results",
    help="Keep or delet results after finishing the tests. Defaults to --no-keep-results.",
    default=False,
)

option_compare_results = partial(
    click.option,
    "--compare-results/--no-compare-results",
    help="Run or skip query output comparison. Defaults to --compare-results",
    is_flag=True,
    default=True,
)


option_db_uri = partial(click.option, "--db-uri", help="The URI to the database.")


option_db_admin_uri = partial(click.option, "--db-admin-uri", help="The admin URI to the database.")


option_options_file = partial(
    click.option,
    "--options-file",
    "-@",
    expose_value=False,  # This option should not be forwarded
    help="""Path to a YAML file containing overrides of command line options.
The YAML should be organized as a hierarchy with subcommand names at the top
level, options names and values (as key-value pairs) for that subcommand
below. The option name should NOT include prefix dashes.""",
    callback=yaml_presets,
)


option_cmd = partial(
    click.option,
    "--cmd",
    help="""The command template (in jinja2 format) that will be used to call
    the command that this function executes.""",
    show_default=True,
)


option_targs = partial(
    click.option,
    "--targs",
    help="""Key=value pairs to be applied to templates. Value may be a comma-separated list.""",
    multiple=True,
    callback=process_targs,
)


option_targs_file = partial(
    click.option,
    "--targs-file",
    help="""Path to a yaml file that contains key-value pairs to apply to templates.""",
)


class options_targs(OptionGroup):  # noqa: N801
    @property
    def decorators(self) -> list[Callable]:
        return [
            option_targs(),
            option_targs_file(),
        ]


option_log_cfg_file = partial(
    click.option,
    "--log-cfg-file",
    help="Path to the log4cxx config file.",
    default="/config-etc/log/log.cnf",
    show_default=True,
)
