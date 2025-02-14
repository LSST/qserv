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


"""This module contains functions for rendering jinja template
values in values passed into the `entrypoint` command line."""

from copy import copy

import jinja2

from .utils import Targs


class UnresolvableTemplateError(RuntimeError):
    """Exception class used by `render` when a template value can not be
    resolved."""

    pass


def _format_targs(targs: Targs) -> str:
    """Format targs for printing to an error message."""
    return ", ".join([f"{k}={v}" for k, v in targs.items()])


def _get_vars(val: str) -> list[str]:
    """Get variable names from a value that contains template variables.

    Parameters
    ----------
    val : str
        The value from a targs entry.

    Returns
    -------
    vars : list [ `str` ]
        The variables (value inside braces) inside val.
    """
    # Jinja variable names must be surrounded by double braces and may be
    # surrounded by whitespace inside the braces (`{{foo}}` or `{{ foo }}`).
    # Python variable names may not contain braces. So, find all the leading
    # braces, and use the rest of the string up to the closing braces.
    return [i[: i.find("}}")].strip() for i in val.split("{{") if "}}" in i]


def render_targs(targs: Targs) -> Targs:
    """Go through a dict whose values may contain jinja templates that are
    other keys in the dict, and resolve the values.

    Will raise if any template value(s) can not be resolved. Causes include:
    * the key is not present
    * there is a circular reference where 2 or more keys refer to eachother.

    Parameters
    ----------
    targs :
        The dict to resolve.

    Returns
    -------
    resolved_targs :
        The dict, resolved.

    Raises
    ------
    UnresolvableTemplate
        If a value in the template can not be resolved.
    """
    rendered = copy(targs)
    while True:
        changed = False
        for k, v in rendered.items():
            if not isinstance(v, str):
                continue
            if "{{" in v:
                if k in _get_vars(v):
                    raise UnresolvableTemplateError(
                        "Template value may not refer to its own key, directly or as a circualr reference:"
                        + _format_targs(targs)
                    )
                t = jinja2.Template(v, undefined=jinja2.StrictUndefined)
                try:
                    r = t.render(rendered)
                    if r != rendered[k]:
                        rendered[k] = r
                        changed = True
                except jinja2.exceptions.UndefinedError as e:
                    raise UnresolvableTemplateError(f"Missing template value: {e!s}")
        if not changed:
            break
    if any([isinstance(v, str) and "{{" in v for v in rendered.values()]):
        raise UnresolvableTemplateError(f"Could not resolve inputs {targs}, they became: {rendered}")
    return rendered
