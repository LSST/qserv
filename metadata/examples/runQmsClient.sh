#!/bin/sh
XRD_DIR=/u1/qserv/xrootd
PLATFORM=x86_64_linux_26_dbg

BASEPATH=`pwd`

export PYTHONPATH=/u1/lsst/lib/python2.5/site-packages:$BASEPATH/../python

PYTHON=/usr/bin/python # Use OS-default python, not SLAC /usr/local/bin/python
export LD_LIBRARY_PATH=/u1/lsst/lib:$XRD_DIR/lib/$PLATFORM
export QSW_RESULTDIR=/u1/qserv-run/tmp
$PYTHON $BASEPATH/../python/lsst/qserv/metadata/qmsClient.py $*
