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


"Mysql utilities."

import logging
import mysql.connector
from typing import cast
from urllib.parse import urlparse

_log = logging.getLogger(__name__)


def mysql_connection(
    uri: str,
    get_warnings: bool = True,
    local_infile: bool = False,
) -> mysql.connector.abstracts.MySQLConnectionAbstract:
    """Create a mysql.connection that is connected to a database.

    Parameters
    ----------
    uri : `str`, optional
        The URI of the database to connnect to.
    local_infile : bool, optional
        Passed to the allow_local_infile parameter of the connector, by default False
    get_warnings : bool, optional
        Passed to the

    Returns
    -------
    connection : `mysql.connection`
        The connected connection object.
    """
    if uri:
        parsed = urlparse(uri)
        hostname = parsed.hostname
        port = parsed.port
        user = parsed.username
        pw = parsed.password
    _log.debug("mysql_connection hostname:%s, port:%s, user:%s", hostname, port, user)
    # Cast justified because no pool args passed here to connect(), so cnx cannot be PooledMySQLConnection
    cnx = cast(
        mysql.connector.abstracts.MySQLConnectionAbstract,
        mysql.connector.connect(
            user=user,
            password=pw,
            host=hostname,
            port=port,
            allow_local_infile=local_infile,
        ),
    )
    cnx.get_warnings = get_warnings
    return cnx
