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


"""Utilities for using the backoff module in qserv."""

from collections.abc import Callable
from functools import partial
from logging import Logger
from typing import Any

import backoff

max_backoff_sec = 5 * 60


def on_backoff(log: Logger) -> Callable[[dict[str, Any]], None]:
    """Factory to make a callback handler that logs information about a
    backoff. To be used with the `on_backoff` parameter of a
    `backoff.on_exception` function decorator.

    Parameters
    ----------
    log : `logging.Logger`
        The logger that will receive backoff execution details.

    Returns
    -------
    handler : func
        A function that can be passed to `on_backoff`.
    """

    def handler(details: dict[Any, Any]) -> None:
        """Handler for `backoff.on_exception`.

        Parameters
        ----------
        details : `dict`
            Details about the current backoff. Items are documented in the
            backoff module at https://github.com/litl/backoff#event-handlers.
        """
        log.info(
            "Backing off {wait:0.1f} seconds after {tries} tries "
            "calling function {target} with args {args} and kwargs "
            "{kwargs}".format(**details)
        )

    return handler


qserv_backoff = partial(
    backoff.on_exception,
    wait_gen=backoff.expo,
    max_time=max_backoff_sec,
)


# Changing to this implementation of backoff.on_exception can be useful for
# executing qserv with more consistent/reproducable behavior.
# qserv_backoff = partial(
#     backoff.on_exception,
#     wait_gen=backoff.constant,
#     jitter=None,
#     interval=1,
#     max_time=max_backoff_seconds,
# )
