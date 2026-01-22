#!/usr/bin/python2 -u

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import sys
import csv
import subprocess
import time
import getopt
import re
import tempfile
import math
import os
import json
import time
from tempfile import mkstemp
import uuid
from os.path import basename

nvme_cli_path = "/opt/nvmesh/target-repo/scripts/nvme-cli/"
dd_path = "dd"
max_num_retries = 5

def confirm(dev_name, format_all, all_disks = None):
    if format_all:
        prompt = 'Are you sure you want to format the following drives:\n%s\n[%s]|%s: ' % ('\n'.join(disk['dev_name'] for disk in all_disks), 'y', 'n')
    else:
        prompt = 'Are you sure you want to format %s [%s]|%s: ' % (dev_name, 'n', 'y')

    while True:
        ans = raw_input(prompt)
        if not ans:
            return False
        if ans not in ['y', 'Y', 'n', 'N']:
            print 'please enter y or n.'
            continue
        if ans == 'y' or ans == 'Y':
            return True
        if ans == 'n' or ans == 'N':
            return False



def print_usage():
    print "Usage:"
    print "nvmesh_format.py --diskId=<disk serial number> --vendorId<vendor id> [--format_all] [-y] [--ec] [-b block_size] [-m metadata_size]\n"
    print "instead of diskId + vendorId it is possible to provide dev_name with --dev_name"
    print "if --format_all is used, all devices on this machine will be formatted"
    print "if -y is used, all questions will be ignored and assumed as yes"
    print "if --ec is used, then the disk will be formatting with erasure coding support,\n"
    print "   in such case valid block_size and metadata_size need to be upplied.\n"
    print "--version will print the format version"


def freeze_disk(disk, freeze=1, msg=""):
    disk_sequence = int(disk['seq'])
   
    freeze_str = "{0}{1}".format(freeze, msg)
    freeze_cmd = "echo -n {0} > /proc/nvmeibs/freeze{1}".format(freeze_str, disk_sequence)
    for i in range(1, max_num_retries):
        p = subprocess.Popen(freeze_cmd, shell=True)
        out, err = p.communicate()
        p.wait()
        if p.returncode:
            print ("Error during freeze: {0} sleeping {1} seconds before retry".format(str(err), i))
            time.sleep(i)
        else:
            if (i > 1):
                print ("TOMAwarn: freeze succeeded on iteration={0}".format(i))
            return

    print ("Error during freeze: {0} - aborting format".format(str(err)))
    sys.exit(2)

def idns_cmd(disk):
    dev_name = disk['dev_name']
    idns_cmd_args = "{0}/nvme id-ns -o json {1}".format(nvme_cli_path, dev_name)
    for i in range(1, max_num_retries):
        p = subprocess.Popen(idns_cmd_args, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()
        if p.returncode:
            print ("{0}/nvme id-ns {1}  error: {2} sleeping {3} seconds before retry".format(nvme_cli_path, dev_name, str(err), i))
            time.sleep(i)
        else:
            if(i > 1):
                print ("TOMAwarn: id-ns on dev={0} succeeded on iteration={1}".format(dev_name, i))
            return out

    print ("{0}/nvme id-ns {1} error, aborting format. err: {2}".format(nvme_cli_path, dev_name, str(err)))
    # unfreeze disk - to enable this failure to be re-entrant.
    freeze_disk(disk, 0, "Format_Error")
    raise NameError("{0}".format(dev_name))


def idctrl_cmd(dev_name):
    idctrl_cmd_args = "{0}/nvme id-ctrl -o json {1}".format(nvme_cli_path, dev_name)
    for i in range(1, max_num_retries):
        p = subprocess.Popen(idctrl_cmd_args, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()
        if p.returncode:
            print ("{0}/nvme id-ctrl {1}  error: {2} sleeping {3} seconds before retry".format(nvme_cli_path, dev_name, str(err), i))
            time.sleep(i)
        else:
            if(i > 1):
                print ("TOMAwarn: id-ctrl on dev={0} succeeded on iteration={1}".format(dev_name, i))
            return out

    print ("{0}/nvme id-ctrl {1}  error: {2} aborting format".format(nvme_cli_path, dev_name, str(err)))
    raise NameError("{0}".format(dev_name))


def list_cmd():
    list_cmd_args = "{0}/nvme list -o json".format(nvme_cli_path)
    for i in range(1, max_num_retries):
        p = subprocess.Popen(list_cmd_args, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()
        if p.returncode:
            print ("{0}/nvme list  error: {1} sleeping {2} seconds before retry".format(nvme_cli_path, str(err), i))
            time.sleep(i)
        else:
            if(i > 1):
                print ("TOMAwarn: nvme-list succeeded on iteration={0}".format(i))
            return out

    print ("{0}/nvme list  error: {1} aborting format".format(nvme_cli_path, str(err)))
    raise NameError("{0}".format(dev_name))

def list_nvme_device_paths():
    devices_list = []

    # Parse nvme-list result
    json_obj = json.loads(list_cmd())
    i = 0
    if json_obj:
        for device in json_obj['Devices']:
            devices_list.append(device['DevicePath'])
            i = i + 1

    return devices_list


def get_lbaf(disk, format_block_size, format_metadata_size):
    # Parse id-ns result
    json_obj = json.loads(idns_cmd(disk))
    i = 0
    for list_item in json_obj['lbafs']:
        ds = pow(2, int(list_item['ds']))
        ms = int(list_item['ms'])
        if ds == format_block_size and ms == format_metadata_size:
            return i
        i = i + 1

    return -1

def get_n_blocks(disk):
    # Parse id-ns result
    json_obj = json.loads(idns_cmd(disk))
    return int(json_obj['nsze'])

def get_metadata_type(disk):
    # Parse id-ns result
    json_obj = json.loads(idns_cmd(disk))
    if int(json_obj['mc'] > 1): #mc = 2 or mc = 3 means extended md is supported.
        return 0
    return 1

def do_format(disk, format_block_size, format_metadata_size):
    dev_name = disk['dev_name']
    # Issue format command based on disk parameters
    lbaf = get_lbaf(disk, format_block_size, format_metadata_size)
    md_type = get_metadata_type(disk)
    if lbaf < 0:
        print ("Format Error: The disk does not support formatting with block_size={0}, metadata_size={1} - aborting format".format(format_block_size, format_metadata_size))
        freeze_disk(disk, 0, "Format_Error")
        sys.exit(2)

    # Issue format command to nvme device with correct lba formatt which was retrieved by the parameters.
    nvme_format_args = ['{0}/nvme'.format(nvme_cli_path), 'format', '{0}'.format(dev_name), '-l {0}'.format(lbaf), '-m {0}'.format(md_type)]
    print ("{0}/nvme format command: {1}".format(nvme_cli_path, " ".join(nvme_format_args)))
    p = subprocess.Popen(nvme_format_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (out, err) = p.communicate()
    p.wait()
    is_firmware_reset = False
    if "FW_NEEDS_CONVENTIONAL_RESET" in str(err):
        is_firmware_reset = True
        print ("is_firmware_reset = True!")
    if p.returncode and not is_firmware_reset:
        print ("Format error, aborting format err: {0}".format(str(err)))
        freeze_disk(disk, 0, "Format_Error")
        sys.exit(2)

def write_format_status(disk, lba_s, block_size, format_block_size, format_metadata_size, ec_capable, disk_guid, format_request_counter):
    dev_name = disk['dev_name']
    # Generate format status file
    tmp_file_name = tempfile.mktemp('')
    f = open(tmp_file_name, 'w')

    report_block_size = block_size
    report_metadata_size = 0
    if ec_capable:
        report_block_size = format_block_size
        report_metadata_size = format_metadata_size

    f.write("NVMESH_FORMATTED_DISK, block_size={0}, metadata_size={1}, is_ec_supported={2}, disk_guid={3}, format_request_counter={4}\n".format(report_block_size, report_metadata_size, int(ec_capable), str(disk_guid), format_request_counter))
    f.close()

    # Write format status to the first block of the disk so that TOMA will be able to know how it was formatted
    known_value_signal_args = [dd_path, 'if={0}'.format(tmp_file_name), 'of={0}'.format(dev_name), 'ibs={0}'.format(block_size), 'bs={0}'.format(block_size), 'seek={0}'.format(lba_s), 'count=1', 'status=noxfer']
    for i in range(1, max_num_retries):
        p = subprocess.Popen(known_value_signal_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()
        if p.returncode:
            print ("Error writing format status: {0}".format(str(err)))
            time.sleep(i)
        else:
            if(i > 1):
                print ("TOMAwarn: write_format_status succeeded on iteration={0}".format(i))
            # Remove temp file.
            os.remove(tmp_file_name)
            return


    print ("Error writing format status, aborting format err: {0}".format(str(err)))
    freeze_disk(disk, 0, "Format_Error")
    os.remove(tmp_file_name)
    sys.exit(2)

def write_format_start(disk, lba_s, block_size):
    dev_name = disk['dev_name']
    # Generate format start file
    tmp_file_name = tempfile.mktemp('')
    f = open(tmp_file_name, 'w')

    f.write("Being formatted by NVMesh\n")
    f.close()

    # Write "Being formatted by NVMesh" to the first block to allow troubleshooting offline disks, in case of format failures.
    known_value_signal_args = [dd_path, 'if={0}'.format(tmp_file_name), 'of={0}'.format(dev_name), 'ibs={0}'.format(block_size), 'bs={0}'.format(block_size), 'seek={0}'.format(lba_s), 'count=1', 'status=noxfer']
    print ("write_format_start dd command: {0}".format(" ".join(known_value_signal_args)))
    for i in range(1, max_num_retries):
        p = subprocess.Popen(known_value_signal_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()
        if p.returncode:
            print ("Error writing format start err: {0}  out: {0}. sleeping {0} seconds before retry".format(str(err), str(out), i))
            time.sleep(i)
        else:
            if i > 1:
                print ("TOMAwarn: write_format_start succeeded on iteration={0}".format(i))
            # Remove temp file.
            os.remove(tmp_file_name)
            return

    print ("Error writing format start - aborting format err: {0}".format(str(err)))
    freeze_disk(disk, 0, "Format_Error")
    os.remove(tmp_file_name)
    sys.exit(2)

def format_disk(disk, ec_capable, format_block_size, format_metadata_size, disk_guid, format_request_counter):
    dev_name = disk['dev_name']
    num_blocks = int(disk['blocks'])
    block_size = int(disk['block_size'])
    lba_s = 0
    lba_e = num_blocks - 1

    print("Formatting device: {0} started.  ec_capable: {0}  disk_guid: {0}".format(dev_name, ec_capable, str(disk_guid)))

    # Freeze disk - so that TOMA won't know about it, for now. (and won't try to read/write to/from it during format)
    freeze_disk(disk, 1)

    # Erase first 2 blocks to destroy any previous MBR/GPT
    dd_args_start = [dd_path, 'if=/dev/zero', 'of={0}'.format(dev_name), 'ibs={0}'.format(block_size), 'bs={0}'.format(block_size), 'seek={0}'.format(lba_s), 'count=2', 'status=noxfer']
    print ("Running: {0}".format(str(dd_args_start)))
    p = subprocess.Popen(dd_args_start, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (out, err) = p.communicate()
    p.wait()
    if p.returncode:
        print ("Error destroying MBR/GPT - aborting format. err: {0}".format(str(err)))
        freeze_disk(disk, 0, "Format_Error")
        sys.exit(2)

    # Erase last block of device. (backup GPT location)
    dd_args_end = [dd_path, 'if=/dev/zero', 'of={0}'.format(dev_name), 'ibs={0}'.format(block_size), 'bs={0}'.format(block_size), 'seek={0}'.format(lba_e), 'count=1', 'status=noxfer']
    print ("dd_args_end: {0}".format(str(dd_args_end)))
    p = subprocess.Popen(dd_args_end, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    (out, err) = p.communicate()
    p.wait()
    if p.returncode:
        freeze_disk(disk, 0, "Format_Error")
        print ("Error erasing end of disk: {0} - aborting format err: {1}".format(dev_name, str(err)))
        sys.exit(2)

    print "format started with parameters: block_size={0} metadata_size={1} num_blocks={2}".format(format_block_size, format_metadata_size, num_blocks)
    do_format(disk, format_block_size, format_metadata_size)

    # write "R" to freeze file to signal target to refresh disk after format
    freeze_disk(disk, "R")

    # re-identify the disk again to get the correct number of blocks as the exact
    # number of blocks post format is vendor specific, due to internal disk metadata implementation.
    formatted_num_blocks = get_n_blocks(disk)

    print ("Num blocks after format: {0}".format(formatted_num_blocks))
    # Signal that disk format started.
    write_format_start(disk, lba_s, format_block_size)

    # Write status to signal TOMA that disk was formatted.
    write_format_status(disk, lba_s, block_size, format_block_size, format_metadata_size, ec_capable, disk_guid, format_request_counter)

    # Unfreeze disk - TOMA will see the disk again along with the format status
    freeze_disk(disk, 0, "Formatting")
    freeze_disk(disk, 0)

    print("Formatting device: {0} Finished successfully".format(dev_name))


def try_to_move_device_from_nvme_to_nvmeibs_if_needed(full_dev_name):
    dev_name = basename(full_dev_name)
    dev_seq = int(dev_name.lstrip("nvme").split("n")[0])
    if dev_seq >= 1000:
        print("device {0} is already bound to nvmeibs".format(dev_name))
    else:
        move_dev_to_nvmeibs_command_args = ['/opt/nvmesh/target-repo/scripts/nvmesh_auto_bind_nvme', '{0}'.format(dev_seq)]
        print ("move dev from nvme to nvmeibs command: {0}".format(" ".join(move_dev_to_nvmeibs_command_args)))
        p = subprocess.Popen(move_dev_to_nvmeibs_command_args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()

def locate_dev_name_by_diskid_and_vendor(input_disk_id, input_vendor_id):
    num_retries = 15
    while num_retries > 0:
        try:
            nvme_devices = list_nvme_device_paths()
            for dev_name in nvme_devices:
                # Parse id-ctrl result
                json_obj = json.loads(idctrl_cmd(dev_name))
                vendor_id = int(json_obj['vid'])
                # disk id is the disk_id.nsid easiest place to get nsid is from the dev_name.
                disk_id = str(json_obj['sn']).strip() + "." + str(dev_name.split("n")[-1])
                print("nvme device={0} disk_id={1} input_disk_id={2} vendor={3} input_vendor={4}".format(dev_name, disk_id, input_disk_id, vendor_id, input_vendor_id))
                if disk_id == input_disk_id and vendor_id == input_vendor_id:
                    print ("disk_id={0} vendor_id={1} matched with dev_name={2}".format(input_disk_id, input_vendor_id, dev_name))
                    return dev_name
        except NameError:
            print("Error doing idctrl for disk_id={0} vendor={1}, num_retries left ={2}".format(input_disk_id, input_vendor_id, num_retries))
            time.sleep(1)
        num_retries = num_retries - 1

    return None

def locate_diskid_and_vendor_by_dev_name(input_dev_file_name):
    num_retries = 15
    disk_id = None
    vendor_id = None

    while num_retries > 0:
        try:
            nvme_devices = list_nvme_device_paths()
            for dev_name in nvme_devices:
                print("dev_name={0} input_dev_file_name={1}".format(dev_name, input_dev_file_name))
                if dev_name == input_dev_file_name:
                    # Parse id-ctrl result
                    json_obj = json.loads(idctrl_cmd(dev_name))
                    vendor_id = int(json_obj['vid'])
                    disk_id = str(json_obj['sn']).strip() + "." + str(dev_name.split("n")[-1])
                    print ("dev_name={0} matched disk_id={1} vendor_id={2}".format(input_dev_file_name, disk_id, vendor_id))
                    return (disk_id, vendor_id)
        except NameError:
            print("Error doing idctrl for disk_id={0} vendor={1} input_dev_file_name={2} num_retries left ={3}".format(disk_id, vendor_id, input_dev_file_name, num_retries))
            time.sleep(1)
        num_retries = num_retries - 1

    return None

def main():
    disk_id = None
    vendor_id = None
    dev_file_name = None
    format_all = False
    auto_yes = False
    ec_capable = False
    format_block_size = 512
    format_metadata_size = 0
    found_device = False
    disk_guid = uuid.UUID('{00000000-0000-0000-0000-000000000000}')
    format_request_counter = -1
    num_retries = 5

    if len(sys.argv) < 2:
        print("No arguments passed")
        print_usage()
        sys.exit(1)

    try:
        opts, args = getopt.getopt(sys.argv[1:], "y", ["format_all", "diskId=", "vendorId=", "ec", "version", "blockSize=", "metadataSize=", "uuid=", "dev_name=", "format_request_counter="])
    except getopt.GetoptError as err:
        print "Input errors: " + str(err)
        print_usage()
        sys.exit(2)
    for opt, arg in opts:
       if opt == '-h':
          print_usage()
          sys.exit()
       elif opt in ('--diskId'):
           disk_id = str(arg)
       elif opt in ('--vendorId'):
           vendor_id = int(arg)
       elif opt in ('--format_all'):
           format_all = True
       elif opt in ('--version'):
           format_version = 2
           print ("Version = {0}".format(format_version))
           sys.exit(0)
       elif opt == '-y':
           auto_yes = True
           print "Using auto yes to all questions"
       elif opt == '--ec':
           ec_capable = True
       elif opt in ('--blockSize'):
           format_block_size = int(arg)
       elif opt in ('--metadataSize'):
           format_metadata_size = int(arg)
       elif opt in ('--uuid'):
           disk_guid = uuid.UUID('{0}'.format(arg))
       elif opt in ('--dev_name'):
           dev_file_name = arg
       elif opt in ('--format_request_counter'):
           format_request_counter = int(arg)
       else:
           print ("Unknown argument: %s" % (arg) )
           sys.exit(3)

    print("format command={0}".format(str(sys.argv)))

    if format_request_counter < 0:
        print ("Format request counter not supplied!")
        sys.exit(6)

    if ec_capable:
        if format_block_size == 0:
            print ("Format block size cannot be 0 in Erasure Coding format")
            sys.exit(3)

        if format_metadata_size == 0:
            print ("Format metadata size cannot be 0 in Erasure Coding format")
            sys.exit(3)

    if not format_all:
        if not dev_file_name:
            dev_file_name = locate_dev_name_by_diskid_and_vendor(disk_id, vendor_id)
            if not dev_file_name:
                print ("Device {0} of vendor {1} is not found".format(disk_id, vendor_id))
                sys.exit(4)
        else:
            disk_id, vendor_id = locate_diskid_and_vendor_by_dev_name(dev_file_name)
            if not disk_id or not vendor_id:
                print ("Device {0} cannot locate parameters, disk_id={1} vendor_id={2}".format(dev_file_name, disk_id, vendor_id))
                sys.exit(5)

        print ("Dev={0} disk_id={1} vendor_id={2}".format(dev_file_name, disk_id, vendor_id))
        try_to_move_device_from_nvme_to_nvmeibs_if_needed(dev_file_name)
        time.sleep(1)

    while num_retries > 0:
        # make a copy of the disks.csv to assure it is not changed while we read it
        # (csv reader reads file more than once hence not atomic)
        fd, disks_csv_copy = mkstemp()
        copy_disks_csv_command_argc = [dd_path, 'if=/proc/nvmeibs/disks.csv', 'of={0}'.format(disks_csv_copy), 'bs=1M', 'count=1']
        print ("dd command: {0}".format(" ".join(copy_disks_csv_command_argc)))
        p = subprocess.Popen(copy_disks_csv_command_argc, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        (out, err) = p.communicate()
        p.wait()
        if p.returncode:
            print ("Error doing copy_disks_csv_command_argc: {0}".format(str(err)))
            sys.exit(2)

        dev_file_name = locate_dev_name_by_diskid_and_vendor(disk_id, vendor_id)

        # parse disk file.
        with open(disks_csv_copy, 'rb') as csvfile:
            disks_csv = csv.DictReader(csvfile, delimiter=',')

            if format_all or dev_file_name:
                # Confirm action with user or ignore questions
                if not auto_yes and not confirm(dev_file_name, format_all, disks_csv):
                    print("Aborting..")
                    sys.exit(0)

                # Printing the names in confirm function causes disks_csv to end, resetting it here
                if not auto_yes:
                    csvfile.seek(0)

                for disk in disks_csv:
                    if disk['id'] == 'id':
                        continue
                    if format_all or (disk['dev_name'] == dev_file_name):
                        print ("Starting format for disk={0}".format(disk['id']))
                        found_device = True
                        format_disk(disk, ec_capable, format_block_size, format_metadata_size, disk_guid, format_request_counter)

        os.close(fd)
        os.remove(disks_csv_copy)

        if not found_device:
            print ("Device={0} vendor={1} not found in disks.csv, num_retries_left={2}".format(disk_id, vendor_id, num_retries))
            num_retries = num_retries - 1;
            time.sleep(1)
        else:

            print("Done")
            break;

    sys.exit(0)


if __name__ == "__main__":
    main()
