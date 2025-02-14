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


import logging
from collections import namedtuple
from contextlib import closing
from time import sleep
from typing import Any
from collections.abc import Sequence

import requests

_log = logging.getLogger(__name__)

try:
    from .mysql_connection import mysql_connection
except:
    _log.warning("Could not import mysql.connection")


CheckFailure = namedtuple("CheckFailure", "query_id execution_time query")

start_msg_t = "Starting watcher on *{cluster_id}*. Will notify for queries running longer than {timeout_sec} seconds, polling every {interval_sec} seconds."

stop_msg_t = "Stopping watcher on *{cluster_id}*."

first_check_msg_t = "On *{cluster_id}* during the first check the following query ids had already exceeded the {timeout_sec} second timeout: {ids}"

msg_prefix_t = "On *{cluster_id}* the following queries exceeded the {timeout_sec} second timeout:\n"
show_query_msg_t = "- Query '{query}' id {id} has been running for {time} seconds.\n"
query_msg_t = "- Query id {id} has been running for {time} seconds.\n"

check_query_failed = "On *{cluster_id}* could not execute the check query."

notify_data = '{{"text":"{msg}"}}'
notify_headers = {"Content-Type": "application/json"}


class NotifyUrlError:
    pass


class Watcher:
    """Poll a qserv instance for queries that have been executing for longer
    than a given time, and send an alert to a given url if any are found.

    Parameters
    ----------
    cluster_id : `str`
        A itentifier for the cluster that will be diplayed in notifications.
    notify_url_file : `str`
        The path to the file that contains the url to send notifications to.
    qserv : `url`
        The url to connect to qserv as an admin user.
    timeout_sec : `int`
        How long a query can be running for, after which it will be considered
        stuck or timed out, in seconds.
    interval_sec : `int`
        How long to wait between checks for stuck queries, in seconds.
    show_query : `bool`
        True if the query should be included in the alert message.
    """

    def __init__(
        self,
        cluster_id: str,
        notify_url_file: str,
        qserv: str,
        timeout_sec: int,
        interval_sec: int,
        show_query: bool,
    ):
        self.cluster_id = cluster_id
        self.prev_failed_ids: set[str] = set()
        self.first_check = True
        self.qserv = qserv
        self.timeout_sec = timeout_sec
        self.interval_sec = interval_sec
        self.show_query = show_query
        self.notify_url: None | type[NotifyUrlError] | str = self._get_notify_url(notify_url_file)

    def _get_notify_url(self, notify_url_file: str) -> type[NotifyUrlError] | str:
        _log.debug(f"Reading notify url {notify_url_file}")
        notify_url: type[NotifyUrlError] | str = NotifyUrlError
        try:
            with open(notify_url_file) as f:
                notify_url = f.read()
            _log.debug(f"Notify url: {notify_url}")
        except:
            notify_url = NotifyUrlError
            _log.error(f"Could not get notify url from {notify_url_file}")
        return notify_url

    def alert(self, check_failures: list[CheckFailure]) -> None:
        """Send an alert for queries that failed the timeout."""
        if self.first_check:
            msg = first_check_msg_t.format(
                cluster_id=self.cluster_id,
                timeout_sec=self.timeout_sec,
                ids=", ".join([str(cf.query_id) for cf in check_failures]),
            )
        else:
            msg = msg_prefix_t.format(timeout_sec=self.timeout_sec, cluster_id=self.cluster_id)
            if self.show_query:
                msg_t = show_query_msg_t
            else:
                msg_t = query_msg_t
            msg += " ".join(
                [msg_t.format(query=f.query, id=f.query_id, time=f.execution_time) for f in check_failures]
            )
        self.notify(msg=msg)

    def alert_check_query_failed(self) -> None:
        """Send an alert that the check query failed."""
        self.notify(check_query_failed.format(cluster_id=self.cluster_id))

    def notify(self, msg: str) -> None:
        """Send a notification."""
        url = self.notify_url
        if url is None:
            return
        if url is NotifyUrlError:
            _log.error(f"Could not open the notify url file, can not send notification: {msg}")
            return
        # This satisfies the type checker; mypy does not seem to realize that `if url is NotifyUrlError`
        # handles that case, and the only remaning possibility is that url is a str.
        if not isinstance(url, str):
            raise RuntimeError(f"notify_url is {url}, expected a string value.")
        try:
            res = requests.post(url, headers=notify_headers, data=notify_data.format(msg=msg))
            if res.status_code != 200:
                _log.error(
                    f"FAILED TO SEND NOTIFICATION cluster: {self.cluster_id}, msg: {msg} to {url}: {res}"
                )
        except Exception as e:
            _log.error(f"Failed to send notification {msg}, exception: {e}")
        _log.debug(f"Sent notification: {msg}")

    def query(self, uri: str, stmt: str) -> Sequence[Any]:
        """Execute a mysql query at the provided URI

        Parameters
        ----------
        uri : `str`
            The uri to the database.
        stmt : `str`
            The sql query to execute.

        Returns
        -------
        results : `list` [`tuple`]
            The results of the query. Each list item is a row and each row item is a
            column value.
        """
        try:
            with closing(mysql_connection(uri)) as cnx:
                with closing(cnx.cursor()) as cursor:
                    res = cursor.execute(stmt)
                    results = cursor.fetchall()
        except Exception as e:
            results = []
            _log.error(f"Failed to execute the query {stmt} at {uri}, exception: {e}")
        return results

    def check(self) -> list[CheckFailure]:
        """Check that no queries have been running for longer than a given time.

        Returns
        -------
        check_failures : `list` [ `CheckFailure` ]
            A list of information about queries that exceeded the timeout duration.
            Each query will only be retuned once, same ids will not be returned on
            future checks.
        """
        stmt = (
            "SELECT queryId, status, submitted, TIME_TO_SEC(TIMEDIFF(NOW(),submitted)), query "
            "FROM qservMeta.QInfo "
            f"WHERE status='EXECUTING' AND TIME_TO_SEC(TIMEDIFF(NOW(), submitted)) > {self.timeout_sec} "
            "ORDER BY submitted DESC"
        )
        results = self.query(self.qserv, stmt)
        if results is None:
            return None
        ret = [
            CheckFailure(result[0], result[3], result[4])
            for result in results
            if result[0] not in self.prev_failed_ids
        ]
        for result in results:
            self.prev_failed_ids.add(result[0])
        return ret

    def notify_start(self) -> None:
        """Send the start notification."""
        self.notify(
            msg=start_msg_t.format(
                cluster_id=self.cluster_id,
                timeout_sec=self.timeout_sec,
                interval_sec=self.interval_sec,
            )
        )

    def notify_stop(self) -> None:
        """Send the stop notification."""
        self.notify(msg=stop_msg_t.format(cluster_id=self.cluster_id))

    def run_check(self) -> None:
        """Run one check and send an alert if needed."""
        check_failures = self.check()
        if check_failures is None:
            self.alert_check_query_failed()
            _log.error("Failed to run a check.")
            return
        if check_failures:
            self.alert(check_failures)
        else:
            _log.debug(
                f"cluster: {self.cluster_id}, checked, did not find any long-running/timed-out queries."
            )
        self.first_check = False

    def watch(self) -> None:
        """Start polling."""
        try:
            self.notify_start()
            while True:
                self.run_check()
                sleep(self.interval_sec)
        finally:
            self.notify_stop()


def watch(
    cluster_id: str,
    notify_url_file: str,
    qserv: str,
    timeout_sec: int,
    interval_sec: int,
    show_query: bool,
) -> None:
    watcher = Watcher(cluster_id, notify_url_file, qserv, timeout_sec, interval_sec, show_query)
    watcher.watch()
