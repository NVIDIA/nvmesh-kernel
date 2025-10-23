#!/usr/bin/python3

##############################################################################
#  Copyright (C) 2015-2018 Excelero, Inc. All Rights Reserved.               #
#                                                                            #
#  This file is part of Excelero NVMesh software.                            #
#                                                                            #
#  Unauthorized copying of this file, via any medium is strictly prohibited  #
#  Proprietary and confidential                                              #
##############################################################################
import os
import sys
import argparse
import logging
import logging.handlers
import traceback
import atexit
import time
import subprocess
import json
import socket
BUNDLE_MODE = hasattr(sys, '_MEIPASS')
if not BUNDLE_MODE:
	sys.path.append('/opt/nvmesh/client-repo')
from management_cm import nvmesh_script_utils

@atexit.register
def flush_logger(*args, **kwargs):
	logging.shutdown()

# Checks all client instances are in ready state, no re-read partitions is ongoing and set it to off
def check_client_upgrade_possible(multi_client_utils):
	logger = multi_client_utils.logger
	if not multi_client_utils.is_client_state_ready():
		logger.error("Client state is not Ready, cannot issue upgrade")
		return False
	instaces_reread_partition = multi_client_utils.get_read_partitions()
	logging.debug("clnt instances: {0}".format(instaces_reread_partition))
	res = True
	for instance_name, instance_info in instaces_reread_partition.items():
		if instance_info[0] is None and instance_info[1] is None:
			logger.warning('Cannot verify re-read partition, upgrade will commence in 10 seconds please abort by pressing CTRL+C.')
			try:
				time.sleep(10)
			except KeyboardInterrupt as err:
				logger.warning('Upgrade aborted manually.')
				res = False
			else:
				logger.warning('Proceeding with upgrade.')
			return res
		if instance_info[0] != 0:
			logger.error("Client instance {0} has re-read paritions pending, cannot continue with upgrade".format(instance_name))
			res = False
	# If all instances do not have re-read ongoing, stop it for all instances Then verify again
	if res:
		multi_client_utils.set_atom_read_part_to_off()
		# Refresh values - since we canceled read partition this is just sanity
		instaces_reread_partition = multi_client_utils.get_read_partitions()
		for instance_name, instance_info in instaces_reread_partition.items():
			if instance_info[0] != 0:
				logger.error("Client instance {0} has re-read paritions pending, cannot continue with upgrade".format(instance_name))
				res = False
	return res

def do_client_shutdown(multi_client_utils, is_upgrade):
	logger = multi_client_utils.logger
	if is_upgrade:
		logger.warning("Client shutdown-for-upgrade requested")
		cmd_line = "upgr_shutdown"
		if multi_client_utils.is_client_state_already_in_upgrade():
			logger.warning("Upgrade already running, this command has no effect")
			return
		if not check_client_upgrade_possible(multi_client_utils):
			multi_client_utils.handle_error_and_exit("Client upgrade aborted", 1)
	else:
		logger.warning("Client shutdown-no-upgrade requested")
		cmd_line = "shutdown"
	with multi_client_utils.open_proc_file() as command_file:
		try:
			os.write(command_file, cmd_line.encode('utf-8'))
		except OSError as err:
			error_string = "Write error communicating with Client. ErrorID: 1001; File: {0}; Error: ({1}, {2})".format(multi_client_utils.proc_file, str(err), err.errno)
			multi_client_utils.handle_error_and_exit(error_string, error_code=1001)
	while multi_client_utils.wait_client_zombie() == False:
		time.sleep(0.01)
	logger.warning("Please proceeed...")

class maxLevelFilter(logging.Filter):
    def filter(self, rec):
        return rec.levelno < logging.WARNING

def init_shut_down_logger(json, debug):
	my_logger = logging.getLogger('nvmesh_shut_down')	# prefix for logger
	stderr_handler = logging.StreamHandler(sys.stderr)
	stderr_handler.setFormatter(logging.Formatter('%(message)s'))
	stderr_handler.setLevel(logging.WARNING)
	stdout_handler = logging.StreamHandler(sys.stdout)
	stdout_handler.setFormatter(logging.Formatter('%(message)s'))
	stdout_handler.addFilter(maxLevelFilter())
	my_logger.addHandler(stdout_handler)
	my_logger.addHandler(stderr_handler)
	if json:
		my_logger.setLevel(logging.CRITICAL)
		return my_logger
	if debug:
		my_logger.setLevel(logging.DEBUG)
		return my_logger
	try:
		with open("/sys/module/nvmeibc/parameters/debug_level") as f:
			debug_level = f.readline()
			if '2' in debug_level:
				my_logger.setLevel(logging.DEBUG)
			elif '1' in debug_level:
				my_logger.setLevel(logging.INFO)
			else:
				my_logger.setLevel(logging.WARNING)
	except Exception:
		my_logger.setLevel(logging.ERROR)
		my_logger.error("Request cannot be executed, as Client does not seem to be running, module is down. ErrorID: 1008;")
	return my_logger

if __name__ == "__main__":
	arg_parser = argparse.ArgumentParser("nvmesh_shut_down")
	arg_parser.add_argument('-u', '--upgrade', action='store_true', help='upgrade')
	arg_parser.add_argument("--debug", action='store_true', help="print debug information")
	arg_parser.add_argument('--json', action='store_true', help=argparse.SUPPRESS)
	args = arg_parser.parse_args(sys.argv[1:])
	args.client_shutdown = True
	my_logger = init_shut_down_logger(args.json, args.debug)
	instance_name = 'nvmeibc'
	dev_root = 'nvmesh'
	multi_client_utils = nvmesh_script_utils.MultiClientUtils(my_logger, instance_name, dev_root, args.json)
	subprocess.run(['logger', '-t', 'nvmesh_clnt_shutdown', 'Shutdown process started'])

	# Check root user
	if os.geteuid() != 0: # Non root user
		multi_client_utils.handle_error_and_exit("Running shut-down commands should be done as root user.")

	with multi_client_utils.open_lock_file():
		try:
			if args.client_shutdown:
				do_client_shutdown(multi_client_utils, args.upgrade)
		except (Exception, KeyboardInterrupt) as e:
			if isinstance(e, KeyboardInterrupt):
				my_logger.error("Request interrupted by signal. There may be unexpected results. ErrorID: 1012;")
			else:
				my_logger.error("Unexpected error. ErrorID: 1013, Error: {0}".format(str(e)))
				my_logger.error("Internal Error. Traceback: {0}".format(traceback.format_exc()))
			multi_client_utils.print_json_output()
			sys.exit(nvmesh_script_utils.RETRY_ERROR_CODE)
	multi_client_utils.print_json_output()
	subprocess.run(['logger', '-t', 'nvmesh_clnt_shutdown', 'Shutdown process complete'])
