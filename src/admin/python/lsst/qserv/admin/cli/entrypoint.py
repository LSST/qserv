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


"""Command line tool for launching qserv components inside a qserv container."""

import logging
import os
import sys
from collections import OrderedDict
from collections.abc import Callable
from dataclasses import dataclass, field
from functools import partial
from typing import Any

import click
from click.decorators import pass_context

from ..watcher import watch
from . import script, utils
from .options import (
    OptionGroup,
    option_case,
    option_cmd,
    option_cmsd_manager_name,
    option_compare_results,
    option_czar_connection,
    option_db_admin_uri,
    option_db_qserv_user,
    option_db_uri,
    option_debug,
    option_keep_results,
    option_load,
    option_load_http,
    option_log_cfg_file,
    option_log_level,
    option_mysql_monitor_password,
    option_options_file,
    option_reload,
    option_repl_admin_auth_key,
    option_repl_auth_key,
    option_repl_connection,
    option_repl_http_port,
    option_repl_instance_id,
    option_repl_registry_host,
    option_repl_registry_port,
    option_results_dirname,
    option_results_protocol,
    option_run,
    option_run_tests,
    option_tests_yaml,
    option_unload,
    option_vnid_config,
    option_worker_connection,
    option_xrootd_manager,
    options_targs,
)
from .render_targs import render_targs

_log = logging.getLogger(__name__)


template_dir = "/usr/local/qserv/templates/"
mysql_proxy_cfg_template = os.path.join(template_dir, "proxy/etc/my-proxy.cnf.jinja")
czar_cfg_template = os.path.join(template_dir, "proxy/etc/qserv-czar.cnf.jinja")
czar_http_cfg_template = os.path.join(template_dir, "http/etc/qserv-czar.cnf.jinja")
cmsd_manager_cfg_template = os.path.join(template_dir, "xrootd/etc/cmsd-manager.cf.jinja")
cmsd_worker_cfg_template = os.path.join(template_dir, "xrootd/etc/cmsd-worker.cf.jinja")
xrdssi_cfg_template = os.path.join(template_dir, "xrootd/etc/xrdssi.cf.jinja")
xrootd_manager_cfg_template = os.path.join(template_dir, "xrootd/etc/xrootd-manager.cf.jinja")

mysql_proxy_cfg_path = "/config-etc/my-proxy.cnf"
czar_cfg_path = "/config-etc/qserv-czar.cnf"
czar_http_cfg_path = "/config-etc/qserv-czar.cnf"
cmsd_manager_cfg_path = "/config-etc/cmsd-manager.cnf"
cmsd_worker_cfg_path = "/config-etc/cmsd-worker.cf"
xrdssi_cfg_path = "/config-etc/xrdssi-worker.cf"
xrootd_manager_cfg_path = "/config-etc/xrootd-manager.cf"

socket_option_help = f"""Accepts query key {
    click.style("socket", bold=True)
}: The path to a socket file used to connect to the database.
"""

socket_option_description = f"""For URI options that accept a socket: if
{click.style("host", bold=True)} and {click.style("port", bold=True)} are
provided then node excution will be paused early, until the database TCP
connection is available for connections. If {click.style("socket", bold=True)}
is provided then the {click.style("host", bold=True)} and
{click.style("port", bold=True)} part of the URI are not required. If
{click.style("host", bold=True)}, {click.style("port", bold=True)}, and
{click.style("socket", bold=True)} are provided then node excution will be
paused until the database is available via TCP connection, and the
{click.style("socket", bold=True)} will be used for subsequent database
communication."""

extended_args_description = """Options and arguments may be passed directly to
{app} by adding '--', and then adding those options and arguments."""


worker_db_help = f"""Non-admin URI to the worker database. {socket_option_help}
 Populates 'hostname', 'port', and 'socket' under '[mysql]' in the xrdssi config
file. Also used to wait for schema to be at the correct version in this
database.
"""


admin_worker_db_help = "Admin URI to the worker database. " + socket_option_help


@dataclass
class CommandInfo:
    default_cmd: str | None = None


# Commands are in the ordered dict in "help order" - the order they
# appear in `entrypoint --help`
commands = OrderedDict(
    (
        (
            "proxy",
            CommandInfo(
                "mysql-proxy --proxy-lua-script=/usr/local/lua/qserv/scripts/mysqlProxy.lua "
                "--lua-cpath=/usr/local/lua/qserv/lib/czarProxy.so --defaults-file={{proxy_cfg_path}}",
            ),
        ),
        (
            "czar-http",
            CommandInfo(
                "qserv-czar-http "
                "--czar-name {{czar_name}} "
                "--config {{czar_cfg_path}} "
                "--port {{http_port}} "
                "--threads {{http_threads}} "
                "--worker-ingest-threads {{http_worker_ingest_threads}} "
                "--ssl-cert-file {{http_ssl_cert_file}} "
                "--ssl-private-key-file {{http_ssl_private_key_file}} "
                "--tmp-dir {{http_tmp_dir}} "
                "--conn-pool-size {{http_conn_pool_size}} "
                "--verbose",
            ),
        ),
        (
            "cmsd-manager",
            CommandInfo(
                "cmsd -c {{cmsd_manager_cfg_path}} -n manager -I v4",
            ),
        ),
        ("xrootd-manager", CommandInfo("xrootd -c {{xrootd_manager_cfg_path}} -n manager -I v4")),
        (
            "worker-cmsd",
            CommandInfo(
                "cmsd -c {{cmsd_worker_cfg_path}} -n worker -I v4 -l @libXrdSsiLog.so -+xrdssi "
                "{{xrdssi_cfg_path}}",
            ),
        ),
        (
            "worker-repl",
            CommandInfo(
                "qserv-replica-worker "
                "--qserv-worker-db={{db_admin_uri}} "
                "--config={{config}} {% for arg in extended_args %}{{arg}}  {% endfor %}"
            ),
        ),
        (
            "worker-xrootd",
            CommandInfo(
                "xrootd -c {{cmsd_worker_cfg_path}} -n worker -I v4 -l @libXrdSsiLog.so -+xrdssi "
                "{{xrdssi_cfg_path}}",
            ),
        ),
        (
            "replication-controller",
            CommandInfo(
                "qserv-replica-master-http "
                "--config={{db_uri}} "
                "--http-root={{http_root}} "
                "--qserv-czar-db={{qserv_czar_db}} "
                "{% for arg in extended_args %}{{arg}} {% endfor %}"
            ),
        ),
        (
            "replication-registry",
            CommandInfo(
                "qserv-replica-registry "
                "--config={{db_uri}} "
                "{% for arg in extended_args %}{{arg}} {% endfor %}"
            ),
        ),
        ("smig-update", CommandInfo()),
        ("integration-test", CommandInfo()),
        ("integration-test-http", CommandInfo()),
        ("delete-database", CommandInfo()),
        ("load-simple", CommandInfo()),
        ("watcher", CommandInfo()),
        ("prepare-data", CommandInfo()),
        ("spawned-app-help", CommandInfo()),
    )
)


option_cmsd_worker_cfg_file = partial(
    click.option,
    "--cmsd-worker-cfg-file",
    help="Path to the cmsd worker config file.",
    default=cmsd_worker_cfg_template,
    show_default=True,
)


option_cmsd_worker_cfg_path = partial(
    click.option,
    "--cmsd-worker-cfg-path",
    help="Location to render cmsd_worker_cfg_file.",
    default=cmsd_worker_cfg_path,
    show_default=True,
)


option_xrdssi_cfg_file = partial(
    click.option,
    "--xrdssi-cfg-file",
    help="Path to the xrdssi config file.",
    default=xrdssi_cfg_template,
    show_default=True,
)


option_xrdssi_cfg_path = partial(
    click.option,
    "--xrdssi-cfg-path",
    help="Location to render xrdssi-cfg-file.",
    default=xrdssi_cfg_path,
    show_default=True,
)


class EntrypointCommandExArgs(click.Command):
    """Command class for custom entrypoint subcommand behaviors.

    * Provides command support for the "--" option pass-thru arguments; removes
      all args after "--" and puts the args (without the "--") into the context
      for separate handling by the command function. (use the `@pass_context`
      decorator on the command function to be passed the context)
    """

    @dataclass
    class ContextObj:
        """Click allows for the context to have a `obj` parameter, opaque to
        click, for passing data to command functions.
        This is the context object type for EntrypointExtendedArgs.
        (Factor this dataclass and/or command class as you need for greater
        command type polymorphism.)
        """

        extended_args: list[str] = field(default_factory=list)

    def parse_args(self, ctx: click.Context, args: list[str]) -> list[str]:
        """Remove args after "--" and put them in the context, then parse as
        normal.
        """
        separator = "--"
        ctx.obj = self.ContextObj()
        if separator in args:
            if separator in args:
                ctx.obj.extended_args = args[args.index(separator) + 1 :]
                args = args[: args.index(separator)]
        args = super().parse_args(ctx, args)
        return args


class EntrypointCommandGroup(click.Group):
    """Group class for custom entrypoint command behaviors.

    * Provides ordering for list of subcommands in --help
    """

    def list_commands(self, ctx: click.Context) -> list[str]:
        """List the qserv commands in the order specified by help_order.

        Returns
        -------
        commands : Sequence [ str ]
            The list of commands, in the order they should appear in --help.
        """
        # make sure that all the commands are named in our help_order list:
        missing = set(commands.keys()).symmetric_difference(self.commands.keys())
        if missing:
            raise RuntimeError(f"{missing} is found in help_order or commands but not both.")
        return list(commands.keys())


def cmd_default(ctx: click.Context, param: click.core.Option, value: str) -> None:
    """Sets the default value for --cmd for the current function in the
    context's default map.
    """
    if not ctx.command.name:
        return
    ctx.default_map = ctx.default_map or {}
    _log.debug(
        'Changing the %s default_map value for --cmd from "%s" to "%s"',
        ctx.command.name,
        ctx.default_map.get("cmd", "None"),
        default_cmd := commands[ctx.command.name].default_cmd,
    )
    ctx.default_map.update({"cmd": default_cmd})


# option_cmd_default updates the default_map to have the default value for the --cmd option.
option_cmd_default = partial(
    click.option,
    "--cmd-default",
    callback=cmd_default,
    is_eager=True,  # required to show the default in --help
    expose_value=False,
    hidden=True,
)


class options_cms(OptionGroup):  # noqa: N801
    """Applies the option_cmd and the option_cmd_default decorators to a
    click.command function.
    """

    @property
    def decorators(self) -> list[Callable]:
        return [
            option_cmd(),
            option_cmd_default(),
        ]


@click.group(cls=EntrypointCommandGroup)
@option_log_level()
def entrypoint(log_level: str) -> None:
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s %(levelname)-8s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


@entrypoint.command()
@option_repl_auth_key()
@click.argument("repl_ctrl_uri")
@option_load_http()
def load_simple(repl_ctrl_uri: str, repl_auth_key: str, load_http: bool) -> None:
    """Load a small test dataset into qserv.

    REPL_CTRL_URI is the uri to the replication controller.
    """
    script.load_simple(repl_ctrl_uri, auth_key=repl_auth_key, load_http=load_http)


@entrypoint.command()
@option_repl_connection(
    help=option_repl_connection.keywords["help"]
    + " If provided will wait for the replication system to be responsive before "
    "loading data (does not guarantee system readiness)."
)
@option_unload()
@option_load()
@option_reload()
@option_load_http()
@option_case()
@option_run_tests()
@option_tests_yaml()
@option_compare_results()
def integration_test(
    repl_connection: str,
    unload: bool,
    load: bool | None,
    reload: bool,
    load_http: bool,
    cases: list[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
) -> None:
    """Run integration tests using ingested test data.

    TESTS_YAML is the yaml file path that contains connection information and describes tests to load and run.
    """

    results = script.integration_test(
        repl_connection=repl_connection,
        unload=unload,
        load=load,
        reload=reload,
        load_http=load_http,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
    )
    click.echo(str(results))
    sys.exit(0 if results.passed else 1)


@entrypoint.command()
@option_repl_connection(
    help=option_repl_connection.keywords["help"]
    + " If provided will wait for the replication system to be responsive before "
    "loading data (does not guarantee system readiness)."
)
@option_unload()
@option_load()
@option_reload()
@option_load_http()
@option_case()
@option_run_tests()
@option_tests_yaml()
@option_compare_results()
def integration_test_http(
    repl_connection: str,
    unload: bool,
    load: bool | None,
    reload: bool,
    load_http: bool,
    cases: list[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
) -> None:
    """Run integration tests of the HTTP frontend using catalogs loaded into Qserv.

    TESTS_YAML is the yaml file path that contains connection information and describes tests to load and run.
    """

    results = script.integration_test_http(
        repl_connection=repl_connection,
        unload=unload,
        load=load,
        reload=reload,
        load_http=load_http,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
    )
    click.echo(str(results))
    sys.exit(0 if results.passed else 1)


@entrypoint.command()
@option_repl_connection(
    help=option_repl_connection.keywords["help"]
    + " If provided will wait for the replication system to be responsive before "
    "loading data (does not guarantee system readiness)."
)
@option_run_tests()
@option_keep_results()
@option_tests_yaml()
def integration_test_http_ingest(
    repl_connection: str,
    run_tests: bool,
    keep_results: bool,
    tests_yaml: str,
) -> None:
    """Run integration tests of the ingesting user tables via the HTTP frontend.

    TESTS_YAML is the yaml file path that contains connection information and describes tests to load and run.
    """

    results = script.integration_test_http_ingest(
        repl_connection=repl_connection,
        run_tests=run_tests,
        keep_results=keep_results,
        tests_yaml=tests_yaml,
    )
    click.echo(str(results))
    sys.exit(0 if results else 1)


@entrypoint.command()
@option_tests_yaml()
def prepare_data(
    tests_yaml: str,
) -> None:
    """Unzip and partition test datasets.

    TESTS_YAML is the yaml file paths that contains connection information & describes tests to load and run.
    """

    ok = script.prepare_data(
        tests_yaml=tests_yaml,
    )
    click.echo(str(ok))
    sys.exit(0 if ok else 1)


@entrypoint.command()
@click.argument("DATABASE")
@click.argument("repl_ctrl_uri")
@click.option(
    "--admin",
    help="Use the admin insetad of user auth key.",
    is_flag=True,
)
@option_repl_auth_key()
@option_repl_admin_auth_key()
def delete_database(
    repl_ctrl_uri: str,
    database: str,
    admin: bool,
    repl_auth_key: str,
    repl_admin_auth_key: str,
) -> None:
    """Remove a database.

    It is NOT recommended to use this for integration test data; because the
    reference database will not be removed, this only removes a database from
    qserv.

    This deletes a named database from a qserv instance.
    !!Does not verify or ask the user to confirm!!

    In the future we should add a credential check...?

    DATABASE is the name of the database to remove."

    REPL_CTRL_URI is the uri to the replication controller.
    """
    script.delete_database(
        repl_ctrl_uri=repl_ctrl_uri,
        database=database,
        admin=admin,
        auth_key=repl_auth_key,
        admin_auth_key=repl_admin_auth_key,
    )


@entrypoint.command(help=f"Start as a qserv proxy node.\n\n{socket_option_description}")
@pass_context
@option_db_uri(
    help="The non-admin URI to the proxy's database, used for non-smig purposes. " + socket_option_help,
    required=True,
)
@option_db_admin_uri(
    help="The admin URI to the proxy's database, used for schema initialization. " + socket_option_help,
    required=True,
)
@option_mysql_monitor_password()
@option_xrootd_manager(required=True)
@click.option(
    "--proxy-backend-address",
    default="127.0.0.1:3306",
    show_default=True,
    help="This is the same as the proxy-backend-address option to mysql proxy. This value is substitued "
    "into the proxy-backend-address parameter in 'my-proxy.cnf.jinja'.",
)
@click.option(
    "--proxy-cfg-file",
    help="Path to the mysql proxy config file.",
    default=mysql_proxy_cfg_template,
    show_default=True,
)
@click.option(
    "--proxy-cfg-path",
    help="Location to render the mysql proxy config file.",
    default=mysql_proxy_cfg_path,
    show_default=True,
)
@click.option(
    "--czar-cfg-file",
    help="Path to the czar config file.",
    default=czar_cfg_template,
    show_default=True,
)
@click.option(
    "--czar-cfg-path",
    help="Location to render the czar config file.",
    default=czar_cfg_path,
    show_default=True,
)
@option_log_cfg_file()
@option_repl_instance_id(required=True)
@option_repl_auth_key(required=True)
@option_repl_admin_auth_key(required=True)
@option_repl_registry_host(required=True)
@option_repl_registry_port(required=True)
@option_repl_http_port(required=True)
@options_targs()
@options_cms()
@option_options_file()
def proxy(ctx: click.Context, **kwargs: Any) -> None:
    """Start as a qserv-proxy node."""
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_proxy(
        targs=targs,
        db_uri=targs["db_uri"],
        db_admin_uri=targs["db_admin_uri"],
        proxy_backend_address=targs["proxy_backend_address"],
        proxy_cfg_file=targs["proxy_cfg_file"],
        proxy_cfg_path=targs["proxy_cfg_path"],
        czar_cfg_file=targs["czar_cfg_file"],
        czar_cfg_path=targs["czar_cfg_path"],
        cmd=targs["cmd"],
        log_cfg_file=targs["log_cfg_file"],
    )


@entrypoint.command(help=f"Start as a qserv http Czar node.\n\n{socket_option_description}")
@pass_context
@option_db_uri(
    help="The non-admin URI to the Czar's database, used for non-smig purposes. " + socket_option_help,
    required=True,
)
@option_mysql_monitor_password()
@option_xrootd_manager(required=True)
@click.option(
    "--http-port",
    default="4048",
    show_default=True,
    help="The HTTP port of the frontend. The value of the parameter is passed as a command-line"
    " parameter to the application.",
)
@click.option(
    "--http-threads",
    default="2",
    show_default=True,
    help="The number of the request processing threads in the REST service. The value of the parameter is "
    "passed as a command-line parameter to the application.",
)
@click.option(
    "--http-worker-ingest-threads",
    default="2",
    show_default=True,
    help="The number of the request processing threads in the REST service. The value of the parameter is "
    "passed as a command-line parameter to the application.",
)
@click.option(
    "--http-ssl-cert-file",
    help="A location of a file containing an SSL/TSL certificate.",
    default="/config-etc/ssl/czar-cert.pem",
    show_default=True,
)
@click.option(
    "--http-ssl-private-key-file",
    help="A location of a file containing an SSL/TSL private key.",
    default="/config-etc/ssl/czar-key.pem",
    show_default=True,
)
@click.option(
    "--czar-cfg-file",
    help="Path to the czar config file.",
    default=czar_http_cfg_template,
    show_default=True,
)
@click.option(
    "--czar-cfg-path",
    help="Location to render the czar config file.",
    default=czar_cfg_path,
    show_default=True,
)
@click.option(
    "--http-tmp-dir",
    help="The temporary directory for the HTTP server of the frontend.",
    default="/tmp",
    show_default=True,
)
@click.option(
    "--http-conn-pool-size",
    help="A size of a connection pool for synchronous communications over the HTTP"
    " protocol with the Qserv Worker Ingest servbers. The default value is 0,"
    " which assumes that the pool size is determined by an implementation of"
    " the underlying library 'libcurl'. The number of connectons in a production"
    " Qserv deployment should be at least the number of workers in the deployment.",
    default=0,
    show_default=True,
)
@click.option(
    "--czar-name",
    help="The unique name of the Czar instance.",
    default="http",
    show_default=True,
)
@option_log_cfg_file()
@option_repl_instance_id(required=True)
@option_repl_auth_key(required=True)
@option_repl_admin_auth_key(required=True)
@option_repl_registry_host(required=True)
@option_repl_registry_port(required=True)
@option_repl_http_port(required=True)
@options_targs()
@options_cms()
@option_options_file()
def czar_http(ctx: click.Context, **kwargs: Any) -> None:
    """Start as a http-czar node."""
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_czar_http(
        targs=targs,
        db_uri=targs["db_uri"],
        czar_cfg_file=targs["czar_cfg_file"],
        czar_cfg_path=targs["czar_cfg_path"],
        log_cfg_file=targs["log_cfg_file"],
        http_ssl_cert_file=targs["http_ssl_cert_file"],
        http_ssl_private_key_file=targs["http_ssl_private_key_file"],
        cmd=targs["cmd"],
    )


@entrypoint.command()
@pass_context
@click.option(
    "--cms-delay-servers",
    help="Populates 'cms.delay servers' in the cmsd manager config file.",
)
@click.option(
    "--cmsd_manager_cfg_file",
    help="Path to the cmsd manager config file.",
    default=cmsd_manager_cfg_template,
    show_default=True,
)
@click.option(
    "--cmsd-manager-cfg-path",
    help="Location to render cmsd_manager_cfg_file",
    default=cmsd_manager_cfg_path,
    show_default=True,
)
@options_targs()
@options_cms()
@option_options_file()
def cmsd_manager(ctx: click.Context, **kwargs: Any) -> None:
    """Start as a cmsd manager node."""
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_manager_cmsd(
        targs=targs,
        cmsd_manager_cfg_file=targs["cmsd_manager_cfg_file"],
        cmsd_manager_cfg_path=targs["cmsd_manager_cfg_path"],
        cmd=targs["cmd"],
    )


@entrypoint.command()
@pass_context
@option_cmsd_manager_name()
@click.option(
    "--xrootd_manager-cfg-file",
    help="Path to the xrootd manager config file.",
    default=xrootd_manager_cfg_template,
    show_default=True,
)
@click.option(
    "--xrootd-manager-cfg-path",
    help="Location to render xrootd_manager_cfg_file.",
    default=xrootd_manager_cfg_path,
    show_default=True,
)
@options_targs()
@options_cms()
@option_options_file()
def xrootd_manager(ctx: click.Context, **kwargs: Any) -> None:
    """Start as an xrootd manager node."""
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_xrootd_manager(
        targs=targs,
        xrootd_manager_cfg_file=targs["xrootd_manager_cfg_file"],
        xrootd_manager_cfg_path=targs["xrootd_manager_cfg_path"],
        cmd=targs["cmd"],
    )


@entrypoint.command(help=f"Start as a worker cmsd node.\n\n{socket_option_description}")
@pass_context
@option_db_uri(help=worker_db_help)
@option_vnid_config(required=True)
@option_vnid_config(required=True)
@option_repl_instance_id(required=True)
@option_repl_auth_key(required=True)
@option_repl_admin_auth_key(required=True)
@option_repl_registry_host(required=True)
@option_repl_registry_port(required=True)
@option_repl_http_port(required=True)
@option_results_dirname()
@option_results_protocol()
@option_cmsd_manager_name()
@option_debug()
@option_cmsd_worker_cfg_file()
@option_cmsd_worker_cfg_path()
@option_xrdssi_cfg_file()
@option_xrdssi_cfg_path()
@option_log_cfg_file()
@options_targs()
@options_cms()
@option_options_file()
def worker_cmsd(ctx: click.Context, **kwargs: Any) -> None:
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_worker_cmsd(
        targs=targs,
        db_uri=targs["db_uri"],
        cmsd_worker_cfg_file=targs["cmsd_worker_cfg_file"],
        cmsd_worker_cfg_path=targs["cmsd_worker_cfg_path"],
        xrdssi_cfg_file=targs["xrdssi_cfg_file"],
        xrdssi_cfg_path=targs["xrdssi_cfg_path"],
        log_cfg_file=targs["log_cfg_file"],
        cmd=targs["cmd"],
    )


@entrypoint.command(help=f"Start as a worker xrootd node.\n\n{socket_option_description}")
@pass_context
@option_debug()
@option_db_uri(help=worker_db_help)
@option_db_admin_uri(help=admin_worker_db_help)
@option_vnid_config(required=True)
@option_repl_instance_id(required=True)
@option_repl_auth_key(required=True)
@option_repl_admin_auth_key(required=True)
@option_repl_registry_host(required=True)
@option_repl_registry_port(required=True)
@option_repl_http_port(required=True)
@option_results_dirname()
@option_results_protocol()
@option_cmsd_manager_name()
@option_mysql_monitor_password()
@option_db_qserv_user()
@option_cmsd_worker_cfg_file()
@option_cmsd_worker_cfg_path()
@option_xrdssi_cfg_file()
@option_xrdssi_cfg_path()
@option_log_cfg_file()
@options_targs()
@options_cms()
@option_options_file()
def worker_xrootd(ctx: click.Context, **kwargs: Any) -> None:
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_worker_xrootd(
        targs=targs,
        db_uri=targs["db_uri"],
        db_admin_uri=targs["db_admin_uri"],
        cmsd_worker_cfg_file=targs["cmsd_worker_cfg_file"],
        cmsd_worker_cfg_path=targs["cmsd_worker_cfg_path"],
        xrdssi_cfg_file=targs["xrdssi_cfg_file"],
        xrdssi_cfg_path=targs["xrdssi_cfg_path"],
        log_cfg_file=targs["log_cfg_file"],
        cmd=targs["cmd"],
    )


@entrypoint.command(
    help="Start as a replication worker node.\n\n"
    "{socket_option_description}\n\n"
    f"{extended_args_description.format(app='qserv-replica-worker')}",
    cls=EntrypointCommandExArgs,
)
@pass_context
@option_db_admin_uri(
    help="The admin URI to the worker's database, used for replication and ingest. " + socket_option_help
)
@option_repl_connection(help=f"{option_repl_connection.keywords['help']} {socket_option_help}")
@option_debug()
@options_cms()
@click.option(
    "--config",
    help="The path to the configuration database for qserv-replica-worker.",
    default="{{repl_connection}}",
    show_default=True,
)
@option_log_cfg_file()
@options_targs()
@option_run()
@option_options_file()
def worker_repl(ctx: click.Context, **kwargs: Any) -> None:
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_worker_repl(
        db_admin_uri=targs["db_admin_uri"],
        repl_connection=targs["repl_connection"],
        log_cfg_file=targs["log_cfg_file"],
        cmd=targs["cmd"],
        run=targs["run"],
    )


@entrypoint.command(
    help=f"Start as a replication controller node.\n\n{socket_option_description}\n\n"
    f"{extended_args_description.format(app='qserv-replica-master-http')}",
    cls=EntrypointCommandExArgs,
)
@pass_context
@option_db_uri(
    help="The non-admin URI to the replication controller's database, used for non-smig purposes.",
    required=True,
)
@option_db_admin_uri(
    help="The admin URI to the replication controller's database, used for schema initialization. "
    + socket_option_help,
    required=True,
)
@click.option(
    "--xrootd-manager",
    help="The host name of the xrootd manager node.",
)
@option_log_cfg_file()
@options_cms()
@click.option(
    "--http-root",
    help="The root folder for the static content to be served by the built-in HTTP service.",
    default="/usr/local/qserv/www",
    show_default=True,
)
@click.option("--qserv-czar-db", help="The connection URL to the MySQL server of the Qserv master database.")
@options_targs()
@option_run()
@option_options_file()
def replication_controller(ctx: click.Context, **kwargs: Any) -> None:
    """Start as a replication controller node."""
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_replication_controller(
        db_uri=targs["db_uri"],
        db_admin_uri=targs["db_admin_uri"],
        log_cfg_file=targs["log_cfg_file"],
        cmd=targs["cmd"],
        run=targs["run"],
    )


@entrypoint.command(
    help=f"Start as a replication registry node.\n\n{socket_option_description}\n\n"
    f"{extended_args_description.format(app='qserv-replica-registry')}",
    cls=EntrypointCommandExArgs,
)
@pass_context
@option_db_uri(
    help="The non-admin URI to the replication systems's database, used for non-smig purposes.",
    required=True,
)
@option_db_admin_uri(
    help="The admin URI to the proxy's database, used for schema initialization. " + socket_option_help,
    required=True,
)
@option_log_cfg_file()
@options_cms()
@options_targs()
@option_run()
@option_options_file()
def replication_registry(ctx: click.Context, **kwargs: Any) -> None:
    """Start as a replication registry node."""
    targs = utils.targs(ctx)
    targs = render_targs(targs)
    script.enter_replication_registry(
        db_uri=targs["db_uri"],
        db_admin_uri=targs["db_admin_uri"],
        log_cfg_file=targs["log_cfg_file"],
        cmd=targs["cmd"],
        run=targs["run"],
    )


@entrypoint.command()
@click.option(
    "--cluster-id",
    required=True,
    help="The name/identifier of the cluster to show in alerts and log messages.",
)
@click.option(
    "--notify-url-file",
    help="The path to the file that contains the url to receive alerts. "
    "Accepts 'None' to not send notifications (useful for debugging).",
    required=True,
    callback=lambda ctx, par, val: None if val == "None" else val,
)
@click.option(
    "--qserv",
    default="qserv://qsmaster@czar-proxy:4040",
    help="The url to the qserv instance to watch.",
)
@click.option(
    "--timeout-sec",
    help="The threshold time, in seconds, queries taking longer than this will trigger an alert.",
    default=600,  # 10 minutes
)
@click.option(
    "--interval-sec",
    help="How long to wait (in seconds) between checks.",
    default=60,  # 1 minute
)
@click.option(
    "--show-query",
    help="Show the query in alerts.",
    default=False,
    show_default=True,
    is_flag=True,
)
def watcher(
    cluster_id: str,
    notify_url_file: str,
    qserv: str,
    timeout_sec: int,
    interval_sec: int,
    show_query: bool,
) -> None:
    """Run a watcher algorithm that sends notifications if querys appear to stop
    processing."""
    watch(
        cluster_id=cluster_id,
        notify_url_file=notify_url_file,
        qserv=qserv,
        timeout_sec=timeout_sec,
        interval_sec=interval_sec,
        show_query=show_query,
    )


@entrypoint.command()
@option_czar_connection()
@option_worker_connection()
@option_repl_connection()
@option_options_file()
def smig_update(czar_connection: str, worker_connections: list[str], repl_connection: str) -> None:
    """Run schema update on nodes."""
    script.smig_update(
        czar_connection=czar_connection,
        worker_connections=worker_connections,
        repl_connection=repl_connection,
    )


@entrypoint.command()
@click.argument("COMMAND")
def spawned_app_help(
    command: str,
) -> None:
    """Print the help output of a spawned command for a given
    entrypoint subcommand or a message indicating why help is not available.

    Help is available for entrypoint commands that spawn an app. If the named
    command does not exist or is misspelled is misnamed the message will say it
    does not exist. Some entrypoint commands perform work without spawning an
    app, in which case it will say that no spawned app help is available for
    that command.
    """
    if command not in commands:
        click.echo(f"{command} is not an entrypoint command.")
        return
    if (cmd := commands[command].default_cmd) is not None:
        script.spawned_app_help(cmd)
    else:
        click.echo(f"No spawned app help is available for {command}.")
