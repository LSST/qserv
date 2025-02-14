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

"""This module mimics the lsst.utils.wrappers.py module.

This provides only the dependencies for modules we use (`lsst.log`) and reduces
upstream dependencies, i.e. `lsst.utls` depends on numpy, but we don't need
numpy for the `lsst.utils` functions used by `lsst.log`
"""

import sys


def continue_class(cls):
    """Re-open the decorated class, adding any new definitions into the
    original.

    For example:

    .. code-block:: python

        class Foo:
            pass

        @continueClass
        class Foo:
            def run(self):
                return None

    is equivalent to:

    .. code-block:: python

        class Foo:
            def run(self):
                return None

    .. warning::

        Python's built-in `super` function does not behave properly in classes
        decorated with `continueClass`.  Base class methods must be invoked
        directly using their explicit types instead.

    """
    orig = getattr(sys.modules[cls.__module__], cls.__name__)
    for name in dir(cls):
        # Common descriptors like classmethod and staticmethod can only be
        # accessed without invoking their magic if we use __dict__; if we use
        # getattr on those we'll get e.g. a bound method instance on the dummy
        # class rather than a classmethod instance we can put on the target
        # class.
        attr = cls.__dict__.get(name, None) or getattr(cls, name)
        if is_attribute_safe_to_transfer(name, attr):
            setattr(orig, name, attr)
    return orig


INTRINSIC_SPECIAL_ATTRIBUTES = frozenset(
    (
        "__qualname__",
        "__module__",
        "__metaclass__",
        "__dict__",
        "__weakref__",
        "__class__",
        "__subclasshook__",
        "__name__",
        "__doc__",
    )
)


def is_attribute_safe_to_transfer(name, value):
    """Return True if an attribute is safe to monkeypatch-transfer to another
    class.

    This rejects special methods that are defined automatically for all
    classes, leaving only those explicitly defined in a class decorated by
    `continueClass` or registered with an instance of `TemplateMeta`.
    """
    if name.startswith("__") and (
        value is getattr(object, name, None) or name in INTRINSIC_SPECIAL_ATTRIBUTES
    ):
        return False
    return True
