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

import backoff
import copy
from .qserv_backoff import max_backoff_sec, on_backoff
import json
import logging
import os
from requests import delete, get, post, put
from requests.exceptions import ConnectionError
from requests_toolbelt.multipart.encoder import MultipartEncoder
import subprocess
from .itest_table import LoadTable
from typing import Any, Callable, Dict, Generator, List, Optional, NamedTuple, Tuple
from urllib.parse import urlparse


chunk_file_t = "chunk_{chunk_id}.txt"
chunk_overlap_file_t = "chunk_{chunk_id}_overlap.txt"

headers = {"Content-Type": "application/json"}

chunk_info_file = "chunk_info.json"

repl_api_version = 40

_log = logging.getLogger(__name__)


class ChunkLocation(NamedTuple):
    chunk_id: str
    host: str
    port: str
    http_host: str
    http_port: str


class RegularLocation(NamedTuple):
    host: str
    port: str
    http_host: str
    http_port: str


def _check(result: Dict[Any, Any], url: str) -> None:
    """Check the result of an http command and raise a RuntimeError if it was
    not successful.

    Parameters
    ----------
    result : `dict`
        The dict of the result json returned by the ingest system.
    url : `str`
        The url that returned the result.
    """
    if result["success"] != 1:
        raise ReplicationInterface.CommandError(f"Command {url} failed: {result}")


@backoff.on_exception(
    exception=ConnectionError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def _post(url: str, data: str) -> Dict[Any, Any]:
    """Call requests.post and check the result for success=1.

    Parameters
    ----------
    url : `str`
        The url to send to `post`.
    data : `data`
        The data to send to `post`.

    Returns
    -------
    result : `dict`
        The dict containing the result of calling `post`.
    """
    res: Dict[Any, Any] = post(url, headers=headers, data=data).json()
    _check(res, url)
    return res

@backoff.on_exception(
    exception=ConnectionError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def _post_file_upload(url: str, encoder: MultipartEncoder) -> Dict[Any, Any]:
    """Call requests.post and check the result for success=1.

    Parameters
    ----------
    url : `str`
        The url to send to `post`.
    data : `data`
        The data to send to `post`.

    Returns
    -------
    result : `dict`
        The dict containing the result of calling `post`.
    """
    res: Dict[Any, Any] = post(url, data=encoder, headers={'Content-Type': encoder.content_type}).json()
    _check(res, url)
    return res

@backoff.on_exception(
    exception=ConnectionError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def _delete(url: str, data: str, check: Callable[[Dict[Any, Any], str], None] = _check) -> Dict[Any, Any]:
    """Call requests.delete and apply a check function.

    Parameters
    ----------
    url : `str`
        The url to send to `delete`.
    data : `str`
        The data to send to `delete`.
    check : func `(dict, str) -> None`, optional
        The check function to use, that takes the dict returned from calling
        `delete(url, data)` and the url that was called (for logging). By
        default `_check`.

    Returns
    -------
    result : `dict`
        The dict containing the result of calling `delete`.
    """
    res: Dict[Any, Any] = delete(url, headers=headers, data=data).json()
    check(res, url)
    return res


@backoff.on_exception(
    exception=ConnectionError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def _put(url: str, data: str) -> Dict[Any, Any]:
    """Call requests.put and check the result for success=1.

    Parameters
    ----------
    url : `str`
        The url to send to `put`.
    data : `str`
        The data to send to `put`.

    Returns
    -------
    result : `dict`
        The dict containing the result of calling `put`.
    """
    res: Dict[Any, Any] = put(url, headers=headers, data=data).json()
    _check(res, url)
    return res


@backoff.on_exception(
    exception=ConnectionError,
    wait_gen=backoff.expo,
    on_backoff=on_backoff(log=_log),
    max_time=max_backoff_sec,
)
def _get(url: str, data: str) -> Dict[Any, Any]:
    """Call requests.get and check the result for success=1.

    Parameters
    ----------
    url : `str`
        The url to send to `get`.
    data : `data`
        The data to send to `get`.

    Returns
    -------
    result : `dict`
        The dict containing the result of calling `get`.
    """
    res: Dict[Any, Any] = get(url, headers=headers, data=data).json()
    _check(res, url)
    return res


class ReplicationInterface:
    """Interface functions for the qserv replication controller.

    Parameters
    ----------
    repl_ctrl_uri : `str`
        The uri to the replication controller service.
    auth_key : `str`
        The authorizaiton key for the replication-ingest system.
    """

    class CommandError(RuntimeError):
        """Raised when the call to the replication system returns not-success."""
        pass

    def __init__(
        self,
        repl_ctrl_uri: str,
        auth_key: Optional[str] = None,
        admin_auth_key: Optional[str] = None
    ):
        self.repl_ctrl = urlparse(repl_ctrl_uri)
        self.auth_key = auth_key
        self.admin_auth_key = admin_auth_key
        _log.debug(f"ReplicationInterface %s", self.repl_ctrl)

    def version(self) -> str:
        """Get the replication system version."""
        _log.debug("get version")
        res = _get(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/meta/version",
            data=json.dumps({}),
        )
        return str(res["version"])

    def ingest_database(self, database_json: Dict[Any, Any]) -> None:
        _log.debug("ingest_database json: %s", database_json)
        dj = copy.copy(database_json) # todo input var name needs changing
        dj["auth_key"] = self.auth_key
        js = json.dumps(dj)
        _post(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/database",
            data=js,
        )

    def ingest_table_config(self, table_json: Dict[Any, Any]) -> None:
        _log.debug("ingest_table_config: %s", table_json)
        dj = copy.copy(table_json) # todo name needs changing
        dj["auth_key"] = self.auth_key
        js = json.dumps(dj)
        _post(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/table",
            data=js,
        )

    def start_transaction(self, database: str) -> int:
        """Start a transaction and return the transaction id.

        Parameters
        ----------
        database : `str`
            The name of the database the transaction is being started for.

        Returns
        -------
        id : `int`
            The identifier for the transaction that is being started.
        """
        _log.debug("start_transaction database: %s", database)
        res = _post(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/trans",
            data=json.dumps(dict(database=database, auth_key=self.auth_key, version=repl_api_version,)),
        )
        return int(res["databases"][database]["transactions"][0]["id"])

    def commit_transaction(self, transaction_id: int) -> None:
        """Commit a transaction

        Parameters
        ----------
        transaction_id : `int`
            The transaction id obtained by calling `start_transaction`.
        """
        _log.debug("commit_transaction transaction_id: %s", transaction_id)
        res = _put(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/trans/{transaction_id}?abort=0",
            data=json.dumps(dict(auth_key=self.auth_key, version=repl_api_version,)),
        )

    def ingest_chunk_config(self, transaction_id: int, chunk_id: str) -> ChunkLocation:
        """Get the location where a given chunk id should be ingested.

        Parameters
        ----------
        transaction_id : `int`
            The transaction id obtained by calling `start_transaction`.
        chunk_id : `str`
            The id of the chunk that is being ingested.

        Returns
        -------
        location : `tuple`
            Returns a two-item tuple, the first element is the host name and the
            second name is the port
        """
        res = _post(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/chunk",
            data=json.dumps(dict(transaction_id=transaction_id, chunk=chunk_id, auth_key=self.auth_key,
                                 version=repl_api_version,)),
        )
        return ChunkLocation(chunk_id, res["location"]["host"], str(res["location"]["port"]),
                             res["location"]["http_host"], str(res["location"]["http_port"]))

    def ingest_chunk_configs(self, transaction_id: int, chunk_ids: List[int]) -> List[ChunkLocation]:
        """Get the locations where a list of chunk ids should be ingested.

        Parameters
        ----------
        transaction_id : `int`
            The transaction id obtained by calling `start_transaction`.
        chunk_ids : `list` [`int`]

        Returns
        -------
        locations : `list` : `ChunkLocation`
            A list of locations, `ChunkLocation` is a namedtuple with the parameters
            `chunk_id`, `host`, `port`.
        """
        res = _post(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/chunks",
            data=json.dumps(dict(transaction_id=transaction_id, chunks=chunk_ids, auth_key=self.auth_key,
                                 version=repl_api_version,)),
        )
        return [ChunkLocation(l["chunk"], l["host"], str(l["port"]),
                              l["http_host"], str(l["http_port"])) for l in res["location"]]

    def ingest_regular_table(self, transaction_id: int) -> List[RegularLocation]:
        """Get the locations where a non-chunk table should be ingested.

        Parameters
        ----------
        transaction_id : `int`
            The transaction id obtained by calling `start_transaction`.

        Returns
        -------
        locations : `list` : `RegularLocation`
            A list of locations, `RegularLocation` is a namedtuple with the parameters
            `host`, `port`.
        """
        _log.debug("ingest_regular_table transaction_id: %s", transaction_id)
        res = _get(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/regular?version={repl_api_version}",
            data=json.dumps(dict(auth_key=self.auth_key, transaction_id=transaction_id,)),
        )
        return [RegularLocation(location["host"], str(location["port"]),
                                location["http_host"], str(location["http_port"])) for location in res["locations"]]

    def ingest_data_file(
        self,
        transaction_id: int,
        chunk_id: str,
        overlap: bool,
        worker_host: str,
        worker_port: str,
        worker_http_host: str,
        worker_http_port: str,
        data_file: str,
        table: LoadTable,
        load_http: bool,
    ) -> None:
        """Ingest table data from a file.

        Parameters
        ----------
        transaction_id : `int`
            The transaction id.
        chunk_id : `str`
            The chunk id.
        overlap : `bool`
            The flag indicating if the file reprsentes the chunk overlap.
        worker_host : `str`
            The name of the host ingesting the data.
        worker_port : `str`
            The worker_host port to use.
        worker_http_host : `str`
            The name of the host ingesting the data (HTTP protocol).
        worker_http_port : `str`
            The worker_host port to use (HTTP protocol).
        data_file : `str`
            The path to the data file to ingest.
        table : `LoadTable`
            Table descriptor, including its name, ingest configuration, etc.
        load_http : `bool`
            The protocol to use for loading the data.
        """
        if not self.auth_key:
            raise RuntimeError("auth_key must be set to ingest a data file.")
        if load_http:
            encoder = MultipartEncoder(
                fields={
                    "auth_key": (None, self.auth_key),
                    "transaction_id": (None, str(transaction_id)),
                    "table": (None, table.table_name),
                    "chunk": (None, str(chunk_id)),
                    "overlap": (None, str("1" if overlap else "0")),
                    "fields_terminated_by": (None, str(table.fields_terminated_by)),
                    "fields_enclosed_by": (None, str(table.fields_enclosed_by)),
                    "fields_escaped_by": (None, str(table.fields_escaped_by)),
                    "lines_terminated_by": (None, str(table.lines_terminated_by)),
                    "file": (os.path.basename(data_file), open(data_file, "rb"), "text/plain"),
                }
            )
            _log.debug("encoder: %s", encoder)
            res_http = _post_file_upload(
                url=f"http://{worker_http_host}:{worker_http_port}/ingest/csv",
                encoder=encoder)
            if not res_http["success"]:
                raise RuntimeError(f"Ingest failed ({res_http})")
            _log.debug("ingest file res: %s", res_http)
        else:
            args = [
                "qserv-replica-file",
                "INGEST",
                "FILE",
                worker_host,
                worker_port,
                str(transaction_id),
                table.table_name,
                # app help says P for 'partitioned' and R for 'regular'/non-partitioned.
                "P" if table.is_partitioned else "R",
                data_file,
                "--verbose",
                f"--fields-terminated-by={table.fields_terminated_by}",
                f"--fields-enclosed-by={table.fields_enclosed_by}",
                f"--fields-escaped-by={table.fields_escaped_by}",
                f"--auth-key={self.auth_key}",
                f"--lines-terminated-by={table.lines_terminated_by}",
            ]
            _log.debug("ingest file args: %s", args)
            res = subprocess.run(
                args,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                errors="replace",
            )
            if res.returncode != 0:
                raise RuntimeError(
                    f"Subprocess failed ({res.returncode}) stdout:{res.stdout} stderr:{res.stderr}"
                )
            _log.debug("ingest file res: %s", res)

    def build_table_stats(
        self,
        database: str,
        tables: List[str],
        instance_id: Optional[str],
    ) -> None:
        for table in tables:
            _log.debug("build table stats for %s.%s", database, table)
            _post(
                url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/table-stats",
                data=json.dumps(
                    dict(
                        database=database,
                        table=table,
                        row_counters_deploy_at_qserv=1,
                        row_counters_state_update_policy="ENABLED",
                        force_rescan=1,
                        auth_key=self.auth_key,
                        admin_auth_key=self.admin_auth_key,
                        instance_id=instance_id,
                        version=repl_api_version,
                    ),
                ),
            )

    def publish_database(self, database: str) -> None:
        """Publish a database

        Parameters
        ----------
        database : `str`
            The name of the database being published.
        """
        _log.debug("publish_database database: %s", database)
        _put(
            url=f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/ingest/database/{database}",
            data=json.dumps(dict(auth_key=self.auth_key, version=repl_api_version,)),
        )

    def ingest_chunks_data(
        self,
        transaction_id: int,
        table: LoadTable,
        chunks_folder: str,
        chunk_info_file: str,
        load_http: bool,
    ) -> None:
        """Ingest chunk data that was partitioned using sph-partition.

        Parameters
        ----------
        transaction_id : `int`
            The transaction id obtained by starting a transaction
        table : `LoadTable`
            Table descriptor, including its name, ingest configuration, etc.
        chunks_folder : `str`
            The absolute path to the folder containing the chunk files to be ingested.
        chunks_info_file : `str`
            The absolute path to the file containing information about the chunks to be ingested.
        load_http : `bool`
            The protocol to use for loading the data.
        """
        _log.debug(
            "ingest_chunks_data transaction_id: %s table_name: %s chunks_folder: %s",
            transaction_id,
            table.table_name,
            chunks_folder,
        )
        # Ingest the chunk configs & get the host+port location for each chunk
        with open(chunk_info_file) as f:
            chunk_info = json.load(f)

        # Create locations for the chunk configs (repl system calls this "ingest" chunk)
        locations = self.ingest_chunk_configs(
            transaction_id, [chunk["id"] for chunk in chunk_info["chunks"]],
        )

        # Ingest the chunk files:
        # Helpful note: Generator type decl is Generator[yield, send, return],
        # see https://www.python.org/dev/peps/pep-0484/#annotating-generator-functions-and-coroutines
        def generate_locations() -> Generator[Tuple[str, str, str, str, str, str, bool], None, None]:
            for location in locations:
                for chunk_file in (chunk_file_t, chunk_overlap_file_t):
                    full_path = os.path.join(chunks_folder, chunk_file.format(chunk_id=location.chunk_id))
                    if os.path.exists(full_path):
                        _log.debug(
                            f"Ingesting %s to %s:%s/%s:%s chunk %s.",
                            full_path,
                            location.host,
                            location.port,
                            location.http_host,
                            location.http_port,
                            location.chunk_id,
                        )
                        overlap = "overlap" in chunk_file
                        yield full_path, location.host, location.port, location.http_host, location.http_port, location.chunk_id, overlap
                    else:
                        _log.warn(
                            "Not ingesting %s; it does not exist (probably there is no data for that chunk).",
                            full_path,
                        )

        for _file, host, port, http_host, http_port, chunk_id, overlap in generate_locations():
            self.ingest_data_file(
                transaction_id,
                chunk_id,
                overlap,
                host,
                port,
                http_host,
                http_port,
                data_file=_file,
                table=table,
                load_http=load_http
            )

    def ingest_table_data(
        self, transaction_id: int,
        table: LoadTable,
        data_file: str,
        load_http: bool,
    ) -> None:
        """Ingest data for a non-partitioned table.

        Parameters
        ----------
        transaction_id : `int`
            The trasaction id obtained by starting a transaction.
        table : `LoadTable`
            Table descriptor, including its name, ingest configuration, etc.
        data_file : `str`
            The absolute path to the file containing the table data.
        load_http : `bool`
            The protocol to use for loading the data.
        """
        _log.debug(
            "ingest_table_data: transaction_id: %s table.table_name: %s data_file: %s",
            transaction_id,
            table.table_name,
            data_file,
        )
        locations = self.ingest_regular_table(transaction_id)
        for location in locations:
            _log.debug(
                "Ingesting %s to %s:%s/%s:%s table %s.",
                data_file,
                location.host,
                location.port,
                location.http_host,
                location.http_port,
                table.table_name,
            )
            self.ingest_data_file(
                transaction_id,
                "0",
                False,
                location.host,
                location.port,
                location.http_host,
                location.http_port,
                data_file=data_file,
                table=table,
                load_http=load_http,
            )

    def delete_database(
        self,
        database: str,
        admin: bool,
    ) -> None:
        """Delete a database.

        Parameters
        ----------
        database : `str`
            The name of the database to delete.
        admin : `bool`
            True if the admin auth key should be used.
        """
        if admin: data = dict(admin_auth_key=self.admin_auth_key, version=repl_api_version,)
        else: data = dict(auth_key=self.auth_key, version=repl_api_version,)
        _log.debug("delete_database database:%s, data:%s", database, data)

        def warn_if_not_exist(res: Dict[Any, Any], url: str) -> None:
            if not res["success"]:
                if "no such database" in res["error"]:
                    _log.warn("Can not delete database %s; it does not exist.", database)
                    return
            _check(res, url)

        _delete(
            url="http://{hostname}:{port}/ingest/database/{database}?delete_secondary_index=1".format(
                hostname=self.repl_ctrl.hostname, port=self.repl_ctrl.port, database=database
            ),
            data=json.dumps(data),
            check=warn_if_not_exist,
        )

    def get_databases(self) -> List[str]:
        """Get the names of the existing databases from the replication system.

        Returns
        -------
        databases : List[str]
            The database names.
        """
        url = f"http://{self.repl_ctrl.hostname}:{self.repl_ctrl.port}/replication/config?version={repl_api_version}"
        res = _get(url, data=json.dumps({}))
        return [db["database"] for db in res["config"]["databases"] or []]
