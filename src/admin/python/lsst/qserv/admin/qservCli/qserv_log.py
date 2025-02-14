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

"""Logging definitions and utilities for qserv CLI."""

log_level_flag = "--log-level"
default_log_level = "INFO"
log_level_choices = ["CRITICAL", "ERROR", "WARNING", "INFO", "DEBUG"]
missing_argument_msg = f"{log_level_flag} requires an argument."
invalid_value_msg = (
    f"Invalid value for {log_level_flag}, must be one of {log_level_choices}, not case sensitive."
)


def log_level_from_args(args: list[str], default: str = default_log_level) -> tuple[bool, str]:
    """Get the log level from script arguments.

    Parameters
    ----------
    args : list [Any]
        A list of arguments a script was launched with.

    Returns
    -------
    (success, value) : Tuple[bool, str]
        If the first item is true True then a log level option was found or not
        found, and then the second item is the log level value that was found or
        the default value. If the first item is False then a log level option
        was found but was missing a value or the found value was not a valid log
        level, and so the second item is a user-readable description of the
        problem.
    """
    split_args = []
    for arg in args:
        if "=" in arg:
            split_args.extend(arg.split("="))
        else:
            split_args.append(arg)
    if log_level_flag not in split_args:
        return (True, default)
    log_level_idx = split_args.index(log_level_flag)
    if len(split_args) <= log_level_idx + 1:
        return (False, missing_argument_msg)
    val = split_args[log_level_idx + 1].upper()
    if val not in log_level_choices:
        return (False, invalid_value_msg)
    return True, val
