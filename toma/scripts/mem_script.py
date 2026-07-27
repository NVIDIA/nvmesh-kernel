#!/usr/bin/python3
# DanielHsH, 2025/Nov, this script is deprecated. Originally tried to find mem leaks from logs. Better use compiler for that, because logs can be missing

import os
import glob
import argparse
from collections import defaultdict
import re

IGNORE_SIZE_MISMATCH    = True
overide_files           = None #list of files

def main(log_dir, file1, file2):
    allocated_dicts = {'normal': defaultdict(lambda: [0, ""]), 'bm': defaultdict(lambda: [0, ""])}
    files = list_log_files(log_dir, file1, file2)
    for log in files:
        print("Opening log file={}".format(log))
        with open(log, 'r') as f:
            data = f.readlines()
            for line in data:
                line = line.strip()
                is_relevant, address, size, is_alloc, realloc_old_address, realloc_old_size, is_bm = parse_line(line)
                if (is_relevant):
                    allocated_dict = allocated_dicts['bm'] if is_bm else allocated_dicts['normal']
                    if (realloc_old_address):
                        make_sure_sizes_match_and_remove_from_dict(line, realloc_old_address, realloc_old_size, allocated_dict)
                    add_to_dict(address, size, is_alloc, line, allocated_dict)
    print("Not BM: ", end='')
    print_the_still_allocated(allocated_dicts['normal'])
    print("BM: ", end='')
    print_the_still_allocated(allocated_dicts['bm'])

def list_log_files(log_dir, file1, file2):
    files_list = []

    if (file1):
        files_list.append(file1)
        if (file2):
            files_list.append(file2)
    elif (os.path.isfile(log_dir)):
        files_list.append(log_dir)
    elif (os.path.isdir(log_dir)):
        files_list.extend(glob.glob(os.path.join(log_dir,'toma*.log')))
    else:
        print("Some issue with one of the paths. file1={}, file2={}, log_dir={}".format(file1, file2, log_dir))

    files_list = overide_files if overide_files else files_list
    return files_list

def parse_line(line):
    pattern = r' ((BM_)?((M|C|RE|ALIGNED_)?ALLOC|FREE))( prev=(\(nil\)|(0x[0-9a-f]+)))? p=(0x[0-9a-f]+)( prev_size=(\d+))? size=(\d+)'
    is_relevant         = False
    address             = None
    size                = None
    is_alloc            = None
    realloc_old_address = None
    realloc_old_size    = None
    is_bm               = None

    match = re.search(pattern, line)

    if (match):
        is_bm               = bool(match.group(2))
        is_relevant         = True
        address             = int(match.group(8),0)
        size                = int(match.group(11))
        is_alloc            = match.group(1).endswith("ALLOC")
        is_realloc          = match.group(7)
        if (match.group(7)):
            realloc_old_address = int(match.group(7),0)
            realloc_old_size    = int(match.group(10))

    return is_relevant, address, size, is_alloc, realloc_old_address, realloc_old_size, is_bm

def add_to_dict(address, size, is_malloc, line, allocated_dict):
    if (is_malloc):
        make_sure_not_already_allocated_and_add(line, address, size, allocated_dict)
    else:
        make_sure_sizes_match_and_remove_from_dict(line, address, size, allocated_dict)

def make_sure_not_already_allocated_and_add(line, address, size, allocated_dict):
    if (allocated_dict[address][0]):
        print("Parsing: {}, already parsed: {} and no free in between, ignoring older allocation, something is wrong with log, consider aborting".format(line, allocated_dict[address][1]))
    allocated_dict[address] = [size, line]

def make_sure_sizes_match_and_remove_from_dict(line, address, size, allocated_dict):
    if (allocated_dict[address][0] == size or IGNORE_SIZE_MISMATCH) or (allocated_dict[address][0] == 0 and MISSING_LOGS_START):
        allocated_dict[address][0] = 0
    else:
        print("Free size and alloc size mismatch: {}, already parsed: {} and no free in between, ignoring older allocation, something is wrong with log, consider aborting".format(line, allocated_dict[address][1]))

def print_the_still_allocated(allocated_dict):
    print("These lines weren't freed")
    total_size = 0
    for address, (size, line) in allocated_dict.items():
        if (size):
            total_size += size
            print(line)
    print("Total={}".format(total_size))
    #add stats per file, function, line

if __name__ == "__main__":
    working_dir = os.path.realpath(os.path.join(os.getcwd(), os.path.dirname(__file__)))
    parser = argparse.ArgumentParser(description = 'Memory Allocations Analyzing Script')
    parser.add_argument('--log_dir', default=working_dir, help='The path to toma_*.log')
    #add argument for changing the prefix for the log file
    parser.add_argument('--file1', help='The path to the first log file')
    parser.add_argument('--file2', help='The path to the second and last log file')
    #add argument for printing all allocations
    #add argument for not fresh logs - MISSING_LOGS_START
    #add flag to ignore size mismatch - IGNORE_SIZE_MISMATCH
    args = parser.parse_args()
    main(args.log_dir, args.file1, args.file2)

