# This file is part of qserv.
#
# Developed for the LSST Data Management System.
# This product includes software developed by the LSST Project
# (http://www.lsst.org).
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
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

"""Unit tests for test_watcher."""

import unittest
from collections import namedtuple
from unittest.mock import patch

import requests
from lsst.qserv.admin.watcher import (
    CheckFailure,
    Watcher,
    first_check_msg_t,
    msg_prefix_t,
    notify_data,
    notify_headers,
    query_msg_t,
    show_query_msg_t,
    start_msg_t,
    stop_msg_t,
)

res_42_43 = [
    (42, "EXECUTING", "unused_summitted_time", 700, "SELECT objectId from Object"),
    (43, "EXECUTING", "unused_summitted_time", 702, "SELECT sourceId from Source"),
]

check_failures_42_43 = [
    CheckFailure(42, 700, "SELECT objectId from Object"),
    CheckFailure(43, 702, "SELECT sourceId from Source"),
]

res_44 = (44, "EXECUTING", "unused_submitted_time", 722, "SELECT foo FROM bar")

check_failure_44 = [
    CheckFailure(44, 722, "SELECT foo FROM bar"),
]

query_results = None

PostResult = namedtuple("PostResult", "status_code")


def mock_query(self, uri, stmt):
    """return mock sql query query results.

    Parameters
    ----------
    uri : `str`
        Unused. Would be the uri to the database.
    stmt : `str`
        Unused. The sql query.

    Returns
    -------
    query_results : `list` [ `tuple` ]
        Mock query results, the list is rows, the tuple is column values.
    """

    "SELECT queryId, status, submitted, TIME_TO_SEC(TIMEDIFF(NOW(),submitted)), query "
    return query_results


class WatcherTestCase(unittest.TestCase):
    """Tests the qserv watcher script."""

    def setUp(self):
        global query_results
        query_results = res_42_43

    @patch("lsst.qserv.admin.watcher.Watcher.query", mock_query)
    def test_check(self):
        """Verify that the check function returns CheckFailures exactly one time for
        each query id returned by the check query.
        """
        global query_results
        watcher = Watcher(
            cluster_id="utest",
            notify_url=None,
            qserv="qserv://user@host:123",
            timeout_sec=600,
            interval_sec=1,
            show_query=False,
        )
        # verify that the first call to check returns the two expected CheckFailures.
        res = watcher.check()
        self.assertEqual(res, check_failures_42_43)
        # verify that the query ids have been cached
        self.assertEqual(watcher.prev_failed_ids, set([42, 43]))
        # verify that the second call to check does not return the two previoiusly seen CheckFailures.
        res = watcher.check()
        self.assertEqual(res, [])

        # add another result row and verify that check returns the new row and not the old ones.
        query_results.append(res_44)
        res = watcher.check()
        self.assertEqual(res, check_failure_44)

    @patch.object(Watcher, "notify")
    def test_notify(self, mock_notify):
        """Verify that when CheckFailures are sent to alert, that the messages
        appear formatted as expected, and if failures are encountered by the
        initial check that the message is condensed."""
        notify_url = "foo/bar/baz"
        timeout_sec = 600
        cluster_id = "utest"
        watcher = Watcher(
            cluster_id=cluster_id,
            notify_url=notify_url,
            qserv="qserv://user@host:123",
            timeout_sec=timeout_sec,
            interval_sec=1,
            show_query=False,
        )
        watcher.alert(check_failures_42_43)
        mock_notify.assert_called_with(
            msg=first_check_msg_t.format(cluster_id=cluster_id, timeout_sec=timeout_sec, ids=("42, 43"))
        )
        # Watcher sets first_check to false at the end of the watch loop.
        # manually set it here (instead of mocking check and inventing a way
        # to only run the watch loop n times.)
        watcher.first_check = False
        watcher.alert(check_failure_44)
        prefix = msg_prefix_t.format(cluster_id=cluster_id, timeout_sec=timeout_sec)
        cf = check_failure_44[0]
        query_msg = query_msg_t.format(id=cf.query_id, time=cf.execution_time)
        mock_notify.assert_called_with(msg=prefix + query_msg)
        # run the same alert again (normally the same query id would be squelched by
        # `check` but we are bypassing that here) with show_query=True and verify:
        watcher.show_query = True
        watcher.alert(check_failure_44)
        prefix = msg_prefix_t.format(cluster_id=cluster_id, timeout_sec=timeout_sec)
        cf = check_failure_44[0]
        query_msg = show_query_msg_t.format(id=cf.query_id, time=cf.execution_time, query=cf.query)
        mock_notify.assert_called_with(msg=prefix + query_msg)

    @patch.object(requests, "post", return_value=PostResult(200))
    def test_start_stop_notification(self, mock_post):
        """Test sending the start and stop notifications."""
        notify_url = "slack.channel.url"
        timeout_sec = 600
        interval_sec = 1
        cluster_id = "utest"
        watcher = Watcher(
            cluster_id=cluster_id,
            notify_url=notify_url,
            qserv="qserv://user@host:123",
            timeout_sec=timeout_sec,
            interval_sec=interval_sec,
            show_query=False,
        )
        watcher.notify_start()
        mock_post.assert_called_once_with(
            notify_url,
            headers=notify_headers,
            data=notify_data.format(
                msg=start_msg_t.format(
                    cluster_id=cluster_id,
                    timeout_sec=timeout_sec,
                    interval_sec=interval_sec,
                )
            ),
        )
        mock_post.reset_mock()
        watcher.notify_stop()
        mock_post.assert_called_once_with(
            notify_url,
            headers=notify_headers,
            data=notify_data.format(
                msg=stop_msg_t.format(
                    cluster_id=cluster_id,
                )
            ),
        )


if __name__ == "__main__":
    unittest.main()
