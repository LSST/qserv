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


import logging
import os
import stat
from typing import Any

import jinja2
import yaml

from .cli.utils import Targs

_log = logging.getLogger(__name__)


default_cfg_file_path = "/config-etc/qserv-template.cfg"

cfg_file_path = os.environ.get("QSERV_CFG", default_cfg_file_path)


def save_template_cfg(values: Targs) -> None:
    """Save a dict of key-value pairs to the template config parameter file.

    Parameters
    ----------
    values : `dict` [`str`, `str`]
        Key-value pairs to add.
    """
    if not values:
        return
    try:
        with open(cfg_file_path) as f:
            cfg = yaml.safe_load(f.read())
    except FileNotFoundError:
        cfg = {}
    cfg.update(values)
    with open(cfg_file_path, "w") as f:
        f.write(yaml.dump(cfg))


def get_template_cfg() -> dict[Any, Any]:
    """Get the dict of key-value pairs from the config parameter file."""
    try:
        with open(cfg_file_path) as f:
            cfg = yaml.safe_load(f.read())
    except FileNotFoundError:
        cfg = {}
    if not isinstance(cfg, dict):
        raise RuntimeError(f"Expected {cfg_file_path} to contain a yaml dictionary.")
    return cfg


def apply_template_cfg(template: str, targs: Targs | None = None) -> str:
    """Apply template values as found in the config parameter file to a
    template.

    Parameters
    ----------
    template : `str`
        A template string with jinja-style templating.

    Returns
    -------
    result : `str`
        The result of having applied template values to the template.
    targs : `dict` [`str`, `str`], optional
        The template arguments to use. If None, will read from the config
        parameter file.

    Raises
    ------
    jinja2.exceptions.UndefinedError
        If any keys in the template are not found in the values.
    """
    t = jinja2.Template(
        template,
        undefined=jinja2.StrictUndefined,
    )
    try:
        if targs:
            return t.render(targs)
        else:
            return t.render(**get_template_cfg())
    except jinja2.exceptions.UndefinedError as e:
        _log.error(f"Missing template value: {e!s}")
        raise


def apply_template_cfg_file(src: str, dest: str, targs: Targs | None = None) -> None:
    """Open and read a template file, apply the config values to it, and write
    the rendered version to a file.

    Parameters
    ----------
    src : `str`
        The path to the template file.
    dest : `str`
        The path to the file where the rendered template should be written.
    targs : `dict` [`str`, `str`], optional
        The template arguments to use. If None, will read from the config
        parameter file.
    """
    with open(src) as s:
        try:
            rendered = apply_template_cfg(s.read(), targs)
        except jinja2.exceptions.UndefinedError as e:
            raise RuntimeError(f"Failure rendering {src}.") from e
    with open(dest, "w") as d:
        d.write(rendered)
    os.chmod(dest, stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP | stat.S_IWGRP)
