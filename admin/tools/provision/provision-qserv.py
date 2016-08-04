#!/usr/bin/env python

"""
Boot instances from an image already created containing Docker
in OpenStack infrastructure, and use cloud config to create users
on virtual machines

Script performs these tasks:
  - launch instances from image and manage ssh key
  - create gateway vm
  - check for available floating ip address
  - add it to gateway
  - create users via cloud-init
  - update /etc/hosts on each VM
  - print ssh client config

@author  Oualid Achbal, IN2P3
"""

# -------------------------------
#  Imports of standard modules --
# -------------------------------
import argparse
import logging
import subprocess
import sys

# ----------------------------
# Imports for other modules --
# ----------------------------
from novaclient.exceptions import BadRequest
import cloudmanager

# -----------------------
# Exported definitions --
# -----------------------

def main():

    userdata_node = cloudManager.build_cloudconfig(cloudmanager.SWARM_NODE)

    # Create instances list
    instances = []

    if args.cleanup:
        cloudManager.nova_servers_cleanup(args.nbServers)

    # Create gateway instance and add floating_ip to it
    gateway_id = 0
    gateway_instance = cloudManager.nova_servers_create(gateway_id,
                                                        userdata_node)

    # Find a floating ip address for gateway
    floating_ip = cloudManager.get_floating_ip()
    if not floating_ip:
        logging.critical("Unable to add public ip to Qserv gateway")
        sys.exit(1)
    logging.info("Add floating ip ({0}) to {1}".format(floating_ip,
                                                       gateway_instance.name))
    try:
        gateway_instance.add_floating_ip(floating_ip)
    except BadRequest as exc:
        logging.critical('The procedure needs to be restarted. '
                         'Exception occurred: %s', exc)
        gateway_instance.delete()
        sys.exit(1)

    # Manage ssh security group
    if cloudManager.ssh_security_group:
        gateway_instance.add_security_group(cloudManager.ssh_security_group)

    instances.append(gateway_instance)

    # Create worker instances
    for instance_id in range(1, args.nbServers):
        worker_instance = cloudManager.nova_servers_create(instance_id,
                                                           userdata_node)
        instances.append(worker_instance)

    instance_id = 'swarm'
    userdata_swarm_mgr = cloudManager.build_cloudconfig(cloudmanager.SWARM_MANAGER,
                                                        args.nbServers-1)
    swarm_instance = cloudManager.nova_servers_create(instance_id,
                                                      userdata_swarm_mgr)
    instances.append(swarm_instance)

    envfile_tpl = '''# Parameters related to Openstack platform
# Used by Swarm
SWARM_NODE="{}"
# Used by shmux
HOSTNAME_TPL="{}"
WORKER_LAST_ID="{}"
'''

    worker_last_id = args.nbServers - 1
    envfile = envfile_tpl.format(swarm_instance.name,
                                 cloudManager.get_hostname_tpl(),
                                 worker_last_id)
    filep = open('integration-tests-env.sh', 'w')
    filep.write(envfile)
    filep.close()

    cloudManager.print_ssh_config(instances, floating_ip)

    # Wait for cloud config completion for all machines
    for instance in instances:
        cloudManager.detect_end_cloud_config(instance)

    cloudManager.check_ssh_up(instances)

    cloudManager.update_etc_hosts(instances)

    logging.debug("SUCCESS: Qserv Openstack cluster is up")


if __name__ == "__main__":
    try:
        # Define command-line arguments
        parser = argparse.ArgumentParser(description='Boot instances from image containing Docker.')
        parser.add_argument('-n', '--nb-servers', dest='nbServers',
                            required=False, default=3, type=int,
                            help='Choose the number of servers to boot')

        cloudmanager.add_parser_args(parser)
        args = parser.parse_args()

        loggerName = "Provisioner"
        cloudmanager.config_logger(loggerName, args.verbose, args.verboseAll)

        cloudManager = cloudmanager.CloudManager(config_file_name=args.configFile,
                                                 used_image_key=cloudmanager.SNAPSHOT_IMAGE_KEY,
                                                 add_ssh_key=True)

        main()
    except Exception as exc:
        logging.critical('Exception occurred: %s', exc, exc_info=True)
        sys.exit(1)
