#!/usr/bin/env python3
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
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""This is the command-line `qserv` command. If it's findable on the PATH then
there will be a CLI command called qserv. Use `qserv --help` to for more
information.
"""


import click
import sys
from typing import List, Optional, Sequence

from cli.options import (
    czar_connection_option,
    pull_option,
    load_option,
    unload_option,
    reload_option,
    run_tests_option,
    compare_results_option,
    case_option,
    repl_connection_option,
    tests_yaml_option,
    worker_connection_option,
)

import launch
import qserv_log

from opt import (
    bind_option,
    build_container_name_option,
    build_image_option,
    cmake_option,
    compose_file_option,
    dashboard_port_option,
    do_build_image_option,
    dry_option,
    itest_container_name_option,
    itest_file_option,
    itest_volume_option,
    jobs_option,
    make_option,
    mariadb_image_ev,
    mariadb_image_option,
    mypy_option,
    project_option,
    pull_image_option,
    push_image_option,
    qserv_default_vals,
    qserv_root_option,
    qserv_build_root_option,
    qserv_env_vals,
    qserv_image_ev,
    qserv_image_option,
    remove_option,
    user_build_image_option,
    user_option,
    run_base_image_option,
    run_base_image_ev,
    test_container_name_option,
    unit_test_option,
)


# This list defines the order of commands output when running "qserv --help".
# If commands are added or removed from from "qserv" then they must be added
# or removed from this list as well.
help_order = [
    "build-build-image",
    "build-user-build-image",
    "build-run-base-image",
    "build-mariadb-image",
    "build",
    "env",
    "up",
    "down",
    "update-schema",
    "itest",
    "itest-rm",
    "run-dev",
    "run-build",
    "run-debug",
    "entrypoint-help"
]


class QservCommandGroup(click.Group):
    """Group class for custom qserv command behaviors."""

    def list_commands(self, ctx: click.Context) -> List[str]:
        """List the qserv commands in the order specified by help_order."""
        # make sure that all the commands are named in our help_order list:
        missing = set(help_order).symmetric_difference(self.commands.keys())
        if missing:
            raise RuntimeError(f"{missing} is found in help_order or commands but not both.")
        return help_order


@click.group(cls=QservCommandGroup)
@click.option(
    qserv_log.log_level_flag,
    default=qserv_log.default_log_level,
    type=click.Choice(qserv_log.log_level_choices, case_sensitive=True),
    help=f"The logging level. Supported levels are [{'|'.join(qserv_log.log_level_choices)}]",
    show_default=True,
    expose_value=False,
)
def qserv() -> None:
    pass


@qserv.command("env")
def show_qserv_environment() -> None:
    """Show qserv environment variables and values."""
    click.echo("Environment variables used for options:")
    click.echo(qserv_env_vals.describe())
    click.echo("Values determined for options:")
    click.echo(qserv_default_vals.describe())


@qserv.command()
@qserv_image_option(help=qserv_image_ev.help("The name and tag of the qserv run image to be built."))
@qserv_root_option()
@qserv_build_root_option()
@run_base_image_option(
    help=run_base_image_ev.help(
        "The name of the lite-run-base image to use as the FROM image for the lite-run image. "
        "Overrides the default lite-run-base image name in the Dockerfile."
    )
)
@user_build_image_option()
@pull_image_option()
@push_image_option()
@click.option(
    "--update-submodules/--no-update-submodules",
    help="Force 'git update submodules --init' to be run, before running cmake. "
    "By default runs if it has not been run yet.",
    is_flag=True,
    default=None,
)
@user_option()
@cmake_option()
@make_option()
@unit_test_option()
@mypy_option()
@do_build_image_option()
@jobs_option()
@dry_option()
def build(
    qserv_root: str,
    qserv_build_root: str,
    unit_test: bool,
    dry: bool,
    jobs: Optional[int],
    run_cmake: bool,
    run_make: bool,
    run_mypy: bool,
    user_build_image: str,
    qserv_image: str,
    run_base_image: str,
    do_build_image: bool,
    push_image: bool,
    pull_image: bool,
    update_submodules: bool,
    user: str,
) -> None:
    "Build qserv and make a new qserv run image."
    launch.build(
        qserv_root=qserv_root,
        qserv_build_root=qserv_build_root,
        unit_test=unit_test,
        dry=dry,
        jobs=jobs,
        run_cmake=run_cmake,
        run_make=run_make,
        run_mypy=run_mypy,
        user_build_image=user_build_image,
        qserv_image=qserv_image,
        run_base_image=run_base_image,
        do_build_image=do_build_image,
        push_image=push_image,
        pull_image=pull_image,
        update_submodules=update_submodules,
        user=user,
    )


@qserv.command()
@build_image_option()
@push_image_option()
@pull_image_option()
@qserv_root_option()
@dry_option()
def build_build_image(
    build_image: str, qserv_root: str, dry: bool, push_image: bool, pull_image: bool
) -> None:
    "Build the qserv lite-build image."
    launch.build_build_image(build_image, qserv_root, dry, push_image, pull_image)


@qserv.command()
@user_build_image_option()
@build_image_option()
@qserv_root_option()
@click.option("--group", help="The name of the user's primary group.")
@dry_option()
def build_user_build_image(
    qserv_root: str, build_image: str, user_build_image: str, group: str, dry: bool
) -> None:
    """Build lite-build with current-user credentials.

    Adds the current user's id and group id to the lite-build image and creates
    an image that will run as the current user. This allows folders & files to
    be bind-mounted in the container without permissions problems.
    """
    launch.build_user_build_image(qserv_root, build_image, user_build_image, group, dry)


@qserv.command()
@run_base_image_option(help=run_base_image_ev.help("The name of the lite-run-base image to create."))
@push_image_option()
@pull_image_option()
@qserv_root_option()
@dry_option()
def build_run_base_image(
    run_base_image: str, qserv_root: str, dry: bool, push_image: bool, pull_image: bool
) -> None:
    "Build the qserv lite-build image."
    launch.build_run_base_image(run_base_image, qserv_root, dry, push_image, pull_image)


@qserv.command()
@mariadb_image_option(help=mariadb_image_ev.help("The name of the mariadb image to create."))
@push_image_option()
@pull_image_option()
@qserv_root_option()
@dry_option()
def build_mariadb_image(
    mariadb_image: str, qserv_root: str, push_image: bool, pull_image: bool, dry: bool
) -> None:
    "Build the qserv lite-mariadb image."
    launch.build_mariadb_image(
        mariadb_image=mariadb_image,
        push_image=push_image,
        pull_image=pull_image,
        qserv_root=qserv_root,
        dry=dry,
    )


@qserv.command()
@qserv_image_option()
@qserv_root_option()
@project_option()
@test_container_name_option()
@bind_option()
@dry_option()
def run_dev(
    qserv_root: str,
    test_container: str,
    qserv_image: str,
    bind: List[str],
    project: str,
    dry: bool,
) -> None:
    """Launch a run container for iterative development.

    If the --dry option is *not* used, then only the container name is
    written to stdout.
    """
    click.echo(launch.run_dev(qserv_root, test_container, qserv_image, bind, project, dry))


@qserv.command()
@qserv_root_option()
@build_container_name_option()
@qserv_build_root_option()
@user_build_image_option()
@user_option()
@dry_option()
def run_build(
    qserv_root: str,
    build_container_name: str,
    qserv_build_root: str,
    user_build_image: str,
    user: str,
    dry: bool,
) -> None:
    "Run and enter a build container."
    launch.run_build(
        qserv_root,
        build_container_name,
        qserv_build_root,
        user_build_image,
        user,
        dry,
    )


@qserv.command()
@click.argument("container_name")
@build_image_option()
@project_option()
@dry_option()
def run_debug(
    container_name: str,
    build_image: str,
    project: str,
    dry: bool,
) -> None:
    """Run and enter a sidecar debug container.

    CONTAINER_NAME is the name of the container to connect to for debugging.
    """
    click.echo(
        launch.run_debug(
            container_name,
            build_image,
            project,
            dry,
        )
    )


@qserv.command()
@qserv_image_option()
@mariadb_image_option(
    help=mariadb_image_ev.help("The name of the database image to use for the reference database.")
)
@qserv_root_option()
@project_option()
@itest_container_name_option()
@itest_volume_option()
@bind_option()
@itest_file_option()
@pull_option()
@load_option()
@unload_option()
@reload_option()
@run_tests_option()
@compare_results_option()
@case_option()
@tests_yaml_option()
@click.option(
    "--wait",
    help="How many seconds to wait before running load and test. "
    "This is useful for allowing qserv to boot if the qserv containers "
    "are started at the same time as this container. "
    f"Default is {click.style('0', fg='green', bold=True)}.",
    default=0,
)
@remove_option()
@dry_option()
def itest(
    qserv_root: str,
    mariadb_image: str,
    itest_container: str,
    itest_volume: str,
    qserv_image: str,
    bind: List[str],
    itest_file: str,
    dry: bool,
    project: str,
    pull: Optional[bool],
    unload: bool,
    load: Optional[bool],
    reload: bool,
    cases: List[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
    wait: int,
    remove: bool,
) -> None:
    """Run integration tests.

    Launches a lite-qserv container and uses it to run integration tests."""
    returncode = launch.itest(
        qserv_root=qserv_root,
        mariadb_image=mariadb_image,
        itest_container=itest_container,
        itest_volume=itest_volume,
        qserv_image=qserv_image,
        bind=bind,
        itest_file=itest_file,
        dry=dry,
        project=project,
        pull=pull,
        unload=unload,
        load=load,
        reload=reload,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
        wait=wait,
        remove=remove,
    )
    sys.exit(returncode)


@qserv.command()
@itest_volume_option()
@dry_option()
def itest_rm(itest_volume: str, dry: bool) -> None:
    """Remove volumes created by itest."""
    launch.itest_rm(itest_volume, dry)


# These defaults match connection options used in
# admin/local/docker/compose/docker-compose.yml
czar_connection_default = "mysql://root:CHANGEME@czar-proxy:3306"
worker_connections_default = [
    "mysql://root:CHANGEME@worker-db-0:3306",
    "mysql://root:CHANGEME@worker-db-1:3306",
]
repl_connection_default = "mysql://root:CHANGEME@repl-mgr-db:3306/qservw_worker"


@qserv.command()
@czar_connection_option(
    default=czar_connection_default,
    help=f"{czar_connection_option.keywords['help']} "
    "The default value works with the default docker-compose file: "
    f"{click.style(czar_connection_default, fg='green', bold=True)}",
)
@worker_connection_option(
    default=worker_connections_default,
    help=f"""{worker_connection_option.keywords['help']}
    The default values work with the default
    {len(worker_connections_default)}-worker docker-compose file:
    {click.style(worker_connections_default, fg='green', bold=True)}""",
)
@repl_connection_option(
    default=repl_connection_default,
    help=f"""{repl_connection_option.keywords['help']}
    The default value works with the default docker-compose file:
    {click.style(repl_connection_default, fg='green', bold=True)}""",
)
@qserv_image_option()
@project_option()
@dry_option()
def update_schema(
    czar_connection: str,
    worker_connections: List[str],
    repl_connection: str,
    qserv_image: str,
    project: str,
    dry: bool,
) -> None:
    """Update the schema on a running qserv instance.

    The typical workflow is to shut down an exsting qserv cluster, install new
    software, and launch qserv. On startup qserv nodes will check for a schema
    version match and if they are behind they will wait in a loop for the schema
    to be updated. This command can be used to update the schema and then nodes
    will continue startup.
    """
    click.echo(
        launch.update_schema(
            czar_connection,
            worker_connections,
            repl_connection,
            qserv_image,
            project,
            dry,
        )
    )


@qserv.command()
@qserv_image_option()
@mariadb_image_option()
@compose_file_option()
@project_option()
@dashboard_port_option()
@dry_option()
def up(
    yaml_file: str,
    dry: bool,
    project: str,
    qserv_image: str,
    mariadb_image: str,
    dashboard_port: int,
) -> None:
    """Launch a docker compose cluster."""
    launch.up(
        yaml_file=yaml_file,
        dry=dry,
        project=project,
        qserv_image=qserv_image,
        mariadb_image=mariadb_image,
        dashboard_port=dashboard_port,
    )


@qserv.command()
@qserv_image_option()
@mariadb_image_option()
@compose_file_option()
@project_option()
@click.option(
    "-v",
    "--volume",
    help="Remove cluster volumes.",
    is_flag=True,
)
@dry_option()
def down(
    yaml_file: str,
    volume: str,
    dry: bool,
    project: str,
    qserv_image: str,
    mariadb_image: str,
) -> None:
    """Bring down a docker compose cluster."""
    launch.down(
        yaml_file=yaml_file,
        volume=volume,
        dry=dry,
        project=project,
        qserv_image=qserv_image,
        mariadb_image=mariadb_image,
    )


@qserv.command()
@click.argument("COMMAND", required=False)
@qserv_image_option()
@dry_option()
def entrypoint_help(
    command: Sequence[str],
    qserv_image: str,
    dry: bool,
) -> None:
    """Show the help output of the entrypoint command inside a run container.

    COMMAND is the entrypoint subcommand to get help for. If not provided, shows
    help output for the entrypoint command.
    """
    launch.entrypoint_help(
        command=command,
        qserv_image=qserv_image,
        dry=dry,
    )
