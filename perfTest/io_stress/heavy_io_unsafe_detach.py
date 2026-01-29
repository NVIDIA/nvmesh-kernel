#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# testing unsafe detach
# every test iteration does the following:
# 1) attach all volumes
# for BTEST_AND_FOPS flavor
#   2) start heavy IO with btest & manual_fops
#   3) while IO runs, detach all volumes
#   4) wait for btest to fail, due to block device failing incoming IO's
#   5) let manual_fops run for 1[sec] more
#   6) close the manual_fops & let the volume detach
#
# for MNT_ONLY flavor
#   2) mkfs & mount a filesystem over the volume
#   3) while mounted, detach all volumes
#   4) unmount & cleanup mount point
#
# for MNT_WITH_FILES  flavor
#   2) mkfs & mount a filesystem over the volume
#   3) create a random number of files with random size. flush all files to filesystem & when done, keep files open.
#   4) while mounted & with open files, detach all volumes
#   5) unmount & cleanup mount point
#
# for MAN_FOPS_TRAIL
#   2) start manual_fops with IO's (1 thread only)
#   3) wait 1 sec for IO's
#   4) while IO runs:
#       3.1) dump volume stats from /proc/nvmeibc/<vol name>/io_stats
#       3.2) detach volume (keep manual_fops firing). this keeps some of the the detached volume objects alive
#   when the required number of iterations complete:
#       a) verify that the number of api_os is identical to number of such test variants
#       b) close all manual_fops.
#       c) verify that the number of api_os is down to 0
#   The trail of open manual_fops & the "struct block_device" they hold are freed only when the script ends (or if the process dies causing the child processes to also get killed).
#   Another limit is on the trail length bcz eventually the OS limit of open files/processes is reached. this limit closes the oldest one  to ensure no more than max allowed are alive.
#
##############################################################
import sys
import time
import os
import traceback
import subprocess
import argparse
import random
import re
import errno
import json
import contextlib
import fcntl


# IO range on volume
io_block_start = 0
io_block__count = 1024*1024*1024    # 1 GB

git_repo_dir = "../../bin"
MANUAL_FOPS_EXE_PATH = "./manual_fops/manual_fops"

#default path for root of all mount points
default_mnt_path = "/mnt"

BLK_SIZE = 4096     # 4KB

FILE_LINE_SIZE = 128    # size which is a divisor of a BLK_SIZE

# a single NVMesh lockset size
BLK_SET_IN_BLOCKS = 32 # 128 KB

# The maximum number of older block devices held orpahn.
# when this limit is reached, a detach will close the oldest client, to let the oldest block device within the kernel free itself (prevent hitting resource limit)
max_detach_tail_length = 30

#btest parameters
btest_thread_count = 2
btest_rw_rand = 50      # % of random read/writes

wait_for_ios_sec = 2

# TODO(EBA): allow -d in cli ???
def debug(str):
	print(str);

def get_root_mount_path():
	fs_dir = "nvmesh_detach_tests"
	if args.root_mount_path:
		path = args.root_mount_path
	else:
		path = default_mnt_path
	path +=  "/" + fs_dir + "/"
	return path

def create_root_mount_path():
	assert (cli_sync("mkdir -p " + get_root_mount_path(), True) == 0)

def del_root_mount_path():
	if os.path.isdir(get_root_mount_path()):
		return cli_sync("rmdir " + get_root_mount_path(), True)

# match the mount point for every volume
def vol_name_to_mount_name(vol_name):
	return get_root_mount_path() + vol_name + "/"

# get the full path of the block device from its name
def device_name_to_device_path(dev_name):
	return "/dev/nvmesh/" + dev_name

# return exit code of operation
def cli_sync(cmd, need_sudo):
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


def get_vol_cli_list():
	vol_list = ""
	for v in args.volumes :
		vol_list += v + ",";
	return vol_list

# attach all NVMesh volumes listed
def attach_test_volumes ():
	cmd = "NVMESH_vol_attach " + get_vol_cli_list();
	cli_sync(cmd, True)


# detach all NVMesh volumes listed
def detach_test_volumes ():
	cmd = "NVMESH_vol_detach " + get_vol_cli_list() + " --force";
	cli_sync(cmd, True)

# get the number of such objects. this represent volumes that were attached but not yet freed.
def get_live_api_os_obj():
	ATOM_STATUS_JSON = "/proc/nvmeiba/status.json"
	MAX_ATOM_STATUS_READ = 128*1024*2
	atom_file = os.open(ATOM_STATUS_JSON, os.O_RDONLY)
	try:
		atom_info = json.loads(os.read(atom_file, MAX_ATOM_STATUS_READ))
	except:
		print("Error parsing ATOM info.");
		return -1
	finally:
		os.close(atom_file)
	counters = atom_info.get("counters")
	count = counters.get("atoms")
	print("has {0} atoms".format(count));
	return count

# start btest to generate IO on the device
def start_io_btest(vol_name):
	# use '0' time for infinite - bcz the IO's will stop once the device is closed & the btest will fail.
	cli_tokens = ["./btestEX", "-q", "-t", "0", "-T", str(btest_thread_count),
					"-D", "-B", "300000", "R", str(btest_rw_rand),
					device_name_to_device_path(vol_name)]
	btest_process = cli_async(cli_tokens, True, False);
	return btest_process

# parse the output of the manual IO tool to get its pid
# output format: "My thread id: 32035/140069287438144"....
def parse_manual_fops_output(proc):
	#out, err = proc.communicate()
	pid = None
	while True:
		line = proc.stdout.readline()
		#print("output line:" + line);
		if "My thread id:" in line:
			print("found pid line:" + line);
			pid = line.split(":")[1].split("/")[0].strip()
			break;
	return pid


manual_io_ops = re.compile(r'io_success_count = (?P<success>[0-9]+), io_fail_count = (?P<fail>[0-9]+)')
# parse the resulting IO's
# result line is "io_success_count = 0, io_fail_count = 50"
def parse_manual_fops_result(proc):
	io_sucecss_count = -1
	io_fail_count = -1
	while True:
		line = proc.stdout.readline()
		#line = "io_success_count = 29, io_fail_count = 0"      # debugging
		#print("output line:" + line);
		m = manual_io_ops.search(line)
		if m != None:
			io_sucecss_count = m.group("success")
			io_fail_count = m.group("fail")
			#if (io_sucecss_count != None) and (io_fail_count != None):
			print("success %s, fail %s" % (io_sucecss_count, io_fail_count));
			return int(io_sucecss_count), int(io_fail_count)

# start our 'manual_fops' test tool to generate open/close/IO's on the device
def start_manual_fops(vol_name):
	spawn_cli_tokens = [MANUAL_FOPS_EXE_PATH,
					"-b", device_name_to_device_path(vol_name),  # the volume we test
					"-d", "10",                       # 10 msec delay between IO's
					"-t", "10"                        # number of threads to fire in parallel
					]
	manual_fops_process = cli_async(spawn_cli_tokens, True, True)
	print("child manual_fops pid is:%d" % (manual_fops_process.pid));
	pid = parse_manual_fops_output(manual_fops_process)
	print("manual_fops pid is:" + pid);
	cli_sync("kill -10 " + pid, True)
	cli_sync("kill -12 " + pid, True)
	return manual_fops_process, pid


# close & stop the manual_fops client
def stop_manual_fops(proc, pid_str):
    print("shutdown manual_fops pid " + pid_str);
    cli_sync("kill -12 " + pid_str, True)    # stop IO's
    cli_sync("kill -10 " + pid_str, True)    # close nvmesh block device (or file)
    io_sucecss_count, io_fail_count = parse_manual_fops_result(proc)
    cli_sync("kill -15 " + pid_str, True)    # ask manual IO server to terminate
    return io_sucecss_count, io_fail_count

# mount the given nvmesh volume under 'get_root_mount_path()' with same name
# return mount name
def mount_ext4(vol_name):
	vol_path = device_name_to_device_path(vol_name)
	mount_name = vol_name_to_mount_name(vol_name)
	result = cli_sync("mkfs -t ext4 " + vol_path, True)
	#print("mkfs returned %d" % (result));
	assert result == 0
	result = cli_sync("mkdir -p " + mount_name, True)
	assert result == 0
	result = cli_sync("mount " + vol_path + " " + mount_name, True)
	assert result == 0
	return mount_name

# create a 128B line for the file, including the new-line
#"-------------------------------------------------------- line 123456 -----------------------------------------------------------\n"
def make_single_file_line(line_num):
	str = "--------------------------------------------------------- "
	str += "line %6s" % (line_num)
	str += " ---------------------------------------------------------\n"
	assert(len(str) == FILE_LINE_SIZE)
	return str


# open some files & create with random sizes
def open_files(vol_name):
	files = []
	mount_name = vol_name_to_mount_name(vol_name)
	num_files = random.randint(1,5)     # number of files to create
	print("vol %s has %d files:" % (vol_name, num_files));
	for file_idx in range(0,num_files):
		file_name = mount_name + str(file_idx) +".txt"
		file_size_blks = random.randint(1, 2 * BLK_SET_IN_BLOCKS)   # get a file size from 1 block up to 2 lock-sets
		file_size_bytes = file_size_blks * BLK_SIZE
		assert ((file_size_bytes % FILE_LINE_SIZE) == 0)
		lines_in_file = file_size_bytes / FILE_LINE_SIZE
		print("\tfile %s, size %d bytes, %d lines" % (file_name, file_size_bytes, lines_in_file ));
		file = open(file_name, 'w')     # Note we keep the file open !!!
		files.append(file)
		for line_idx in range(1,lines_in_file):
			file.write(make_single_file_line(line_idx))
		file.flush()
		os.fsync(file)
	assert len(files) == num_files
	return files

# check whether a volume is mounted or not
def is_mounted(vol_name):
	mount_name = vol_name_to_mount_name(vol_name)
	if (os.path.exists(get_root_mount_path()) == False):
		return False
	if (os.path.isdir(get_root_mount_path()) == True):
		return True
	assert(False)   # path exists but isnt a directory so environment is corrupted

# unmount a file system & remove mount point
def unmount_ext4(vol_name, vol_info):
	mount_name = vol_name_to_mount_name(vol_name)
	#print("umount:" + mount_name);
	result = cli_sync("umount -f " + mount_name, True)
	if result != 0:
		print("umount result: %d" % (result));
		cli_sync("lsof " + mount_name, True)
		if vol_info:
			for of in vol_info.open_files:
				print("file " + of.name + ":" + str(of));
				print(of);
	assert ((result == 0) or args.clean)
	result = cli_sync("rmdir " + mount_name, True)
	#print("rmdir result:" + str(result));
	assert ((result == 0) or args.clean)

#def umount_cleanup(vol_name):
#    mount_name = vol_name_to_mount_name(vol_name)
#    assert cli_sync("rmdir " + mount_name, True)

def wait_for_process_exit(p, vol_name):
    p.wait()
#    while p.poll() is not None:
#        print("waiting for btest on " + vol_name + " to fail...");
#        time.sleep(1)


#
class VolDetachVariant:
	FIRST           = 1
	BTEST_AND_FOPS  = 1     # used as raw block device
	MNT_ONLY        = 2     # we mount a filesystem but no open files
	MNT_WITH_FILES  = 3     # we mount & open some files.
	MAN_FOPS_TRAIL  = 4
	LAST            = 4

# the information we have for every volume we test
class volume_info(object):
	def __init__(self, vol_name, variant):
		self.vol_name = vol_name
		self.btest_proc = None          # the 'btest' client process
		self.manual_fops_proc = None    # the 'manual_fops' client process
		self.manual_fops_pid = None     # the 'manual_fops' process ID, parsed from its stdout
		self.variant = variant
		self.open_files = None          # a list of open files objects on a mounted file system (if any exist)
		self.manual_fops = []           # all processes of manual_fops that werent killed

def test_attach_unsafe_detach(man_fops_trail_vols):
	next_variant = VolDetachVariant.BTEST_AND_FOPS
	print("Attaching volumes...");
	attach_test_volumes()
	print("----- volume list -----");
	cli_sync("ls -l /proc/nvmeibc/volumes/", False)
	cli_sync("ls -l /dev/nvmesh", False)
	vol2proc = {}
	for vol in args.volumes:
		vol_info = volume_info(vol, next_variant)
		if next_variant == VolDetachVariant.LAST:
			next_variant == VolDetachVariant.FIRST
		else:
			next_variant+=1
		# lest start each variant
		if vol_info.variant == VolDetachVariant.BTEST_AND_FOPS:
			print("firing btest for volume " + vol);
			vol_info.btest_proc = start_io_btest(vol)
			vol_info.manual_fops_proc, vol_info.manual_fops_pid = start_manual_fops(vol)
		elif vol_info.variant == VolDetachVariant.MNT_ONLY:
			mount_ext4(vol)
		elif vol_info.variant == VolDetachVariant.MNT_WITH_FILES:
			mount_ext4(vol)
			vol_info.open_files = open_files(vol)
		elif vol_info.variant == VolDetachVariant.MAN_FOPS_TRAIL:
			vol_info.manual_fops_proc, vol_info.manual_fops_pid = start_manual_fops(vol)
		else:
			print("Invalid detach variant:" + str(vol_info.variant));
			exit(-5)
		vol2proc[vol] = vol_info
	print("sleep "+ str(wait_for_ios_sec) + " sec to let IO's start...");
	time.sleep(wait_for_ios_sec)
	print("----- mounted volumes -----");
	cli_sync("ls -l " + get_root_mount_path(), True)
	print("detaching volumes...");
	detach_test_volumes()
	print("detached all volumes");
	print("waiting for IO btest clients to exit...");
	for vol in vol2proc :
		vol_info = vol2proc[vol]
		if vol_info.variant == VolDetachVariant.BTEST_AND_FOPS:
			wait_for_process_exit(vol2proc[vol].btest_proc, vol)
	print("let the manual fops client keep firing post detach for 1 [sec]...");
	time.sleep(1)
	print("waiting for manual fops client process to exit...");
	for vol in vol2proc :
		vol_info = vol2proc[vol]
		if vol_info.variant == VolDetachVariant.BTEST_AND_FOPS:
			io_sucecss_count, io_fail_count = stop_manual_fops(vol_info.manual_fops_proc, vol_info.manual_fops_pid)
			assert(io_sucecss_count > 0)
			assert(io_fail_count > 0)
			wait_for_process_exit(vol_info.manual_fops_proc, vol)
		elif vol_info.variant == VolDetachVariant.MNT_ONLY:
			unmount_ext4(vol_info.vol_name, None)
		elif vol_info.variant == VolDetachVariant.MNT_WITH_FILES:
			debug('close %d file(s) on volume %s' % (len(vol_info.open_files), vol_info.vol_name))
			for of in vol_info.open_files:
				of.close()
			unmount_ext4(vol_info.vol_name, vol_info)
		elif vol_info.variant == VolDetachVariant.MAN_FOPS_TRAIL:
			# chain the volume & process into a list to keep all instances of libe detached volume
			man_fops_trail_vols.append(vol_info)
			if len(man_fops_trail_vols) > max_detach_tail_length:
				oldest = man_fops_trail_vols.pop(0)
				print("max orphan block devices tail reached. closing oldest one (manual_ops.pid=%s)" % (oldest.manual_fops_pid));
				io_sucecss_count, io_fail_count = stop_manual_fops(oldest.manual_fops_proc, oldest.manual_fops_pid)
				assert(io_sucecss_count > 0)
				assert(io_fail_count > 0)
				wait_for_process_exit(oldest.manual_fops_proc, vol)
		else:
			print("Invalid detach variant:" + str(vol_info.variant));
			exit(-5)
	print("config state post termination of IO processes");
	print("----------- list of attached volumes -----------");
	cli_sync("ls -l /proc/nvmeibc/volumes/", False)
	print("----------- list of attached devices -----------");
	cli_sync("ls -l /dev/nvmesh", False)
	live_obj = get_live_api_os_obj()
	print("Verifying all api_os objects were released: count=%d" % (live_obj))
	assert(live_obj == len(man_fops_trail_vols))    # only the live manual_fops hold a live object inside kernel

#*******************************************************************************
# Testing code
#*******************************************************************************

# test spawn of manual_fops & then parse its output
def ut__parse_manual_fops_result():
	spawn_cli_tokens = [MANUAL_FOPS_EXE_PATH,
					"-f", "/tmp/123.txt",             # the file we test
					"-d", "10",                       # 10 msec delay between IO's
					"-t", "10"                        # number of threads to fire in parallel
					]
	process = cli_async(spawn_cli_tokens, True, True)
	cli_sync("kill -10 " + str(process.pid), True)
	cli_sync("kill -12 " + str(process.pid), True)
	io_sucecss_count, io_fail_count = parse_manual_fops_result(process)
	print("%d, %d" % (io_sucecss_count, io_fail_count));
	cli_sync("kill -12 " + str(process.pid), True)
	cli_sync("kill -10 " + str(process.pid), True)
	cli_sync("kill -15 " + str(process.pid), True)
	#process.join()                                  # no need to wait for the process to terminate bcz we asked it to stop IO's & close the file (block dev).

#*******************************************************************************
#               MAIN
# Note:
# 1) This script needs to run as sudo for:
#   1.1) access to block devices under "/dev/"
#   1.2) access to /proc/nvmeibc/
#   1.3) access to module param under /sys/module/nvmeibc/parameters/
#*******************************************************************************

# UT cases
#ut__parse_manual_fops_result()
#exit(1)

if __name__ == "__main__":
	parser = argparse.ArgumentParser()
	parser.add_argument('volumes',  nargs='+', help='Enter volume(s) names, e.g.: jbod-1 vol-0') # named of volumes on which we want to generate IO's & detahc while IO's are blasting
	parser.add_argument('-n', dest="repeat", type=int, default=1, help='Enter the number of times to repeat the test')
	parser.add_argument('-c', dest="clean", action='store_true', default=False, help='cleanup after a failed execution (use the same argumenst as the failed execution)')
	parser.add_argument('-m', dest="root_mount_path", default=default_mnt_path, help='The path where we mount all volumes')
	args = parser.parse_args()
	print("CLI args:" + str(args));
	if args.clean:
		print("cleaning up possible leftovers from previous session");
		print("\tkill btest ...");
		cli_sync("killall btest", True)
		print("kill manual_fops ...");
		cli_sync("killall manual_fops", True);
		print("unmount all NVMesh test file-systems...");
		for vol_name in args.volumes:   # we try all volumes, some will obviously fail bcz they werent mounted
			if is_mounted(vol_name):
				unmount_ext4(vol_name, None)
		del_root_mount_path()
		exit(0)
	try:
		if (not os.getuid() == 0):
			print("Error: must run as sudo");
			exit(-6);
		if (not os.path.isfile(MANUAL_FOPS_EXE_PATH)):
			print("Error: executable {0} does not exist".format(MANUAL_FOPS_EXE_PATH));
			exit(-7);

		cli_sync('sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level"', False)
		# verify no leftovers from previous run
		if (os.path.exists(get_root_mount_path()) == True):
			print('Error: stale state from previous execution. unmount & cleanup ' + get_root_mount_path());
			exit(-2)
		live_obj = get_live_api_os_obj()
		if (live_obj != 0):
			print("Error: NVMesh client has %d volume attached - detach all volume" % (live_obj))
			exit(-2)
		# prepare environment for execution
		create_root_mount_path()
		#execute test
		man_fops_trail_vols = []    #
		for i in range (0,args.repeat) :
			print("------------------------------------ Attempt # " + str(i) + " -------------------------------");
			test_attach_unsafe_detach(man_fops_trail_vols)
		# cleanup all live manual_fops
		time.sleep(1)   # let some IO's fail on all detached volumes
		print("closing %d manual_fops" % (len(man_fops_trail_vols)));
		for vol_info in man_fops_trail_vols:
			print("stopping vol %s" % (vol_info.vol_name));
			io_sucecss_count, io_fail_count = stop_manual_fops(vol_info.manual_fops_proc, vol_info.manual_fops_pid)
			assert(io_sucecss_count > 0)
			assert(io_fail_count > 0)
		live_obj = get_live_api_os_obj()
		print("Verifying all api_os objects were released: count=%d" % (live_obj))
		assert(live_obj == 0)

		# cleanup environment
		assert (del_root_mount_path() == 0)
		exit(0)
	except AssertionError:
		traceback.print_exc()
		print("----------- Doing cleanup -------------");
		print("\tkill btest ...");
		cli_sync("killall btest", True)
		print("kill manual_fops ...");
		cli_sync("killall manual_fops", True);
		exit(1)

