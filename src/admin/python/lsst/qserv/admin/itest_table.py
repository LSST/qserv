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

import os
from typing import Any, Dict, List

from dataclasses import dataclass


@dataclass
class LoadTable:
    """Contains information about a table to be loaded.

    Parameters
    ----------
    table_name: str
        The name of the table according to the load yaml
    ingest_config: Dict[Any, Any]
        The table ingest config dict
    data_file: str
        The absolute path to the data file (contains the csv or tsv data)
    partition_config_files: List[str]
        The absolute path to the partitioner config files
    data_staging_dir: str
        The location where data can be staged (has "rw" permissions)
    ref_db_table_schema_file: str
        The absolute path to the referecene db table schema file
    """

    table_name: str
    ingest_config: Dict[Any, Any]
    data_file: str
    partition_config_files: List[str]
    data_staging_dir: str
    ref_db_table_schema_file: str

    @property
    def is_partitioned(self) -> bool:
        return bool(self.ingest_config["is_partitioned"])

    @property
    def is_match(self) -> bool:
        return bool(self.ingest_config.get("director_table2"))

    @property
    def is_gzipped(self) -> bool:
        return os.path.splitext(self.data_file)[1] == ".gz"

    @property
    def fields_terminated_by(self) -> str:
        return self._csv_dialect_attr("fields_terminated_by", "\\t")

    @property
    def fields_enclosed_by(self) -> str:
        return self._csv_dialect_attr("fields_enclosed_by", "")

    @property
    def fields_escaped_by(self) -> str:
        return self._csv_dialect_attr("fields_escaped_by", "\\\\")

    @property
    def lines_terminated_by(self) -> str:
        return self._csv_dialect_attr("lines_terminated_by", "\\n")

    def _csv_dialect_attr(self, attr: str, default_val: str = "") -> str:
        if value := self.ingest_config.get(attr):
            return value
        return default_val
