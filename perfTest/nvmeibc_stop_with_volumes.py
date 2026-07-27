#!/usr/bin/env python3
#----------------------------------------
# stress the service stop operation with volumes attached
# a test iteration consists of:
# 1) attach a list of volumes
# 2) start btest for a few sec
# 3) kill btest
# 4) stop service
# we count on the service stop/stop to re-attach all volumes without any CLI.
# we do need to wait for these volumes to be IO-able.
# run this as root
###############################################################################

import sys
import time
import os
import subprocess
import argparse
btest_thread_count = 10     # btest parameter
btest_rw_rand = 50          # btest parameter

def debug(str):
	print(str);

# get the full path of the block device from its name
def device_name_to_device_path(dev_name):
	return "/dev/nvmesh/" + dev_name


# return exit code of operation
def cli_sync(cmd):
	print("CLI:" + cmd);
	result = os.system(cmd)
	return (result >> 8)

# create a process & return it to caller for control.
def cli_async(cmd, need_sudo, need_stdout_err):
	myout = None
	myerr = None
	if need_stdout_err == True:
		myout = subprocess.PIPE
		myerr = subprocess.PIPE
	print("CLI:" + str(cmd));
	return subprocess.Popen(cmd, stdout=myout, stderr=myerr, close_fds=True);

# start btest to generate IO on the device
def start_io_btest(vol_name):
	cli_tokens = ["./io_stress/btest", "-t", "1", "-T", str(btest_thread_count),
					"-D", "-B", "300000", "R", str(btest_rw_rand),
					device_name_to_device_path(vol_name)]
	btest_process = cli_async(cli_tokens, True, False);
	return btest_process


def attach_test_volumes(volumes):
	for v in volumes:
		cli_sync("NVMESH_vol_attach " + v)

# wait for a single volume to be IO-able
def wait_for_volume_IOable(vol_name):
	full_vol_name = "/proc/nvmeibc/volumes/" + vol_name + "/status";
	is_ioable = False
	while is_ioable == False:
		try:
			file = open(full_vol_name, 'r')
			for line in file:
				if "IO is currently enabled" in line:
					is_ioable = True
			file.close()
		except IOError:
			debug("volume %s hasnt attached yet" % (vol_name))
			time.sleep(1)   # wait for attach

# wait for all volumes to be IO-able after attach
def wait_for_all_volume_ready(volumes):
	for v in volumes:
		wait_for_volume_IOable(v)


def test_single_iteration(volumes):
	vol2proc = {}
	cli_sync("service nvmeshclient start");
	wait_for_all_volume_ready(volumes)
	if args.use_btest:
		for v in volumes:
			proc = start_io_btest(v)
			vol2proc[v] = proc
		time.sleep(1)
		for v in volumes:
			proc = vol2proc[v]
			proc.wait() # btest should exit after 1 sec of IO
	cli_sync("service nvmeshclient stop")
	if os.path.exists("/proc/nvmeibc/status"):
		print("ERROR - nvmeibc module wasnt removed !!!")
		os.exit(-1);


def main(argv):
	parser = argparse.ArgumentParser()
	parser.add_argument('volumes',  nargs='+', help='Enter volume(s) names, e.g.: jbod-1 vol-0') # The list of volumes to use (attached) while the service is started/stopped. btest is started for each of these.
	parser.add_argument('-n', dest="repeat", type=int, default=1, help='Enter the number of times to repeat the test')
	parser.add_argument('-b', dest="use_btest", action='store_true', default=False, help='whether each volume should have a btest run before we stop the service or not')
	args = parser.parse_args()
	print(args);
	attach_test_volumes(args.volumes)
	for i in range (1,args.repeat+1):
		debug("test # %d" %(i))
		test_single_iteration(args.volumes)

if __name__ == "__main__":
	main(sys.argv[1:])
