#! /bin/bash
# admin/tools/docker/loader/container/dev/worker/appWorker.bash

_term() { 
  echo "Caught SIGTERM signal!" 
  kill -TERM "$child" 2>/dev/null
}

trap _term SIGTERM
trap _term SIGKILL

source /qserv/stack/loadLSST.bash
cd /qserv/dev/qserv
setup -r . -t qserv-dev

export LSST_LOG_CONFIG=/qserv/dev/qserv/admin/templates/configuration/etc/log4cxx.index.properties

/qserv/dev/qserv/build/loader/appWorker /qserv/dev/qserv/core/modules/loader/config/worker-k8s-a.cnf &

child=$!
wait "$child"
