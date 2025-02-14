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

import getpass
import logging
import os
from functools import partial
from collections.abc import Callable

import click

from .images import get_description

_log = logging.getLogger(__name__)


# The location of the root directory ("qserv root") relative to this file:
relative_qserv_root = "../../../../../../../../"


class EV:
    """Base class for classes that use environment variables to assign values
    and defaults to options.

    Parameters
    ----------
    env_var : `str`
        The environment variable.
    default : `str` or `None`
        The default value if the environment variable is not defined.
    private : `bool`
        True if the value should not be printed or logged.
    """

    def __init__(self, env_var: str, default: str | None, private: bool):
        self.env_var = env_var
        self.default = default
        self.private = private

    def val(self) -> str | None:
        """Get the value of the environment variable or a default value.

        Returns
        -------
        value : `str`
            The value.
        """
        return os.getenv(self.env_var, self.default)

    @property
    def var_val(self) -> str | None:
        """Get the value of the environment variable or return None if the var
        is not defined."""
        return os.getenv(self.env_var, None)


class EnvVal(EV):
    """Defines an environment varaible and defaults that may be used by options.
    Parameters
    ----------
    Inherited from `EV`
    description : `str`
        Description of what the variable is for, used for help output.
    """

    def __init__(self, env_var: str, description: str, private: bool = False):
        super().__init__(env_var, default=None, private=private)
        self.description = description

    @property
    def used_for(self) -> str:
        """Get the description of what the EnvVal is used for, for help output."""
        return self.description

    def val(self) -> str | None:
        """Get the value.

        Returns
        -------
        value : `str` or `None`
            The value of the environment variable or None
        """
        return super().var_val

    def val_with_default(self, default: str) -> str:
        """Get the value.

        Parameters
        ----------
        default : `str`, optional
            The value to return if the environment variable does not exist, by
            default None

        Returns
        -------
        value : `str` or `None`
            The value of the environment variable or the passed-in default
            value.
        """
        return super().var_val or default


class FlagEnvVal(EV):
    """Associates CLI flags that can also be set by environment variables
    and optionally their default value.

    Parameters
    ----------
    Inherited from `EV`
    opt : `str`, Optional
        The option flag.
    """

    def __init__(self, opt: str, env_var: str, default: str | None):
        super().__init__(env_var, default, private=False)
        self.opt = opt

    @property
    def used_for(self) -> str:
        """Get the description of what the EnvVal is used for, for help output."""
        return self.opt

    def help(self, preamble: str) -> str:
        """Get the help text for this item, including boilerplate about how the
        value can be set.

        Parameters
        ----------
        preamble : `str`
            The text that describes the item to put before the boilerplate help
            output.

        Returns
        -------
        help_text : `str`
            The complete help text for this item.
        """
        msg = preamble
        msg += (
            f" Can be set with environment variable {click.style(self.env_var, bold=True)}; current value is "
        )
        var = os.getenv(self.env_var, None)
        if var is not None:
            msg += click.style(var, fg="green", bold=True)
            msg += f". (Default value is {click.style(self.default, fg='blue', bold=True)})."
        else:
            msg += click.style("not set", fg="red", bold=True)
            msg += f". Will use default value: {click.style(self.default, fg='green', bold=True)})."
        return msg


class ImageName:
    """Generate an image name and/or tag based on image type, considering:
    * image type (see `image_types`)
    * the last time an image type's docker file has changed
    * if the QSERV_IMAGE_TAG environment variable is set
    """

    # paths to dockerfiles relative to qserv root:
    base_dockerfile = "admin/tools/docker/base/Dockerfile"
    user_dockerfile = "admin/tools/docker/build-user/Dockerfile"
    run_base_dockerfile = "admin/tools/docker/base/Dockerfile"
    mariadb_dockerfile = "admin/tools/docker/mariadb/Dockerfile"

    image_types = ["qserv", "run-base", "mariadb", "build-base", "build-user"]

    def __init__(self, image: str):
        if image not in self.image_types:
            raise RuntimeError(f"Unexpected image type: {image}")
        self.image = image

    @property
    def tagged_name(self) -> str:
        """Get the image name+tag.

        Returns
        -------
        name_and_tag : `str`
            The image name + tag
        """
        return self.name_with_tag(self.tag)

    def name_with_tag(self, tag: str) -> str:
        """Get the image name+tag using the provided tag.

        Parameters
        ----------
        tag : str
            The tag to add to the image name.

        Returns
        -------
        name_and_tag : `str`
            The image name + tag
        """
        return f"{self.name}:{tag}"

    @property
    def name(self) -> str:
        """Get the image name.

        Returns
        -------
        name : `str`
            The image name
        """
        if self.image == "qserv":
            return "qserv/lite-qserv"
        if self.image == "run-base":
            return "qserv/lite-run-base"
        if self.image == "mariadb":
            return "qserv/lite-mariadb"
        if self.image == "build-base":
            return "qserv/lite-build"
        if self.image == "build-user":
            return f"qserv/lite-build-{getpass.getuser()}"
        raise RuntimeError(f"Invalid image type: {self.image}")

    @property
    def tag(self) -> str:
        """Get the image tag.

        Returns
        -------
        tag : `str`
            The image tag
        """
        qserv_root = env_qserv_root.val()
        if qserv_root is None:
            raise RuntimeError("qserv root was unexpectedly None.")
        return env_image_tag.val_with_default(get_description(self.dockerfiles, qserv_root))

    @property
    def dockerfiles(self) -> list[str] | None:
        """Get the path(s) (relative to qserv root) of the dockerfile(s)
        associated with the current image type.

        Returns
        -------
        dockerfiles : list [str] or None
            The relative paths to the dockerfiles.
        """
        if self.image == "qserv":
            return None
        if self.image == "run-base":
            return [self.run_base_dockerfile]
        if self.image == "mariadb":
            return [self.mariadb_dockerfile]
        if self.image == "build-base":
            return [self.base_dockerfile]
        if self.image == "build-user":
            return [self.base_dockerfile, self.user_dockerfile]
        raise RuntimeError(f"Invalid image type: {self.image}")


env_qserv_root = FlagEnvVal(
    "--qserv-root",
    "QSERV_ROOT",
    os.path.abspath(os.path.join(__file__, relative_qserv_root)),
)
env_image_tag = EnvVal(env_var="QSERV_IMAGE_TAG", description="the tag of all qserv image names")
env_qserv_image = FlagEnvVal(
    "--qserv-image",
    "QSERV_IMAGE",
    ImageName("qserv").tagged_name,
)
env_run_base_image = FlagEnvVal(
    "--run-base-image",
    "QSERV_RUN_BASE_IMAGE",
    ImageName("run-base").tagged_name,
)
env_mariadb_image = FlagEnvVal(
    "--mariadb-image",
    "QSERV_MARIADB_IMAGE",
    ImageName("mariadb").tagged_name,
)
env_build_image = FlagEnvVal(
    "--build-image",
    "QSERV_BUILD_IMAGE",
    ImageName("build-base").tagged_name,
)
env_user_build_image = FlagEnvVal(
    "--user-build-image",
    "QSERV_USER_BUILD_IMAGE",
    ImageName("build-user").tagged_name,
)
# qserv root default is derived by the relative path to the qserv folder from
# the locaiton of this file.
env_qserv_build_root = FlagEnvVal("--qserv-build-root", "QSERV_BUILD_ROOT", "/home/{user}/code/qserv")
env_project = FlagEnvVal("--project", "QSERV_PROJECT", getpass.getuser())
env_outdir = FlagEnvVal("--outdir", "OUTDIR", "/tmp")
env_dashboard_port = FlagEnvVal("--dashboard-port", "QSERV_DASHBOARD_PORT", "25081")
env_http_frontend_port = FlagEnvVal("--http-frontend-port", "QSERV_HTTP_FRONTEND_PORT", "4048")
env_dh_user = EnvVal("QSERV_DH_USER", "CI only; the dockerhub user for pushing and pulling images")
env_dh_token = EnvVal(
    "QSERV_DH_TOKEN",
    "CI only; the dockerhub user token for pushing and pulling images",
    private=True,
)
env_ltd_user = EnvVal("QSERV_LTD_USERNAME", "CI only; the LSST The Docs user for pushing docs.")
env_ltd_password = EnvVal(
    "QSERV_LTD_PASSWORD", "CI only; the LSST The Docs password for pushing docs.", private=True
)
env_gh_event_name = EnvVal(
    "QSERV_GH_EVENT_NAME", "CI only; The name of the event that triggered the GHA workflow."
)
env_gh_head_ref = EnvVal(
    "QSERV_GH_HEAD_REF", "CI only; The head ref or source branch of the pull request in a GHA workflow run."
)
env_gh_ref = EnvVal("QSERV_GH_REF", "CI only; The branch or tag ref that triggered the workflow run.")


class OptDefault:
    """Associates CLI flags with a default value that can be derived using a
    FlagEnvVal environment variable.

    Parameters
    ----------
    opt : `list` [`str`]
        The flag and variable name arguments to click.option
    default : `str` or `None`
        The default value of the value if no environment variable is
        defined, or `None` if there is no default value.
    ev : `FlagEnvVal`
        The `FlagEnvVal` instance that defines the associated environment variable.
    val : `(str) -> `str`
        A callable function that takes the currently defined value of an
        associated `FlagEnvVal` and returns a value derived from it. Will not be
        called if the environment variable is not set.
    """

    def __init__(
        self,
        opt: list[str],
        default: str | None,
        ev: FlagEnvVal,
        val: Callable[[str], str],
    ):
        self.opt = opt
        self.default = default
        self.ev = ev
        self._val = val

    def val(self) -> str | None:
        """Get the value for this default: if the environment variable is
        defined, returns the value derived from that environment variable, or a
        default value (which may be `None`).

        Returns
        -------
        value : `str` or `None`
            The value to use.
        """
        ev_val = self.ev.val()
        if ev_val is None:
            return self.default
        return self._val(ev_val)

    def help(self, preamble: str) -> str:
        """Get the help text for this item, including boilerplate about how the
        value can be set.

        Parameters
        ----------
        preamble : `str`
            The text that describes the item to put before the boilerplate help
            output.

        Returns
        -------
        help_text : `str`
            The complete help text for this item.
        """
        msg = preamble
        if self.default is None:
            msg += " No default value."
        else:
            msg += f" Default is {click.style(self.default, fg='blue', bold=True)}."
        msg += f" Can derive value from from {click.style(self.ev.env_var, bold=True)}."
        msg += f" Current value is {click.style(self.val(), fg='green', bold=True)}."
        return msg


itest_default = OptDefault(
    opt=["--itest-file"],
    default=None,
    ev=env_qserv_root,
    val=lambda ev_val: os.path.join(ev_val, "src/admin/etc/integration_tests.yaml"),
)
itest_container_default = OptDefault(
    opt=["--itest-container"],
    default="itest",
    ev=env_project,
    val=lambda ev_val: f"{ev_val}_itest",
)
itest_ref_container_default = OptDefault(
    opt=["--itest-ref-container"],
    default="itest_ref",
    ev=env_project,
    val=lambda ev_val: f"{ev_val}_itest_ref",
)
itest_http_container_default = OptDefault(
    opt=["--itest-http-container"],
    default="itest_http",
    ev=env_project,
    val=lambda ev_val: f"{ev_val}_itest_http",
)
itest_http_ingest_container_default = OptDefault(
    opt=["--itest-http-ingest-container"],
    default="itest_http_ingest",
    ev=env_project,
    val=lambda ev_val: f"{ev_val}_itest_http_ingest",
)
test_container_default = OptDefault(
    opt=["--test-container"],
    default="test",
    ev=env_project,
    val=lambda ev_val: f"{ev_val}_test",
)
compose_file_default = OptDefault(
    opt=["--file", "yaml_file"],
    default=None,
    ev=env_qserv_root,
    val=lambda ev_val: os.path.join(ev_val, "admin/local/docker/compose/docker-compose.yml"),
)
build_container_default = OptDefault(
    opt=["--build-container-name"],
    default=f"build_container",
    ev=env_project,
    val=lambda ev_val: f"{ev_val}_build",
)


class FlagEnvVals:
    """Container for all the FlagEnvVal instances.

    Parameters
    ----------
    evs : `list` [ `FlagEnvVal` ]
        The `FlagEnvVal` instances used by the qserv command.
    """

    def __init__(self, evs: list[FlagEnvVal | EnvVal]):
        self.evs = evs

    def describe(self) -> str:
        """Get the description of all the FlagEnvVal instances, incluing current and
        default values, for command line display.
        """
        ret = ""
        indent = "  "
        for ev in self.evs:
            ret += indent
            ret += f"{click.style(ev.env_var, bold=True)} "
            ret += f"({click.style('***' if ev.private else ev.var_val) if ev.var_val else click.style('Not defined', fg='red')}) "
            ret += f"for {click.style(ev.used_for)} "
            ret += f"(default: {click.style(ev.default, fg='blue')}) "
            ret += f"is {click.style('***' if ev.private else ev.val(), fg='green', bold=True) if ev.val() else click.style('None', fg='red')}."
            ret += "\n"
        return ret


class DefaultValues:
    """Container for the OptDefault instances.

    Parameters
    ----------
    defaults : `list` [ `OptDefault` ]
        The `OptDefault` instances used by the qserv command.
    """

    def __init__(self, defaults: list[OptDefault]):
        self.defaults = defaults

    def describe(self) -> str:
        """Get the description of all the OptDefault instances, incluing the
        related environment variable and default values, for command line
        display.
        """
        ret = ""
        for default in self.defaults:
            val = default.val()
            ret += f" {default.opt[0]} using {click.style(default.ev.env_var, bold=True)} "
            if val is None:
                ret += f"({click.style('not defined', fg='red')}), "
            else:
                ret += f"is {click.style(default.val(), fg='green', bold=True)} "
            ret += f"(default: {click.style(default.default, fg='blue', bold=True)})"
            ret += "\n"
        return ret


qserv_env_vals = FlagEnvVals(
    [
        env_image_tag,
        env_qserv_image,
        env_build_image,
        env_user_build_image,
        env_run_base_image,
        env_mariadb_image,
        env_qserv_root,
        env_qserv_build_root,
        env_project,
        env_dashboard_port,
        env_http_frontend_port,
        env_dh_user,
        env_dh_token,
        env_ltd_user,
        env_ltd_password,
        env_gh_event_name,
        env_gh_head_ref,
        env_gh_ref,
    ]
)


qserv_default_vals = DefaultValues(
    [
        itest_default,
        compose_file_default,
        # itest_container_default, # need to fix these (nonsensical when QSERV_PROJECT is not set)
        # test_container_default
    ]
)


bind_choices = ["all", "python", "bin", "lib64", "lua", "qserv", "etc"]


option_qserv_image = partial(
    click.option,
    env_qserv_image.opt,
    help=env_qserv_image.help("The name of the qserv image."),
    default=env_qserv_image.val(),
)


option_build_image = partial(
    click.option,
    env_build_image.opt,
    help=env_build_image.help("The name of the qserv build image."),
    default=env_build_image.val(),
)

option_user_build_image = partial(
    click.option,
    env_user_build_image.opt,
    help=env_user_build_image.help("The name of the qserv user build image."),
    default=env_user_build_image.val(),
)


option_user = partial(
    click.option,
    "--user",
    help="The user name to use when running the build container. "
    f"Default will use current user: {click.style(getpass.getuser(), fg='green', bold=True)}",
    default=getpass.getuser(),
)


option_push_image = partial(
    click.option,
    "--push-image",
    help="Push the image to dockerhub if it does not exist. Requires login to dockerhub first.",
    is_flag=True,
)


option_pull_image = partial(
    click.option,
    "--pull-image",
    help="Pull the image from dockerhub if it exists.",
    is_flag=True,
)


option_run_base_image = partial(
    click.option,
    env_run_base_image.opt,
    help=env_run_base_image.help("The name of the run base image."),
    default=env_run_base_image.val(),
)


option_mariadb_image = partial(
    click.option,
    env_mariadb_image.opt,
    help=env_mariadb_image.help("The name of the mariadb image."),
    default=env_mariadb_image.val(),
)


option_qserv_root = partial(
    click.option,
    env_qserv_root.opt,
    help=env_qserv_root.help(
        "Location of the qserv sources folder outside of the build container. "
        "Default location is relative to the qserv command file."
    ),
    envvar=env_qserv_root.env_var,
    default=env_qserv_root.default,
    required=True,
)


option_qserv_build_root = partial(
    click.option,
    env_qserv_build_root.opt,
    help=env_qserv_build_root.help("Location of the qserv sources folder inside the build container."),
    default=env_qserv_build_root.default,
)

option_outdir = partial(
    click.option,
    env_outdir.opt,
    help=env_outdir.help(
        "Location of the folder that will contain unzipped and partitioned integration test datasets"
    ),
    envvar=env_outdir.env_var,
    default=env_outdir.default,
)


option_qserv_group = partial(
    click.option,
    "--group",
    help="The name of the user's primary group.",
)


option_dashboard_port = partial(
    click.option,
    env_dashboard_port.opt,
    help=env_dashboard_port.help("The host port to use for the qserv dashboard."),
    default=env_dashboard_port.val(),
)


option_http_frontend_port = partial(
    click.option,
    env_http_frontend_port.opt,
    help=env_http_frontend_port.help("The host port to use for the qserv HTTP frontend."),
    default=env_http_frontend_port.val(),
)


option_unit_test = partial(
    click.option,
    "--unit-test/--no-unit-test",
    help="Run unit tests. Default: --unit-test",
    is_flag=True,
    default=True,
)


option_dry = partial(
    click.option,
    "-d",
    "--dry",
    help="Do not run the command, instead print the action to the command line.",
    is_flag=True,
)


option_jobs = partial(
    click.option,
    "-j",
    "--jobs",
    help="Same as the gnu make -j option; the number of make recipies to execute at once.",
    type=int,
)


option_project = partial(
    click.option,
    env_project.opt,
    help=env_project.help("The project name for the qserv docker-compose instance."),
    default=env_project.val(),
    callback=lambda ctx, par, val: None if val == "None" else val,
)


option_compose_file = partial(
    click.option,
    *compose_file_default.opt,
    help=compose_file_default.help("The location of the yaml file that describes the compose cluster."),
    default=compose_file_default.val(),
    required=True,
)


option_itest_container_name = partial(
    click.option,
    *itest_container_default.opt,
    help=itest_container_default.help("The name to give the integration test container."),
    default=itest_container_default.val(),
    required=True,
)


option_itest_ref_container_name = partial(
    click.option,
    *itest_ref_container_default.opt,
    help=itest_ref_container_default.help("The name to give the integration test reference db container."),
    default=itest_ref_container_default.val(),
    required=True,
)


option_itest_file = partial(
    click.option,
    *itest_default.opt,
    help=itest_default.help("Path to an yaml file that describes how to run the integration tests."),
    default=itest_default.val(),
    required=True,
)


option_itest_http_container_name = partial(
    click.option,
    *itest_http_container_default.opt,
    help=itest_http_container_default.help(
        "The name to give the integration test container for HTTP frontend."
    ),
    default=itest_http_container_default.val(),
    required=True,
)

option_itest_http_ingest_container_name = partial(
    click.option,
    *itest_http_ingest_container_default.opt,
    help=itest_http_ingest_container_default.help(
        "The name to give the integration test container for testing user table ingest via the HTTP frontend."
    ),
    default=itest_http_ingest_container_default.val(),
    required=True,
)


option_test_container_name = partial(
    click.option,
    *test_container_default.opt,
    help=test_container_default.help("The name to give the test container."),
    default=test_container_default.val(),
    required=True,
)


option_bind = partial(
    click.option,
    "-b",
    "--bind",
    help="Build artifact location(s) to be bind mounted into the container. "
    "Allows for local iterative build & test without having to rebuild the docker image. "
    "'all' includes all locations.",
    multiple=True,
    type=click.Choice(bind_choices),
)


option_cmake = partial(
    click.option,
    "--cmake/--no-cmake",
    "run_cmake",
    help="Force cmake to run or not run, before running make. By default runs cmake if the build folder does not exist yet.",
    is_flag=True,
    default=None,
)


option_make = partial(
    click.option,
    "--make/--no-make",
    "run_make",
    help="Run make before creating the image. Default: --make",
    default=True,
    is_flag=True,
)


option_do_build_image = partial(
    click.option,
    "--do-build-image/--no-build-image",
    help="Build the run image after running cmake and make. Default: --do-build-image",
    default=True,
    is_flag=True,
)


option_build_container_name = partial(
    click.option,
    *build_container_default.opt,
    help=build_container_default.help("The name to give the build container."),
    default=build_container_default.val(),
)


option_remove = partial(
    click.option,
    "--remove/--no-remove",
    help="Remove docker container(s) after execution.",
    default=True,
)


option_mypy = partial(
    click.option,
    "--mypy/--no-mypy",
    "run_mypy",
    help="Run mypy on python files.",
    default=True,
)


option_clang_format = partial(
    click.option,
    "--clang-format",
    "clang_format_mode",
    type=click.Choice(["CHECK", "REFORMAT", "OFF"], case_sensitive=False),
    callback=lambda ctx, par, val: val.lower(),
    help="If CHECK, check C++ files with clang-format and fail if any changes are needed. "
    "If REFORMAT, run clang-format on C++ files (will reformat files)."
    "If OFF, do not run clang-format.",
    default="OFF",
)


option_debuggable = partial(
    click.option,
    "--debuggable/--no-debuggable",
    help="Run the container with permissions to enable debugging.",
    default=True,
    show_default=True,
)
