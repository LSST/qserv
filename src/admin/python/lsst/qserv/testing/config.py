"""Configuration classes for testing harness."""

__all__ = ["Config", "QueryFactory"]

import contextlib
import logging
import random

import numpy as np
import yaml

_LOG = logging.getLogger(__name__)


@contextlib.contextmanager
def _make_file(path_or_file):
    """Context manager that makes a file out of argument.

    Parameters
    ----------
    path_or_file : `str` or file object
        Path name for a file or a file object.
    """
    if hasattr(path_or_file, "read"):
        yield path_or_file
    else:
        with open(path_or_file) as file:
            yield file


class _ValueRandomUniform:
    """Generator for uniformly distributed floating point numbers.

    Parameters
    ----------
    min, max : `float`
        Range for generated numbers.
    """

    def __init__(self, min, max):
        self._min = float(min)
        self._max = float(max)

    def __call__(self):
        return random.uniform(self._min, self._max)


class _ValueRandomUniformInt:
    """Generator for uniformly distributed integer numbers.

    Parameters
    ----------
    min, max : `int`
        Range for generated numbers.
    """

    def __init__(self, min, max):
        self._min = float(min)
        self._max = float(max)

    def __call__(self):
        return int(random.uniform(self._min, self._max))


class _ValueIntFromFile:
    """Generator for numbers that are read from input file.

    Parameters
    ----------
    path : `str`
        Name of the file to read numbers from.
    mode : `str`, optional
        One of "random" or "sequential".
    """

    def __init__(self, path, mode="random"):
        # read all numbers from file as integers
        if path == "/dev/null":
            # for testing only
            self._array = [0]
        else:
            self._array = np.fromfile(path, dtype=int, sep=" ")
        self._mode = mode
        assert mode in ("random", "sequential")
        self._seq = 0

    def __call__(self):
        if self._mode == "random":
            return random.choice(self._array)
        else:
            if self._seq >= len(self._array):
                self._seq = 0
            value = self._array[self._seq]
            self._seq += 1
            return value


class QueryFactory:
    """Class which generates queries.

    Parameters
    ----------
    txt : `str`
        If ``variables`` is ``None`` then full query text, otherwise it is a
        query template.
    variables : `dict` [`str`, `dict`]
        Dictionary whose keys are variable names and values are dictionaries
        with a description of how to generate variable value.
    """

    def __init__(self, txt, variables=None):
        self._txt = txt
        self._vars = {}
        if variables is not None:
            for var, config in variables.items():
                generator = None
                if "distribution" in config:
                    if config["distribution"] == "uniform":
                        min = config.get("min", 0.0)
                        max = config.get("max", 1.0)
                        generator = _ValueRandomUniform(min, max)
                    elif config["distribution"] == "uniform_int":
                        min = config.get("min", 0)
                        max = config.get("max", 2**64)
                        generator = _ValueRandomUniformInt(min, max)
                elif "path" in config:
                    path = config["path"]
                    mode = config.get("mode", "random")
                    generator = _ValueIntFromFile(path, mode)
                if generator is None:
                    raise ValueError(f"Cannot parse variable configuration {var} = {config}")
                self._vars[var] = generator

    def query(self):
        """Return next query to execute.

        Returns
        -------
        query : `str`
            Query to be executed.
        """
        if not self._vars:
            return self._txt
        else:
            values = {}
            for var, generator in self._vars.items():
                values[var] = generator()
            return self._txt.format(**values)


class Config:
    """Configuration of test harness.

    Parameters
    ----------
    configs : `list` [`dict`]
        List of dictionaries, cannot be empty.
    """

    def __init__(self, configs):
        if not configs:
            raise ValueError("empty configurations list")

        # merge them
        self._config = {}
        for config in configs:
            self._config = self._merge(self._config, config)

        classes = set(self._config.get("queryClasses", {}).keys())
        if not classes:
            raise ValueError("No query classes defined in configuration")
        querykeys = set(self._config.get("queries", {}).keys())

        self._classes = classes & querykeys
        if not self._classes:
            raise ValueError(
                f"Query classes has no common keys with queries: queryClasses={classes}, queries={querykeys}"
            )

        self._queries = {}
        for qclass in self._classes:
            queries = self._queries[qclass] = {}
            for qkey, qcfg in self._config["queries"][qclass].items():
                if isinstance(qcfg, str):
                    # complete query string
                    queries[qkey] = QueryFactory(qcfg)
                elif "template" in qcfg and "variables" in qcfg:
                    queries[qkey] = QueryFactory(qcfg["template"], qcfg["variables"])
                else:
                    raise ValueError(f"Unexpected query configuration: {qkey}: {qcfg}")

    @classmethod
    def from_yaml(cls, config_files):
        """Make configuration from bunch of YAML files

        Parameters
        ----------
        config_files : `list` [`str` or file]
            List of YAML sources, file name or open file object is acceptable.

        Returns
        -------
        config : `Config`
            Configuration class instance.
        """
        # read all YAML files into memory
        configs = []
        for cfg_file in config_files:
            with _make_file(cfg_file) as file:
                configs.append(yaml.load(file, Loader=yaml.SafeLoader))
        return cls(configs)

    def to_yaml(self):
        """Convert current config to YAML string.

        Returns
        -------
        yaml_str : `str`
            YAML representation of the configuration as a string.
        """
        return yaml.dump(self._config)

    def classes(self):
        """Return set of classes defined in configuration.

        Returns
        -------
        classes : `set` [`str`]
            Sequence of class names defined by configuration.
        """
        return self._classes

    def queries(self, q_class):
        """Return queries for given class.

        Parameters
        ----------
        q_class : `str`
            Query class name.

        Returns
        -------
        queries : `dict` [`str`, `QueryFactory`]
            Dictionary whose key is a query ID (abstract string defined by
            config) and value is a `QueryFactory` instance.
        """
        return self._queries[q_class]

    def concurrent_queries(self, q_class):
        """Return number of concurrent queries for given class.

        Parameters
        ----------
        q_class : `str`
            Query class name.

        Returns
        -------
        n_queries : `int`
            Number of queries that should run simultaneously
        """
        return self._config["queryClasses"][q_class]["concurrentQueries"]

    def max_rate(self, q_class):
        """Return maximum rate for given class.

        Parameters
        ----------
        q_class : `str`
            Query class name.

        Returns
        -------
        rate : `float`
            Max rate of submission for this class of queries.
        """
        return self._config["queryClasses"][q_class].get("maxRate")

    def arraysize(self, q_class):
        """Return array size for fetchmany().

        Parameters
        ----------
        q_class : `str`
            Query class name.

        Returns
        -------
        arraysize : `int`
            Array size to use.
        """
        return self._config["queryClasses"][q_class].get("arraysize")

    def split(self, n_workers, i_worker):
        """Divide configuration (or its workload) between number of workers.

        If we want to run test with multiple workers we need to divide work
        between them so that total mount of work is what is defined by this
        configuration. Dividing floating point "work" is easy, for integer
        "work" we need to be careful. The algorithm is simple, e.g. if we are
        to divide 5 units of work between 3 workers, two of them get two unis
        and one will get one unit (5 = 2 + 2 + 1). To know which worker is
        assigned extra units we also need to know worker ordering, this is
        why we pass extra argument which is worker serial number.

        Parameters
        ----------
        n_workers : `int`
            Total number of workers.
        i_worker : `int`
            This worker serial number (0 to n_workers-1 inclusive)

        Returns
        -------
        config : `Config`
            Configuration for specified worker.
        """
        overrides = {}
        for q_class in self._classes:
            n_queries = self.concurrent_queries(q_class)
            quot, rem = divmod(n_queries, n_workers)
            if i_worker < rem:
                quot += 1
            n_queries = quot

            overrides[q_class] = dict(concurrentQueries=n_queries)

        return self.__class__([self._config, dict(queryClasses=overrides)])

    @staticmethod
    def _merge(config1, config2):
        """Merge two config objects, return result.

        If configuration present in both then second one overrides first.

        Parameters
        ----------
        config1, config2 : `dict`
            Dictionaries with configuration data.

        Returns
        -------
        config : `dict`
            Merged configuration data.
        """

        result = {}
        keys = set(config1.keys()) | set(config2.keys())
        for key in keys:
            if key not in config2:
                result[key] = config1[key]
            elif key in config1 and isinstance(config1[key], dict) and isinstance(config2[key], dict):
                # if value is dict in both of them then do recursive merge
                result[key] = Config._merge(config1[key], config2[key])
            else:
                # otherwise second one overrides
                result[key] = config2[key]
        return result
