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

"""Utilities for CLI functions in qserv."""

import copy
import logging
import os
import traceback
from typing import Any, Dict, List, Sequence, Tuple, Union, cast

import click
import click.testing
import yaml

_log = logging.getLogger(__name__)


Targs = Dict[str, Any]


def split_kv(values: Sequence[str]) -> Dict[str, str]:
    """Split muliple groups of comma-separated key=value pairs into a dict.

    Parameters
    ----------
    values : `Sequence` [`str`]
        Each item in the list should be a string in the form "key=value" or
        "key=value,key=value,..."

    Returns
    -------
    items : `Dict` [`str`, `str`]
        The separated key-value pairs.
    """
    if not values:
        return {}
    # combine all the (possibly comma-separated) values into one comma-separated
    # string, and then split on comma:
    pairs = ",".join(values).split(",")
    # verify each pair has exactly one equal sign
    for pair in pairs:
        if pair.count("=") != 1:
            raise RuntimeError(f"Each key-value pair must be separated by '='.")
    # split each pair on the equal sign:
    split_pairs = (cast(Tuple[str, str], pair.split("=")) for pair in pairs)
    # and finally, make a dict:
    return dict(split_pairs)


def yaml_presets(ctx: click.Context, param: click.core.Option, value: str) -> None:
    """Update a click context with defaults from a yaml file.

    Parameters
    ----------
    ctx : `click.Context`
        The click context to update.
    param : `click.core.Option`
        The name of the parameter.
    value : `str`
        The value of the parameter.
    """
    ctx.default_map = ctx.default_map or {}
    cmd_name = ctx.info_name
    if not cmd_name:
        return
    _log.debug("Applying command line overrides for subcommand %s from URI %s", cmd_name, value)
    if value:
        try:
            overrides = _read_yaml_presets(value, cmd_name)
        except Exception as e:
            raise click.BadOptionUsage(cmd_name, f"Error reading overrides file: {e}", ctx)
        # Override the defaults for this subcommand
        ctx.default_map.update(overrides)


def _read_yaml_presets(file: str, cmd_name: str) -> Dict[str, Union[str, int, bool]]:
    """Read file command line overrides from YAML config file.

    Parameters
    ----------
    file : `str`
        Path to override YAML file containing the command line overrides.
        They should be grouped by command name. The option name should
        NOT include prefix dashes.
    cmd_name : `str`
        The subcommand name that is being modified.

    Returns
    -------
    overrides : `dict` of [str, Union[`str`, `int`, `bool`]]
        The relevant command line options read from the override file.
    """
    with open(file) as f:
        presets = yaml.safe_load(f.read())
    return presets.get(cmd_name, dict())


def process_targs(ctx: click.Context, param: click.Parameter, vals: List[str]) -> Targs:
    """Helper for the `click.option` that accepts template argument overrides.

    On the CLI the option must be used once for each template argument.

    The value can contain a single element, or multiple elements separated by
    commas to create a list. (A single item followed by a comma will create a
    list).

    Parameters
    ----------
    ctx : click.Context
        The click context for the current command.
    param : click.Parameter
        The click parameter currently being processed.
    vals : List[str]
        The value for the current parameter.

    Returns
    -------
    Dict[str, str]
        The modified current parameter value.

    Raises
    ------
    RuntimeError
        If the value does not contain exactly one equal sign.
    """
    kvs = list((pair.split("=", maxsplit=1) for pair in vals))
    if any([len(kv) != 2 for kv in kvs]):
        raise RuntimeError("Each argument to --targs must be a key-value pair with exactly one '='.")
    # if the value ends with a comma it should be a list but remove the trailing
    # comma to prevent an empty value, because "a,".split(",") becomes
    # ["a", ""], and "a".split(",") becomes ["a"].
    d = dict((k, (v.rstrip(",").split(",") if "," in v else v)) for k, v in kvs)
    return d


def targs(
    ctx: click.Context,
) -> Targs:
    """Helper for click.command functions that assemble template
    arguments from environment variables, command options, a --targs option and
    a --targs-file option.

    Parameters
    ----------
    ctx : click.Context
        The click context for the current command.

    Returns
    -------
    targs : Dict[str, Union[int, Any]]
        The dict of values to to render templates.
    """
    options = copy.copy(ctx.params)
    targs = options.pop("targs")
    targs_file = options.pop("targs_file")
    ret: Targs = dict(os.environ)
    if hasattr(ctx.obj, "extended_args"):
        ret["extended_args"] = ctx.obj.extended_args
    ret.update(options)
    if targs_file:
        with open(targs_file) as f:
            ret.update(yaml.safe_load(f.read()))
    if targs:
        ret.update(targs)
    return ret


def clickResultMsg(result: click.testing.Result) -> str:
    """Helper for unit tests that use `click.testing.CliRunner`, which
    returns a result object. This accepts a result object and returns a string
    that can be used in a `unittest.assert...` `msg` argument.

    Parameters
    ----------
    result : click.testing.Result
        The result object returned from click.testing.CliRunner.invoke

    Returns
    -------
    msg : `str`
        The message string.
    """
    msg = f"""\noutput: {result.output}\nexception: {result.exception}"""
    if result.exception:
        msg += f"""\ntraceback: {"".join(traceback.format_tb(result.exception.__traceback__))}"""
    return msg
