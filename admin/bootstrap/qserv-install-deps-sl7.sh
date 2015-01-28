#!/bin/bash

##
## Scientific Linux 7 dependencies
##

# newinstall.sh
yum install --assumeyes bash git tar make

# sconsUtils
yum install --assumeyes gettext flex bison 

# eups
yum install --assumeyes patch bzip2 bzip2-devel

# xrootd
yum install --assumeyes gcc gcc-c++ zlib-devel cmake

# zope_interface
yum install --assumeyes python-devel

# mysql
yum install --assumeyes ncurses-devel glibc-devel

# qserv
yum install --assumeyes boost-devel openssl-devel java

# lua
yum install --assumeyes readline-devel

# mysql-proxy
yum install --assumeyes glib2-devel

# kazoo
yum install --assumeyes python-setuptools

# FIXME qserv: deprecated since 2015_01
yum install --assumeyes numpy
