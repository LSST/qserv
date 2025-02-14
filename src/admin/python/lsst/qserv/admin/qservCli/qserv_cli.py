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

import sys

import click

from ..cli.options import (
    option_case,
    option_compare_results,
    option_czar_connection,
    option_keep_results,
    option_load,
    option_load_http,
    option_log_level,
    option_reload,
    option_repl_connection,
    option_run_tests,
    option_tests_yaml,
    option_unload,
    option_worker_connection,
)
from . import images, launch
from .opt import (
    env_build_image,
    env_dh_token,
    env_dh_user,
    env_gh_event_name,
    env_gh_head_ref,
    env_gh_ref,
    env_ltd_password,
    env_ltd_user,
    env_mariadb_image,
    env_qserv_image,
    env_run_base_image,
    env_user_build_image,
    option_bind,
    option_build_container_name,
    option_build_image,
    option_clang_format,
    option_cmake,
    option_compose_file,
    option_dashboard_port,
    option_debuggable,
    option_do_build_image,
    option_dry,
    option_http_frontend_port,
    option_itest_container_name,
    option_itest_file,
    option_itest_http_container_name,
    option_itest_http_ingest_container_name,
    option_itest_ref_container_name,
    option_jobs,
    option_make,
    option_mariadb_image,
    option_mypy,
    option_outdir,
    option_project,
    option_pull_image,
    option_push_image,
    option_qserv_build_root,
    option_qserv_group,
    option_qserv_image,
    option_qserv_root,
    option_remove,
    option_run_base_image,
    option_test_container_name,
    option_unit_test,
    option_user,
    option_user_build_image,
    qserv_default_vals,
    qserv_env_vals,
)

# This list defines the order of commands output when running "qserv --help".
# If commands are added or removed from from "qserv" then they must be added
# or removed from this list as well.
help_order = [
    "build-build-image",
    "build-user-build-image",
    "build-run-base-image",
    "build-mariadb-image",
    "build-images",
    "build",
    "build-docs",
    "env",
    "up",
    "down",
    "update-schema",
    "itest",
    "itest-http",
    "itest-http-ingest",
    "itest-rm",
    "prepare-data",
    "run-dev",
    "run-build",
    "run-debug",
    "entrypoint-help",
    "dh-image-exists",
]


class QservCommandGroup(click.Group):
    """Group class for custom qserv command behaviors."""

    def list_commands(self, ctx: click.Context) -> list[str]:
        """List the qserv commands in the order specified by help_order."""
        # make sure that all the commands are named in our help_order list:
        missing = set(help_order).symmetric_difference(self.commands.keys())
        if missing:
            raise RuntimeError(f"{missing} is found in help_order or commands but not both.")
        return help_order


@click.group(cls=QservCommandGroup)
@option_log_level(
    expose_value=False,
)
def qserv() -> None:
    pass


@qserv.command("env")
@click.option(env_qserv_image.opt, is_flag=True)
@click.option(env_build_image.opt, is_flag=True)
@click.option(env_user_build_image.opt, is_flag=True)
@click.option(env_run_base_image.opt, is_flag=True)
@click.option(env_mariadb_image.opt, is_flag=True)
def show_qserv_environment(
    qserv_image: bool,
    build_image: bool,
    user_build_image: bool,
    run_base_image: bool,
    mariadb_image: bool,
) -> None:
    """Show qserv environment variables and default option values.

    If any of the option flags are passed, only the default/environment-based
    value for each of those flags is returned. This can be used with github
    actions or other scripted tools.
    """
    if qserv_image:
        click.echo(env_qserv_image.val())
    if build_image:
        click.echo(env_build_image.val())
    if user_build_image:
        click.echo(env_user_build_image.val())
    if run_base_image:
        click.echo(env_run_base_image.val())
    if mariadb_image:
        click.echo(env_mariadb_image.val())
    if any((qserv_image, build_image, user_build_image, run_base_image, mariadb_image)):
        return
    click.echo("Environment variables used for options:")
    click.echo(qserv_env_vals.describe())
    click.echo("Values determined for options:")
    click.echo(qserv_default_vals.describe())


@qserv.command()
@option_qserv_image(help=env_qserv_image.help("The name and tag of the qserv run image to be built."))
@option_qserv_root()
@option_qserv_build_root()
@option_run_base_image(
    help=env_run_base_image.help(
        "The name of the lite-run-base image to use as the FROM image for the lite-run image. "
        "Overrides the default lite-run-base image name in the Dockerfile."
    )
)
@option_user_build_image()
@option_pull_image()
@option_push_image()
@option_user()
@option_cmake()
@option_make()
@option_unit_test()
@option_mypy()
@option_clang_format()
@option_do_build_image()
@option_jobs()
@option_dry()
def build(
    qserv_root: str,
    qserv_build_root: str,
    unit_test: bool,
    dry: bool,
    jobs: int | None,
    run_cmake: bool,
    run_make: bool,
    run_mypy: bool,
    clang_format_mode: str,
    user_build_image: str,
    qserv_image: str,
    run_base_image: str,
    do_build_image: bool,
    push_image: bool,
    pull_image: bool,
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
        clang_format_mode=clang_format_mode,
        user_build_image=user_build_image,
        qserv_image=qserv_image,
        run_base_image=run_base_image,
        do_build_image=do_build_image,
        push_image=push_image,
        pull_image=pull_image,
        user=user,
    )


@qserv.command(
    help=f"""Build the qserv documentation.

    Passing --upload will cause the docs to be uploaded to LSST The Docs (LTD).
    Typically this is only used by Github Actions (GHA)

    If using --upload, the following environment variables must be set:

    * {env_ltd_user.env_var} - {env_ltd_user.used_for}

    * {env_ltd_password.env_var} - {env_ltd_password.used_for}

    * {env_gh_event_name.env_var} - {env_gh_event_name.used_for}

    * {env_gh_head_ref.env_var} - {env_gh_head_ref.used_for}

    * {env_gh_ref.env_var} - {env_gh_ref.used_for}

    See the LSST The Docs Conveyor help about the LTD_* environment variables,
    at https://ltd-conveyor.lsst.io.

    See the Github Actions help about environment variables for more information about the
    GITHUB_* environment variables.
    """
)
@option_qserv_root()
@option_qserv_build_root()
@option_user_build_image()
@option_user()
@click.option(
    "--upload/--no-upload",
    help="Upload the documentation.",
    show_default=True,
)
@click.option(
    "--linkcheck/--no-linkcheck",
    help="Test the links in documentation to make sure they are valid (includes all external links).",
    default=False,
    show_default=True,
)
@option_cmake()
@option_dry()
def build_docs(
    upload: bool,
    qserv_root: str,
    qserv_build_root: str,
    user_build_image: str,
    user: str,
    linkcheck: bool,
    run_cmake: bool,
    dry: bool,
) -> None:
    launch.build_docs(
        upload=upload,
        ltd_user=env_ltd_user.val(),
        ltd_password=env_ltd_password.val(),
        gh_event=env_gh_event_name.val(),
        gh_head_ref=env_gh_head_ref.val(),
        gh_ref=env_gh_ref.val(),
        qserv_root=qserv_root,
        qserv_build_root=qserv_build_root,
        build_image=user_build_image,
        user=user,
        linkcheck=linkcheck,
        run_cmake=run_cmake,
        dry=dry,
    )


@qserv.command()
@option_build_image(help=env_build_image.help("The name of the build base image to create."))
@option_push_image()
@option_pull_image()
@option_qserv_root()
@option_dry()
def build_build_image(
    build_image: str, qserv_root: str, dry: bool, push_image: bool, pull_image: bool
) -> None:
    "Build the qserv lite-build image."
    launch.build_build_image(build_image, qserv_root, dry, push_image, pull_image)


@qserv.command()
@option_user_build_image()
@option_build_image()
@option_qserv_root()
@click.option("--group", help="The name of the user's primary group.")
@option_qserv_group()
@option_dry()
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
@option_run_base_image(help=env_run_base_image.help("The name of the lite-run-base image to create."))
@option_push_image()
@option_pull_image()
@option_qserv_root()
@option_dry()
def build_run_base_image(
    run_base_image: str, qserv_root: str, dry: bool, push_image: bool, pull_image: bool
) -> None:
    "Build the qserv lite-build image."
    launch.build_run_base_image(run_base_image, qserv_root, dry, push_image, pull_image)


@qserv.command()
@option_mariadb_image(help=env_mariadb_image.help("The name of the mariadb image to create."))
@option_push_image()
@option_pull_image()
@option_qserv_root()
@option_dry()
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
@option_build_image(help=env_build_image.help("The name of the build base image to create."))
@option_user_build_image(help=env_run_base_image.help("The name of the lite-run-base image to create."))
@option_qserv_group()
@option_run_base_image(help=env_run_base_image.help("The name of the lite-run-base image to create."))
@option_mariadb_image(help=env_mariadb_image.help("The name of the mariadb image to create."))
@option_push_image(
    help="Push base images to dockerhub if they do not exist. Requires login to dockerhub first."
)
@option_pull_image(help="Pull images from dockerhub if they exist.")
@option_qserv_root()
@option_dry()
def build_images(
    build_image: str,
    user_build_image: str,
    group: str,
    run_base_image: str,
    mariadb_image: str,
    push_image: bool,
    pull_image: bool,
    qserv_root: str,
    dry: bool,
) -> None:
    """Build or pull the non-run qserv images.

    Builds or pulls the build base, run base, and mariadb images.
    Builds the user build image.
    """
    launch.build_build_image(build_image, qserv_root, dry, push_image, pull_image)
    launch.build_user_build_image(qserv_root, build_image, user_build_image, group, dry)
    launch.build_run_base_image(run_base_image, qserv_root, dry, push_image, pull_image)
    launch.build_mariadb_image(mariadb_image, qserv_root, dry, push_image, pull_image)


@qserv.command()
@option_qserv_image()
@option_qserv_root()
@option_project()
@option_test_container_name()
@option_bind()
@option_dry()
def run_dev(
    qserv_root: str,
    test_container: str,
    qserv_image: str,
    bind: list[str],
    project: str,
    dry: bool,
) -> None:
    """Launch a run container for iterative development.

    If the --dry option is *not* used, then only the container name is
    written to stdout.
    """
    click.echo(launch.run_dev(qserv_root, test_container, qserv_image, bind, project, dry))


@qserv.command()
@option_qserv_root()
@option_build_container_name()
@option_qserv_build_root()
@option_user_build_image()
@option_user()
@option_debuggable()
@click.option(
    "--mode",
    type=click.Choice(("long-lived", "temp")),
    help="Indicates the container lifecycle.\n\n"
    "* 'long-lived' for long-lived container; runs and does not enter the container.\n\n"
    "* 'temp' for a temporary container; runs and enters container, removes container when exiting.\n\n",
    default="temp",
    show_default=True,
)
@option_dry()
def run_build(
    qserv_root: str,
    build_container_name: str,
    qserv_build_root: str,
    user_build_image: str,
    user: str,
    debuggable: bool,
    mode: str,
    dry: bool,
) -> None:
    "Run and enter a build container."
    launch.run_build(
        qserv_root,
        build_container_name,
        qserv_build_root,
        user_build_image,
        user,
        debuggable,
        mode,
        dry,
    )


@qserv.command()
@click.argument("container_name")
@option_build_image()
@option_project()
@option_dry()
def run_debug(
    container_name: str,
    build_image: str,
    project: str,
    dry: bool,
) -> None:
    """Run and enter a sidecar debug container.

    CONTAINER_NAME is the name of the container to connect to for debugging.
    """
    launch.run_debug(
        container_name,
        build_image,
        project,
        dry,
    )


@qserv.command()
@option_qserv_image()
@option_mariadb_image(
    help=env_mariadb_image.help("The name of the database image to use for the reference database.")
)
@option_qserv_root()
@option_project()
@option_itest_container_name()
@option_itest_ref_container_name()
@option_bind()
@option_itest_file()
@option_load()
@option_unload()
@option_reload()
@option_load_http()
@option_run_tests()
@option_compare_results()
@option_case()
@option_tests_yaml()
@click.option(
    "--wait",
    help="How many seconds to wait before running load and test. "
    "This is useful for allowing qserv to boot if the qserv containers "
    "are started at the same time as this container. "
    f"Default is {click.style('0', fg='green', bold=True)}.",
    default=0,
)
@option_remove()
@option_dry()
def itest(
    qserv_root: str,
    mariadb_image: str,
    itest_container: str,
    itest_ref_container: str,
    qserv_image: str,
    bind: list[str],
    itest_file: str,
    dry: bool,
    project: str,
    unload: bool,
    load: bool | None,
    reload: bool,
    load_http: bool,
    cases: list[str],
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
        itest_ref_container=itest_ref_container,
        qserv_image=qserv_image,
        bind=bind,
        itest_file=itest_file,
        dry=dry,
        project=project,
        unload=unload,
        load=load,
        reload=reload,
        load_http=load_http,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
        wait=wait,
        remove=remove,
    )
    sys.exit(returncode)


@qserv.command()
@option_qserv_image()
@option_mariadb_image(
    help=env_mariadb_image.help("The name of the database image to use for the reference database.")
)
@option_qserv_root()
@option_project()
@option_itest_http_container_name()
@option_itest_ref_container_name()
@option_bind()
@option_itest_file()
@option_load()
@option_unload()
@option_reload()
@option_load_http()
@option_run_tests()
@option_compare_results()
@option_case()
@option_tests_yaml()
@click.option(
    "--wait",
    help="How many seconds to wait before running load and test. "
    "This is useful for allowing qserv to boot if the qserv containers "
    "are started at the same time as this container. "
    f"Default is {click.style('0', fg='green', bold=True)}.",
    default=0,
)
@option_remove()
@option_dry()
def itest_http(
    qserv_root: str,
    mariadb_image: str,
    itest_http_container: str,
    itest_ref_container: str,
    qserv_image: str,
    bind: list[str],
    itest_file: str,
    dry: bool,
    project: str,
    unload: bool,
    load: bool | None,
    reload: bool,
    load_http: bool,
    cases: list[str],
    run_tests: bool,
    tests_yaml: str,
    compare_results: bool,
    wait: int,
    remove: bool,
) -> None:
    """Run integration tests for the HTTP frontend.

    Launches a lite-qserv container and uses it to run integration tests."""
    returncode = launch.itest_http(
        qserv_root=qserv_root,
        mariadb_image=mariadb_image,
        itest_http_container=itest_http_container,
        itest_ref_container=itest_ref_container,
        qserv_image=qserv_image,
        bind=bind,
        itest_file=itest_file,
        dry=dry,
        project=project,
        unload=unload,
        load=load,
        reload=reload,
        load_http=load_http,
        cases=cases,
        run_tests=run_tests,
        tests_yaml=tests_yaml,
        compare_results=compare_results,
        wait=wait,
        remove=remove,
    )
    sys.exit(returncode)


@qserv.command()
@option_qserv_image()
@option_qserv_root()
@option_project()
@option_itest_http_ingest_container_name()
@option_bind()
@option_itest_file()
@option_run_tests()
@option_keep_results()
@option_tests_yaml()
@click.option(
    "--wait",
    help="How many seconds to wait before running load and test. "
    "This is useful for allowing qserv to boot if the qserv containers "
    "are started at the same time as this container. "
    f"Default is {click.style('0', fg='green', bold=True)}.",
    default=0,
)
@option_remove()
@option_dry()
def itest_http_ingest(
    qserv_root: str,
    itest_http_ingest_container: str,
    qserv_image: str,
    bind: list[str],
    itest_file: str,
    dry: bool,
    project: str,
    run_tests: bool,
    keep_results: bool,
    tests_yaml: str,
    wait: int,
    remove: bool,
) -> None:
    """Run integration tests for ingesting user tables via the HTTP frontend.

    Launches a lite-qserv container and uses it to run integration tests."""
    returncode = launch.itest_http_ingest(
        qserv_root=qserv_root,
        itest_http_ingest_container=itest_http_ingest_container,
        qserv_image=qserv_image,
        bind=bind,
        itest_file=itest_file,
        dry=dry,
        project=project,
        run_tests=run_tests,
        keep_results=keep_results,
        tests_yaml=tests_yaml,
        wait=wait,
        remove=remove,
    )
    sys.exit(returncode)


@qserv.command()
@option_project()
@option_dry()
def itest_rm(project: str, dry: bool) -> None:
    """Remove volumes created by itest."""
    launch.itest_rm(project, dry)


@qserv.command()
@option_qserv_image()
@option_qserv_root()
@option_project()
@option_itest_container_name()
@option_itest_file()
@option_outdir()
@option_dry()
def prepare_data(
    qserv_root: str,
    itest_container: str,
    qserv_image: str,
    itest_file: str,
    outdir: str,
    dry: bool,
    project: str,
) -> None:
    """Unzip and partition integration tests datasets.

    Launches a lite-qserv container and uses it to prepare integration tests datasets."""
    returncode = launch.prepare_data(
        qserv_root=qserv_root,
        itest_container=itest_container,
        qserv_image=qserv_image,
        itest_file=itest_file,
        outdir=outdir,
        dry=dry,
        project=project,
    )
    sys.exit(returncode)


# These defaults match connection options used in
# admin/local/docker/compose/docker-compose.yml
czar_connection_default = "mysql://root:CHANGEME@czar-proxy:3306"
worker_connections_default = [
    "mysql://root:CHANGEME@worker-db-0:3306",
    "mysql://root:CHANGEME@worker-db-1:3306",
]
repl_connection_default = "mysql://root:CHANGEME@repl-mgr-db:3306/qservw_worker"


@qserv.command()
@option_czar_connection(
    default=czar_connection_default,
    help=f"{option_czar_connection.keywords['help']} "
    "The default value works with the default docker-compose file: "
    f"{click.style(czar_connection_default, fg='green', bold=True)}",
)
@option_worker_connection(
    default=worker_connections_default,
    help=f"""{option_worker_connection.keywords["help"]}
    The default values work with the default
    {len(worker_connections_default)}-worker docker-compose file:
    {click.style(worker_connections_default, fg="green", bold=True)}""",
)
@option_repl_connection(
    default=repl_connection_default,
    help=f"""{option_repl_connection.keywords["help"]}
    The default value works with the default docker-compose file:
    {click.style(repl_connection_default, fg="green", bold=True)}""",
)
@option_qserv_image()
@option_project()
@option_dry()
def update_schema(
    czar_connection: str,
    worker_connections: list[str],
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
    launch.update_schema(
        czar_connection,
        worker_connections,
        repl_connection,
        qserv_image,
        project,
        dry,
    )


@qserv.command()
@option_qserv_image()
@option_mariadb_image()
@option_compose_file()
@option_project()
@option_dashboard_port()
@option_http_frontend_port()
@option_dry()
def up(
    yaml_file: str,
    dry: bool,
    project: str,
    qserv_image: str,
    mariadb_image: str,
    dashboard_port: int,
    http_frontend_port: int,
) -> None:
    """Launch a docker compose cluster."""
    launch.up(
        yaml_file=yaml_file,
        dry=dry,
        project=project,
        qserv_image=qserv_image,
        mariadb_image=mariadb_image,
        dashboard_port=dashboard_port,
        http_frontend_port=http_frontend_port,
    )


@qserv.command()
@option_qserv_image()
@option_mariadb_image()
@option_compose_file()
@option_project()
@click.option(
    "-v",
    "--volume",
    help="Remove cluster volumes.",
    is_flag=True,
)
@option_dry()
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
@option_qserv_image()
@click.option(
    "--entrypoint/--no-entrypoint",
    is_flag=True,
    default=True,
    help="Show help output for the entrypoint command.",
)
@click.option(
    "--spawned/--no-spawned",
    is_flag=True,
    default=True,
    help="Show help output for the spawned app.",
)
@option_dry()
def entrypoint_help(
    command: str | None,
    qserv_image: str,
    entrypoint: bool,
    spawned: bool,
    dry: bool,
) -> None:
    """Show the entrypoint CLI help output.

    COMMAND is the entrypoint subcommand to get help for. If not provided, shows
    help output for the entrypoint command.

    Shows help output for the entrypoint subcommand. If the subcommand spawns
    another process, shows help output from the app that the subcommand spawns.
    """
    launch.entrypoint_help(
        command=command,
        qserv_image=qserv_image,
        entrypoint=entrypoint,
        spawned=spawned,
        dry=dry,
    )


@qserv.command(
    help=f"""Check if an image is in dockerhub.

    IMAGE is the image name + tag.

    Mostly this is useful for CI, so base image builds can be skipped for
    base images that are already already on dockerhub.

    Dockerhub credentials must be provided in the environment variables
    QSERV_DH_USER and QSERV_DH_TOKEN.
    """
)
@click.argument("IMAGE")
def dh_image_exists(image: str) -> None:
    user = env_dh_user.val()
    token = env_dh_token.val()
    if not (user and token):
        click.echo("QSERV_DH_USER and QSERV_DH_TOKEN must be set to use this command.")
        return
    click.echo(
        images.dh_image_exists(
            image,
            user,
            token,
        )
    )
