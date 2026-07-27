#!/usr/bin/env python3

import os
import time
from collections import deque
from nvmesh_metrics import MetricFactory, MetricBase, Gauge, BytesHistogram
import argparse

CLIENT_PROC_FILE = '/proc/nvmeibc/memmgr_info'
SERVER_PROC_FILE = '/proc/nvmeibs/memmgr_info'

# Compute the delta between two samples and memory volume
def compute_delta(prev_sample, curr_sample, prev_distributions):
    delta = {}
    total_allocated_mem = 0

    for curr_hist in curr_sample:
        if isinstance(curr_hist, BytesHistogram) and curr_hist.name == 'memory.allocation_distribution':
            label = ";".join(curr_hist.labels)
            prev_hist = prev_distributions.get(label)
            if prev_hist is None:
                prev_hist = BytesHistogram(curr_hist.name, ";".join(curr_hist.labels))  # empty histogram

            diff_hist = curr_hist.subtract(prev_hist)
            volume_diff = diff_hist.total_volume

            if volume_diff != 0:
                delta[label] = volume_diff
                total_allocated_mem += volume_diff

            prev_distributions[label] = curr_hist

    return delta, total_allocated_mem

# Function to get the list of volumes
def get_volumes():
    base_path = '/proc/nvmeibc/volumes'
    return [d for d in os.listdir(base_path) if os.path.isdir(os.path.join(base_path, d))]

def get_server_disks():
    base_path = '/proc/nvmeibs/disks'
    return [d for d in os.listdir(base_path) if os.path.isdir(os.path.join(base_path, d))]

def get_client_disks():
    base_path = '/proc/nvmeibc/disks'
    return [d for d in os.listdir(base_path) if os.path.isdir(os.path.join(base_path, d))]

def get_disks(proc_file):
    if proc_file == CLIENT_PROC_FILE:
        return get_client_disks()
    elif proc_file == SERVER_PROC_FILE:
        return get_server_disks()
    else:
        raise ValueError(f"Invalid proc file: {proc_file}")

def initialize_prev_distributions(curr_sample):
    prev_distributions = {}
    for metric in curr_sample['metrics.all_cpus']:
        if metric['type'] == 'bytes_histogram' and metric['name'] == 'memory.allocation_distribution':
            label = metric['labels']
            prev_distributions[label] = metric['values']
    return prev_distributions

# Function to read IOPS from /proc/nvmeibc/volumes/*/iostats
def read_iops():
    total_ops = 0
    for volume in get_volumes():
        with open(f'/proc/nvmeibc/volumes/{volume}/iostats', 'r') as f:
            for line in f:
                if 'num_ops' in line:
                    parts = line.split('|')[1:]
                    for part in parts:
                        total_ops += sum(int(num) for num in part.split())
                    break
    return total_ops

"""Format the value with appropriate units (binary: KB, MB, GB; decimal: K, M, G)."""
def format_with_units(value, unit_type='binary'):
    if unit_type == 'binary':
        if value >= 2**30:
            return f"{value // 2**30} GB"
        elif value >= 2**20:
            return f"{value // 2**20} MB"
        elif value >= 2**10:
            return f"{value // 2**10} KB"
        else:
            return f"{value} B"
    elif unit_type == 'decimal':
        if value >= 10**9:
            return f"{value // 10**9} G"
        elif value >= 10**6:
            return f"{value // 10**6} M"
        elif value >= 10**3:
            return f"{value // 10**3} K"
        else:
            return f"{value}"
    else:
        return f"{value}"
    

def print_table(proc_file, show_per_drive, iops_diff, sorted_table_data, total_allocated, total_active_allocations, total_allocation_last_sec, n_disks):
    print(f"{'Component':<40} {'Allocated':<20} {'Active Allocations':<20} {'Allocated/sec':<20} {'Bytes/IO':<10}" + (f"{'Alloc/Drive':<15} {'Active/Drive':<15} {'AllocSec/Drive':<15}" if show_per_drive  else ""))
    
    num_drives = 0
    num_drives_str = ""
    if show_per_drive:
        num_drives = n_disks if n_disks is not None else len(get_disks(proc_file))
        num_drives_str = f"{num_drives}"
        if num_drives == 0:
            num_drives = 1  # Avoid division by zero
            num_drives_str = "No drives"

    per_drive_info = ""
    for component, values in sorted_table_data:
        bytes_per_io = values['allocation_last_sec'] // iops_diff if iops_diff != 0 else 0
        if show_per_drive:
            alloc_per_drive = values['allocated'].counter // num_drives
            active_per_drive = values['active_allocations'].counter // num_drives
            alloc_sec_per_drive = values['allocation_last_sec'] // num_drives
            per_drive_info = f" {alloc_per_drive:<15} {active_per_drive:<15} {alloc_sec_per_drive:<15}"

        print(f"{component:<40} {values['allocated'].counter:<20} {values['active_allocations'].counter:<15} {values['allocation_last_sec']:<15} {bytes_per_io:<10}{per_drive_info}")

    per_drive_total_info = ""
    if show_per_drive:
        total_alloc_per_drive = total_allocated.counter // num_drives
        total_active_per_drive = total_active_allocations.counter // num_drives
        total_alloc_sec_per_drive = total_allocation_last_sec // num_drives
        per_drive_total_info = f" {total_alloc_per_drive:<15} {total_active_per_drive:<15} {total_alloc_sec_per_drive:<15}"

    total_bytes_per_io = total_allocation_last_sec // iops_diff if iops_diff != 0 else 0
        
    print(f"{'Total':<40} {total_allocated.counter:<20} {total_active_allocations.counter:<15} {total_allocation_last_sec:<15} {total_bytes_per_io:<10}{per_drive_total_info}")

    per_drive_units_info = ""
    if show_per_drive:
        per_drive_units_info = f" {format_with_units(total_alloc_per_drive):<15} {format_with_units(total_active_per_drive, unit_type='decimal'):<15} {format_with_units(total_alloc_sec_per_drive):<15}"

    print(f"{'Total (units)':<40} {format_with_units(total_allocated.counter):<20} {format_with_units(total_active_allocations.counter, unit_type='decimal'):<15} {format_with_units(total_allocation_last_sec):<15} {format_with_units(total_bytes_per_io):<10}{per_drive_units_info}")

    drives_info = f", Drives: {num_drives_str}" if show_per_drive else ""
    print(f"Total IOPS: {int(iops_diff)}{drives_info}")

    print("\n" + "-" * 150 + "\n")


def main(proc_file=CLIENT_PROC_FILE, show_per_drive=False, n_disks=None):
    iops_queue = deque(maxlen=1)

    # Initial sample, reading from file path
    prev_sample = MetricBase.read_memmgr_info(proc_file)
    prev_distributions = {}
    for metric in prev_sample:
        if isinstance(metric, BytesHistogram) and metric.name == 'memory.allocation_distribution':
            label = ";".join(metric.labels)
            prev_distributions[label] = metric  # store full histogram object

    prev_iops = read_iops()

    while True:
        time.sleep(1)

        curr_sample = MetricBase.read_memmgr_info(proc_file)

        # Compute the delta and total allocated memory
        delta, total_allocated_mem = compute_delta(prev_sample, curr_sample, prev_distributions)

        curr_iops = read_iops()
        iops_diff = curr_iops - prev_iops
        prev_iops = curr_iops

        table_data = {}
        total_allocated = Gauge("total_allocated", "", 0)
        total_active_allocations = Gauge("total_active_allocations", "", 0)
        total_allocation_last_sec = 0

        for metric in curr_sample:
            if isinstance(metric, Gauge) and metric.name in ['memory.allocated', 'memory.active_allocations']:
                labels = ";".join(metric.labels)
                component = next((label.split('=')[1] for label in metric.labels if label.startswith("component=")), None)
                if not component:
                    continue
                value = metric.counter

                if metric.name == 'memory.allocated':
                    if component not in table_data:
                        table_data[component] = {'allocated': Gauge(metric.name, labels, 0), 'active_allocations': Gauge(metric.name, labels, 0), 'allocation_last_sec': 0}
                    table_data[component]['allocated'].update(value)
                    total_allocated.update(value)

                elif metric.name == 'memory.active_allocations':
                    if component not in table_data:
                        table_data[component] = {'allocated': Gauge(metric.name, labels, 0), 'active_allocations': Gauge(metric.name, labels, 0), 'allocation_last_sec': 0}
                    table_data[component]['active_allocations'].update(value)
                    total_active_allocations.update(value)
                    allocation_last_sec = delta.get(labels, 0)
                    table_data[component]['allocation_last_sec'] = allocation_last_sec
                    total_allocation_last_sec += allocation_last_sec

        # Sort the table data by component name
        sorted_table_data = sorted(table_data.items())

        print_table(proc_file, show_per_drive, iops_diff, sorted_table_data, total_allocated, total_active_allocations, total_allocation_last_sec, n_disks)
        # Update the previous sample
        prev_sample = curr_sample


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Monitor NVMesh memory manager statistics')
    group = parser.add_mutually_exclusive_group(required=False)
    group.add_argument('--client', action='store_true', 
                      help='Read from ' + CLIENT_PROC_FILE + ' (client)')
    group.add_argument('--server', action='store_true',
                      help='Read from ' + SERVER_PROC_FILE + ' (server)')
    
    parser.add_argument('--by-disk', action='store_true',
                       help='Show per-drive statistics by dividing values by number of drives')
    parser.add_argument('--file', type=str,
                       help='Specify custom proc file path (overrides --client/--server)')
    parser.add_argument('--n_disks', type=int,
                       help='Override number of disks for per-drive calculations (optional, auto-detected if not specified)')
    
    args = parser.parse_args()
    
    # Determine proc file: --file takes precedence, then --server, default to client
    if args.file:
        proc_file = args.file
    elif args.server:
        proc_file = SERVER_PROC_FILE
    else:
        # Default to client if no argument or --client specified
        proc_file = CLIENT_PROC_FILE
    
    # Pass proc_file and by_disk flag to main()
    main(proc_file, args.by_disk, args.n_disks)
