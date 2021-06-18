import requests
import sys

# TODO:UJ This file and directory should be deleted as this is only
#         useful for UberJob proof of concept.

# source /datasets/gapon/stack/loadLSST.bash
# setup -t qserv-dev qserv_distrib
# exec scl enable sclo-git25 bash

# # This is hard coded to wise_01
# # lsst-qserv-master01 may need to be changed to a different master.
# setup ssh tunnel - ssh lsst-login.ncsa.illinois.edu -N -L 25081:lsst-qserv-master01:25080
# # replace ~/work/qserv with appropriate directory in statements below.
# cd ~/work/qserv/admin/tools/chunkmap
# python remap.py wise_01 > info.txt
# cp info.txt ~/work/qserv/admin/templates/configuration/etc/workerchunkdata.txt


master_base_url = "http://localhost:25081"


def getReplicas(database):
    url = "{}/ingest/chunks?database={}".format(master_base_url, database)
    data = {}
    response = requests.get(url, json=data)
    if response.status_code != 200:
        print("failed method: GET url: {}, http error: {}".format(url, response.status_code))
        sys.exit(1)
    responseJson = response.json()
    if not responseJson['success']:
        print("failed method: GET url: {}, server status: {}".format(url, responseJson["error"]))
        sys.exit(1)
    replicas = []
    for replica in responseJson["replica"]:
        replicas.append({"worker": replica["worker"], "chunk": replica["chunk"]})
    return replicas


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: <database>")
        sys.exit(1)
    database = sys.argv[1]
    for replica in getReplicas(database):
        print("{} {}".format(replica["worker"], replica["chunk"]))
