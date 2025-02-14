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

"""Utilities for running subprocesses in the qserv CLI"""

import subprocess
from typing import Dict, List, Optional


class QservSubprocessError(BaseException):
    """Represents an error that occurred while running a qserv CLI command in a
    subprocess."""


def run(
    args: List[str],
    env: Optional[Dict[str, str]] = None,
    capture_stdout: bool = False,
    cwd: Optional[str] = None,
    errmsg: Optional[str] = None,
    encoding: Optional[str] = None,
) -> subprocess.CompletedProcess:
    """Run a command in a subprocess. Raise a QservSubprocessError if the
    result of the subprocess execution was not 0.

    Parameters
    ----------
    args : List[str]
        The arguments to the subprocess.
    env : Dict[str, str], optional
        The environment overrides. If none the subprocess will inherit the
        current environment.
    capture_stdout : bool
        True if stdout should be captured.
    cwd : str, optional
        If provided, will change to this working directory before running the
        arguments.
    errmsg : str, optional
        A message to print, if there is an error, before raising
        QservSubprocessError.
    encoding : str, optional
        The encoding argument to subprocess.run.

    Returns
    -------
    result : `subprocess.CompletedProcess`
        The return value from run(), representing a process that has finished.

    Raises
    ------
    QservSubprocessError
        Raised if the result of the subprocess execution was not 0.
    """
    res = subprocess.run(
        args,
        env=env,
        stdout=subprocess.PIPE if capture_stdout else None,
        stderr=subprocess.STDOUT,
        cwd=cwd,
        encoding=encoding,
    )
    if res.returncode != 0:
        if errmsg:
            print(errmsg)
        if capture_stdout:
            stdout = res.stdout
            if isinstance(stdout, bytes):
                stdout = res.stdout.decode().strip()
            if stdout:
                print(stdout)
        raise QservSubprocessError()
    return res
