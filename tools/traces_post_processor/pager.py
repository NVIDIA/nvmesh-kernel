#!/usr/bin/python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import re
import argparse
import sys
import os
import subprocess
import textwrap
import time
import datetime
import contextlib
import shutil
import tempfile
import fcntl
import errno
import logging
import json
import csv
import re
import functools

from itertools import chain
from shutil import copyfile
from collections import defaultdict, deque, OrderedDict, namedtuple
from typing import Deque, List, Tuple

BUNDLE_MODE = hasattr(sys, '_MEIPASS')

# ---------------- Utility Functions and Classes for BW Analysis ----------------
def parse_bytes(s: str) -> int:
    """
    Convert a human-readable byte string into integer bytes.
    """
    s = s.strip().upper()
    match = re.fullmatch(r'(\d+)(B|KB|MB|GB|TB)?', s)
    if not match:
        raise ValueError(f"Invalid byte string: {s}")

    number, unit = match.groups()
    number = int(number)
    multiplier = {
        'B': 1,
        'KB': 1024,
        'MB': 1024**2,
        'GB': 1024**3,
        'TB': 1024**4,
        None: 1,  # no unit defaults to bytes
    }[unit]

    return number * multiplier

def format_bytes(size):
    units = ['B', 'KB', 'MB', 'GB', 'TB']
    i = 0
    while size >= 1024 and i < len(units) - 1:
        size /= 1024
        i += 1
    if units[i] in ['B', 'KB']:
        return f"{int(size)} {units[i]}"
    else:
        return f"{size:.2f} {units[i]}"

MessageInfo = namedtuple('MessageInfo', ["issued_at", "size", "hostname", "severity", "cpu_id", "trace_name", "func_name", "channel"])
#issued_at: datetime
#size: int
#hostname: str = ""
#severity: str = ""
#cpu_id: int = 0
#trace_name: str = ""
#func_name: str = ""
#channel: str = ""

def parse_csv_stats_only_stream(stream):
    NS_PER_SEC = 1_000_000_000
    reader = csv.reader(stream)

    # Skip the header line
    try:
        next(reader)
    except StopIteration:
        return

    for row in reader:
        try:
            (
                hostname,
                severity,
                cpu_id,
                nanoseconds,
                binary_size,
                trace_name,
                func_name,
                channel,
            ) = row
            ns = int(nanoseconds)
            size = int(binary_size)

            if ns == 0 or size == 0:
                continue

            seconds = ns // NS_PER_SEC
            microseconds = (ns % NS_PER_SEC) // 1000
            issued_at = datetime.datetime.fromtimestamp(seconds).replace(
                microsecond=microseconds
            )

            yield MessageInfo(
                issued_at=issued_at,
                size=size,
                hostname=hostname,
                severity=severity,
                cpu_id=int(cpu_id),
                trace_name=trace_name,
                func_name=func_name,
                channel=channel,
            )
        except (ValueError, IndexError) as e:
            logging.error(f"Failed to parse CSV row: {row}. Error: {e}")
            continue

# -------- Trace Info ----------------
@functools.total_ordering
class TraceInfo:
    def __init__(self):
        self.binary_size = 0
        self.count = 0

    @property
    def as_tuple(self):
        return (self.binary_size, self.count)

    def __eq__(self, other):
        return self.as_tuple == other.as_tuple

    def __lt__(self, other):
        return self.as_tuple < other.as_tuple

    def update(self, msg: MessageInfo):
        self.binary_size += msg.size
        self.count += 1

    @property
    def average(self):
        return self.binary_size // self.count if self.count else 0

    def merge(self, other: 'TraceInfo'):
        self.binary_size += other.binary_size
        self.count += other.count

# -------- TracesStatistics --------------
class TracesStatistics:
    KEYS = ('trace_name', 'func_name', 'channel')

    def __init__(self):
        self.stats = {key: defaultdict(TraceInfo) for key in self.KEYS}

    def add(self, msg: MessageInfo):
        for key in self.KEYS:
            self.stats[key][getattr(msg, key)].update(msg)

    def merge(self, other_stats: 'TracesStatistics'):
        for key in self.KEYS:
            for name, info in other_stats.stats[key].items():
                self.stats[key][name].merge(info)

    def print_top(self, top=10):
        for key in self.KEYS:
            label = f"Top {top}" if top > 0 else "All"
            print(f"  --- {label} by {key} ---")
            sorted_items = sorted(
                self.stats[key].items(),
                key=lambda kv: kv[1].binary_size,
                reverse=True
            )
            items_to_print = sorted_items[:top] if top > 0 else sorted_items
            for name, info in items_to_print:
                print(f"    - {name}: {format_bytes(info.binary_size)} (count {info.count}, avg {format_bytes(info.average)})")
            print()

# ---------------- Cell ----------------
class Cell:
	start_dt: datetime
	bytes: int = 0
	stats: TracesStatistics

	def __init__(self, start_dt: datetime, bytes_: int):
		self.start_dt = start_dt 
		self.bytes = bytes_ 
		self.stats = TracesStatistics()



# ---------------- Sliding Window ----------------
class BWSlidingWindow:
    def __init__(self, period: datetime.timedelta, bytes_limit: int):
        self.period = period
        self.bytes_limit = bytes_limit
        self.cells = deque()
        self.window_sum = 0
        self.total_limit = self.bytes_limit * int(self.period.total_seconds())

    @property
    def has_overflow(self) -> bool:
        return self.window_sum > self.total_limit

    def _evict_old_cells(self, current_dt: datetime):
        left_bound = current_dt - self.period
        while self.cells and self.cells[0].start_dt <= left_bound:
            old = self.cells.popleft()
            self.window_sum -= old.bytes
        if self.window_sum < 0:
            raise RuntimeError("BUG: window_sum < 0 after eviction")

    def add(self, msg: MessageInfo):
        if msg.size <= 0:
            raise ValueError("size must be non-negative")

        sec_start_dt = msg.issued_at.replace(microsecond=0)
       # Check if the deque is empty or if the new message is from a new second
        if not self.cells or sec_start_dt > self.cells[-1].start_dt:
            self._evict_old_cells(sec_start_dt)
            self.cells.append(Cell(sec_start_dt, 0))

        self.cells[-1].bytes += msg.size
        self.cells[-1].stats.add(msg)
        self.window_sum += msg.size

    def get_overflow_info(self) -> List[Cell]:
        if not self.has_overflow:
            return []

        # Find the index of the first cell that exceeds the per-second byte limit.
        try:
            start_idx = next(i for i, c in enumerate(self.cells) if c.bytes > self.bytes_limit)
        except StopIteration:
            # This case should not be reached
            raise RuntimeError("Overflow detected but no cell exceeded the limit.")

        # Efficiently create and return a list with only the overflowing cells.
        return [self.cells[i] for i in range(start_idx, len(self.cells))]

# ---------------- OverflowAccumulator ----------------
def datetime_to_ns(dt: datetime) -> int:
    return int(dt.timestamp() * 1_000_000_000)

# The OverflowAccumulator class efficiently merges new "cells" (time-stamped data)
# into existing intervals, handling both adjacent and overlapping cases.
# It uses deques for O(1) performance on appends and pops from the right side.
class OverflowAccumulator:
    def __init__(self, inactivity_period: datetime.timedelta):
        self.inactivity_period = inactivity_period
        # Store intervals as a list of deques for O(1) appends and pops
        self.intervals: List[Deque[Cell]] = []

    def update(self, new_cells: List[Cell]):
        if not new_cells:
            return

        # Convert the incoming list of cells to a deque for efficient operations
        new_cells_deque = deque(new_cells)

        if not self.intervals:
            self.intervals.append(new_cells_deque)
            return

        last_interval = self.intervals[-1]
        last_start = last_interval[-1].start_dt
        # The end of the last cell's second
        last_end = last_start + datetime.timedelta(seconds=1)
        new_start = new_cells_deque[0].start_dt

        # Check if the new cells can be merged with the last interval.
        # This condition allows a merge if the gap between the end of the last cell
        # and the start of the new one is within the inactivity period.
        if new_start <= last_end + self.inactivity_period:
            # Case 1: Disjoint/Adjacent but within inactivity window
            if new_start > last_start:
                # O(1) extension of the deque
                last_interval.extend(new_cells_deque)
            # Case 2: Overlap - trim the end of the last interval and append new cells
            else:
                # Efficiently remove overlapping cells from the right side (O(1) pops)
                while last_interval and last_interval[-1].start_dt >= new_start:
                    last_interval.pop()
                last_interval.extend(new_cells_deque)
        else:
            # Case 3: Too far apart → start a new interval
            self.intervals.append(new_cells_deque)

    def get_intervals(self) -> List[Deque[Cell]]:
        return self.intervals

    def _print_summary(self, idx: int, cells: Deque[Cell]):
        start_dt = cells[0].start_dt
        end_dt = cells[-1].start_dt

        display_end_dt = end_dt + datetime.timedelta(seconds=1)

        start_ns = datetime_to_ns(start_dt)
        end_ns = datetime_to_ns(display_end_dt)

        total_bytes = sum(c.bytes for c in cells)
        duration_s = (display_end_dt - start_dt).total_seconds()
        avg_bw = total_bytes / duration_s

        print(
            f"Interval #{idx}: {start_dt.strftime('%H:%M:%S')} → {display_end_dt.strftime('%H:%M:%S')} "
            f"({start_ns}, {end_ns}) | duration {int(duration_s)}s, "
            f"total {format_bytes(total_bytes)}, avg bandwidth {format_bytes(avg_bw)}/s"
        )

        for c in cells:
            t1 = datetime_to_ns(c.start_dt)
            t2 = datetime_to_ns(c.start_dt + datetime.timedelta(seconds=1))
            print(f"    {c.start_dt.strftime('%H:%M:%S')}: {format_bytes(c.bytes)} ({t1},{t2})")
        print()

    def _print_statistics(self, cells: Deque[Cell], top_n: int = 10):
        interval_stats = TracesStatistics()
        for c in cells:
            interval_stats.merge(c.stats)

        print("\n  --- Top Traces/Functions in this Interval ---")
        interval_stats.print_top(top=top_n)
        print()

    def print_intervals(self, print_stats: bool = True, top_n: int = 10):
        for idx, cells in enumerate(self.intervals, start=1):
            self._print_summary(idx, cells)
            if print_stats:
                self._print_statistics(cells, top_n)

# ---------------- BW Analysis Mode ----------------
def run_bw_analysis_mode(stream, args):
    if parse_bytes(args.bw_limit_str) <= 0 or args.window_size <= 0:
        logging.error("BW limit and window size must be positive")
        sys.exit(1)

    bwsw = BWSlidingWindow(
        period=datetime.timedelta(seconds=args.window_size),
        bytes_limit=parse_bytes(args.bw_limit_str)
    )
    overflow_acc = OverflowAccumulator(
        inactivity_period=datetime.timedelta(seconds=args.inactivity_period)
    )

    for msg in parse_csv_stats_only_stream(stream):
        bwsw.add(msg)
        if bwsw.has_overflow:
            overflow_acc.update(bwsw.get_overflow_info())

    overflow_acc.print_intervals(print_stats=True, top_n=args.top_n)


@contextlib.contextmanager
def tempdir():
    try:
        dirpath = tempfile.mkdtemp()
        yield dirpath
    finally:
        shutil.rmtree(dirpath)

class SmartFormatter(argparse.ArgumentDefaultsHelpFormatter):
    """ Utility: Just used for nicer looking help
    """

    def __init__(self, prog, indent_increment=2, max_help_position=25, width=105):
        super(SmartFormatter, self).__init__(prog, indent_increment, max_help_position, width)

    def smartsplit(self, line, width):
        match = re.match(r'^\s*(?:\d+\.?|\*|\-)?\s*', line)
        if match:
            indent = len(match.group())
            split = super(SmartFormatter, self)._split_lines(
                line, width - indent)
            res = []
            match = re.match(r'^\s+', line)
            if not split:
                return [' ']
            if match:
                res.append(' '*len(match.group()) + split[0])
            else:
                res.append(split[0])
            for line in split[1:]:
                res.append(' '*indent + line)
            return res
        return super(SmartFormatter, self)._split_lines(line, width)

    def _split_lines(self, text, width):
        if text.startswith('R|'):
            res = []
            for line in text[2:].splitlines():
                res.extend(self.smartsplit(line, width))
        else:
            res = super(SmartFormatter, self)._split_lines(text, width)

        return res

    def _fill_text(self, text, width, indent):
        return '\n'.join(indent + line for line in self._split_lines(text, width))


class ExtendConstAction(argparse.Action):
    """ Argparse action used to extend arguments list
        Strictly based on argparse append_const
    """
    def __init__(self,
                 option_strings,
                 dest,
                 const,
                 default=None,
                 required=False,
                 help=None,
                 metavar=None):
        super(ExtendConstAction, self).__init__(
            option_strings=option_strings,
            dest=dest,
            nargs=0,
            const=const,
            default=default,
            required=required,
            help=help,
            metavar=metavar)

    def __call__(self, parser, namespace, values, option_string=None):
        if (hasattr(namespace, self.dest)):
            items = getattr(namespace, self.dest)
        else:
            items = []
        items.extend(self.const)
        setattr(namespace, self.dest, items)

class ExtendAction(argparse.Action):
    """ Argparse action used to extend arguments list
        Strictly based on argparse append_const
    """

    def __call__(self, parser, namespace, values, option_string=None):
        if (hasattr(namespace, self.dest)):
            items = getattr(namespace, self.dest)
        else:
            items = []
        items.extend(values)
        setattr(namespace, self.dest, items)


class StoreOneOrTwo(argparse.Action):
    """ Custom argparse action, store 1 or 2 args
    """

    def __init__(self, option_strings, dest, nargs=None, default=(None, None), **kwargs):
        if nargs is not None:
            raise ValueError("nargs not allowed")
        if len(default) != 2:
            raise ValueError("default must be of length 2")
        super(StoreOneOrTwo, self).__init__(
            option_strings, dest, default=default, nargs='*', **kwargs)

    def __call__(self, parser, namespace, values, option_string=None):
        if (self.required and len(values) < 2):
            raise argparse.ArgumentTypeError(
                'Argument "{0}" is required'.format(self.dest))

        if (len(values) > 2):
            raise argparse.ArgumentTypeError(
                'Too many values supplied for argument "{0}"'.format(self.dest))

        dest = [self.default[0], self.default[1]]
        setattr(namespace, self.dest, dest)

        if (len(values) > 0):
            dest[0] = values[0]
        if (len(values) > 1):
            dest[1] = values[1]

class TomaShortcutAction(ExtendConstAction):
    """ Argparse action used to set toma-specific arguments
    """
    def __call__(self, parser, namespace, values, option_string=None):
        super(TomaShortcutAction, self).__call__(parser, namespace, values, option_string)
        setattr(namespace, "no_print_cpu_id", True)

C_PAGER_EXEC_NAME = 'cpager'
def get_cpager_path():
    if BUNDLE_MODE:                          
        return os.path.join(sys._MEIPASS, C_PAGER_EXEC_NAME)  # type:
    return os.path.join(os.path.dirname(os.path.realpath(__file__)), C_PAGER_EXEC_NAME)                                                 

PARSER_EXE = get_cpager_path()
MAKE_DIR = os.path.dirname(os.path.realpath(__file__))

DEFAULT_REMOTE_DIR = '/var/log/nvmesh/trace_daemon'
REMOTE_PAGER = '/var/log/nvmesh/trace_daemon/pager'
FIFO_SIZE = 4*1024*1024

SILENT = False
DBG = False

IS_SIMULATOR = 'clnt/block/unitest' in os.getcwd()

OUT_TIME_FORMAT = '%d/%m/%Y %H:%M:%S'
SUPPORTED_TIME_FORMATS = [
    '%Y-%m-%d %H:%M:%S',
    '%Y-%m-%d %H:%M',
    '%m-%d %H:%M:%S',
    '%m-%d %H:%M',
    '%d/%m/%Y %H:%M:%S',
    '%d/%m/%Y %H:%M',
    '%d/%m %H:%M:%S',
    '%d/%m %H:%M',
    '%b %d %Y %H:%M:%S',
    '%b %d %Y %H:%M',
    '%b %d %H:%M:%S',
    '%b %d %H:%M',
    '%H:%M:%S',
    '%H:%M',
]

def eprint(*args):
    print >>sys.stderr, (", ".join(map(str, args)))

def wrprint(*args):
    if not SILENT:
        print(", ".join(map(str, args)))

def dbgprint(*args):
    if DBG:
        print(", ".join(map(str, args)))

def valid_datetime(s):
    """ Translate string @s representing multiple possible datetime formats
        to a valid string accepted by pager binary
    """

    s = s.strip(' \t').rstrip(' \t')
    # Integer timestamp
    match = re.match(r'^((?:0x)?\d+)$', s)
    if match:
        return str(int(match.group(1)))

    # Now - x
    if s == 'now':
        return 'now-0'
    match = re.match(r'^now\s*-\s*((?:0x)?\d+)(.?)$', s)
    if match:
        mul = 1
        if (match.group(2) == 's'):
            mul = 1000
        elif (match.group(2) == 'm'):
            mul = 1000*60
        elif (match.group(2) == 'h'):
            mul = 1000*60*60
        return 'now-' + str(mul*int(match.group(1)))

    # Tail- x
    if s == 'tail':
        return 'tail-0'
    match = re.match(r'^tail\s*-\s*((?:0x)?\d+)(.?)$', s)
    if match:
        mul = 1
        if (match.group(2) == 's'):
            mul = 1000
        elif (match.group(2) == 'm'):
            mul = 1000*60
        elif (match.group(2) == 'h'):
            mul = 1000*60*60
        return 'tail-' + str(mul*int(match.group(1)))

    for fmt in SUPPORTED_TIME_FORMATS:
        try:
            date_time_obj = datetime.datetime.strptime(s, fmt)
            if date_time_obj:
                if date_time_obj.date() == datetime.date(1900, 1, 1):  # If we have time only
                    today = datetime.datetime.today()
                    date_time_obj = date_time_obj.replace(
                        today.year, today.month, today.day)
                if date_time_obj.year == 1900:
                    today = datetime.datetime.today()
                    date_time_obj = date_time_obj.replace(today.year)
                return date_time_obj.strftime(OUT_TIME_FORMAT)
        except:
            pass

    raise argparse.ArgumentTypeError('Invalid time specified: {0}'.format(s))

def collector_root_dirs(d):
    """ Given a directory that is log collector extracted files root, create a list of directories
        that span all the hosts in that log collector's run.
    """
    popen = subprocess.Popen(['find', d, '-type', 'd', '-path', '*trace_daemon'], stdout=subprocess.PIPE, universal_newlines=True)
    lines = popen.stdout.readlines()
    popen.stdout.close()

    return [line.rstrip() for line in lines]

def parse_args(parser):
    args = parser.parse_args()

    if args.collector_dir:
        args.log_dirs = list(chain.from_iterable(collector_root_dirs(d) for d in args.log_dirs))

    if not args.channels:
        args.channels = get_default_channels()

    args.channels = set(args.channels)

    try:
        if args.since:
            args.time_frame = [valid_datetime(' '.join(args.since)), args.time_frame[1]]

        if args.until:
            args.time_frame = [args.time_frame[0], valid_datetime(' '.join(args.until))]
    except argparse.ArgumentTypeError as e:
        eprint(str(e))
        sys.exit(1)

    return args


DEFAULT_TIMEFRAME = (valid_datetime('0'), valid_datetime('18446744073709551615'))


def get_default_channels():
    """ Retrieve the channels user normally wants to see when running in a specific directory
    """
    if IS_SIMULATOR:
        return ['longterm.binlog', 'goodpath.binlog', 'ephemeral.binlog', 'eternal.binlog']
    else:
        return ['nvmeibc_trace_long', 'nvmeibc_trace_short', 'nvmeibc_trace_eph', 'nvmeibc_trace_eter', 'nvmeibs_trace_long', 'nvmeibs_trace_short', 'nvmeibs_trace_eph', 'nvmeibs_trace_eter', 'nvmeshum.long.binlog', 'nvmeshum.eter.binlog', 'nvmeshum.goodpath.binlog', 'eter_core', 'long_core', 'goodpath_core', 'eph_core']


def normalized_dirnames(dirs, is_remote):
    """ For input list of directories of format either host:dir or jsut dir,
        convert all to format host:dir (if more than 1 dir specified)
    """
    is_single_host = not is_remote and (len(dirs) <= 1)
    newdirs = []
    for dname in dirs:
        if is_single_host:
            # jsut one host, hostname shall be ignored
            hname = ''
        elif re.match(r'^[0-9a-zA-Z\-\_\.]*:', dname): # regex is according to hostname(3) spec
            # hostname supplied by user explicitly
            (hname, dname) = dname.split(':', 2)
        else:
            if is_remote:
                hname = dname
                dname = DEFAULT_REMOTE_DIR
            else:
                # try retrieve hostname from local hostname file
                try:
                    with open(os.path.join(dname, 'hostname'), 'r') as content_file: # Try read hostname from file
                        hname = content_file.read().rstrip(' \t\n').strip(' \t\n')
                except:
                    hname = dname # Fallback
        if not is_remote:
            dname = os.path.abspath(dname)
        newdirs.append('{0}:{1}'.format(hname, dname))
    return newdirs


def resulting_channels_list(dirs, channels, is_remote):
    """ Given list of directories and list of channels,
        normalize and create a unified list of all channels in all directories
    """
    res = []
    for d in normalized_dirnames(dirs, is_remote):
        for ch in channels:
            res.append(os.path.join(d, ch))
    return res

def host_channels_map(dirs, channels):
    """ Given a normalized list of resulting channels,
        denote a map from each hostname to list of channels
    """
    res = {}
    for ch in resulting_channels_list(dirs, channels, True):
        split = ch.split(':', 1)
        if not res.get(split[0]):
            res[split[0]] = []
        res[split[0]].append(ch)
    
    return res


def init_argparse():
    parser = argparse.ArgumentParser(
        'Pager wrapper', formatter_class=SmartFormatter, epilog='''R|\
EXAMPLES (GENERAL):
    pager.py # By default show client + server longterm only
    pager.py --client # Client
    pager.py --server # Server
    pager.py --toma # Toma
    pager.py --toma_util # Toma Utils
    pager.py --client --toma # Client and toma but not server
    pager.py -l nvmeibs_trace_long nvmeibc_trace_goodpath # Selecting specific channels
                                                            # (for advanced usage)
EXAMPLES (TIME FRAME):
    pager.py -w # Run in watch mode
    pager.py --tail # The most recent traces
    pager.py -t now-30s # Last 30 seconds
    pager.py -t now-24h # Last 24 hours
    pager.py -t 16:00 # Starting today 4PM
    pager.py -t 1575970000000000000 1575980000000000000 # Between 2 specific timestamps
    pager.py -t 'Jan 1 2019 10:00' '01/01/2020 10:00' # Jan 1st 2019 10:00 till Jan 1st 2020 10:00
EXAMPLES (FILTERS):
    pager.py -f @VLBA = 0x1234 # Simple
    pager.py -f @DISK_NAME = \"Disk 123\" # Quotes are needed if string contains whitespaces
    pager.py -f @DISK_NAME like *Disk_* # Wildcard
    pager.py -f has @IO_PERMS # has operator
    pager.py -f ./pager.py -f func = execute_bio # Specific function
    pager.py -f trace = __T_trace_nvmeibc_main_init # Specific trace
    pager.py -f fmt like *io_perms=* # Message format lookup
                                     # (efficient, not using string manipulations)
    pager.py -f ./pager.py -f cpu = 8 # Specific cpu
    pager.py -f ./pager.py -f file = nvmeibc_jam.c # Specific file
    pager.py -f not (func = execute_bio or has @IO_PERMS) and cpu = 8 # Complex filter
    pager.py -f @VLBA in [232 + 10] # Match vlbas in range [232..242]
    pager.py -f sev in [1, 2] # Show only Error(1) or Warning(2) severity traces
    pager.py -f '@RLBA=0x40 sticky @O_DBG_ID => [@O_DBG_ID] sticky_until fmt like "*Operation end*"'
                # Sticky example:
                # find an operation start
                # on RLBA=0x40 and find
                #  all its stages, sticking
                #  to its O_DBG_ID.
 
Please take some time to read README.md for more info.
P.S. No, seriously. Read the bloody readme.
''')

    parser.add_argument(dest='log_dirs', action='store', type=str, default=['.'], nargs='*',
                        help='''R|\
List of directories to work on.
Each directory represents the traces from a single host.
Each directory can be prefixed with 'hostname:' to specify the name of the host it came from.
If not specified, hostname is deduced by the basename of the directory.
If more than one directory was specified, each line of output is prefixed with the hostname it originated from.
''')

    parser.add_argument('--collector-dir', dest='collector_dir', action='store_true', help='Mode of running on root of log collector extracted dir')

    actions = parser.add_mutually_exclusive_group(required=False)
    actions.add_argument('--mode', dest='action', action='store', default='msg-stream-txt', choices=['msg-stream-txt', 'msg-stream-json', 'msg-stream-bin', 'msg-stream-csv-stats-only', 'ssh'],
                         help='''R|\
msg-stream-txt
    Extract, sort, merge, parse and print traces in human readable format
msg-stream-json
    Extract, sort, merge, parse and print traces in json format
msg-stream-csv-stats-only
    Extract, sort, merge, parse and print traces in minimal csv format containing time-stamp, binary-size, trace-name, etc)
msg-stream-bin'
    Extract, sort, merge and print traces in raw binary format'
ssh'
    Retrieve traces from multiple hosts via ssh and merge'
''')

    actions.add_argument('--bw-analysis', dest='action', action='store_const', const='bw-analysis',
                         help='Run bandwidth analysis using a sliding window & interval accumulator')

    timeargsgroup = parser.add_argument_group('Time arguments')

    timeargs = timeargsgroup.add_mutually_exclusive_group(required=False)

    timeargs.add_argument('-t', dest='time_frame', action=StoreOneOrTwo, type=valid_datetime,
                          default=DEFAULT_TIMEFRAME, help='''R|\
Time frame
Supported formats:
1. Timestamp in nanosechonds since epoch, i.e '12345' or '0x12345'.
2. Time interval before 'now', i.e. 'now - 1234[s/m/h]'
2. Human readable datetime, i.e. '17:00' or 'Jan 1 2013 13:00:22'. Exact list:
   {0}
'''.format((', '.join(SUPPORTED_TIME_FORMATS)).replace('%', '%%')))

    timeargsgroup.add_argument('--since', dest='since', action="store", type=str, nargs='*',
                          help='Alternative way to specify time frame start')
    
    timeargsgroup.add_argument('--until', dest='until', action="store", type=str, nargs='*',
                          help='Alternative way to specify time frame end')

    timeargs.add_argument('--tail', dest='time_frame', action="store_const", const=(valid_datetime('tail-0'), valid_datetime('18446744073709551615')),
                          help='Show the most recent buffer')

    timeargs.add_argument('-w', '--watch', dest='watch', action='store_true',
                          help='Run in watch mode')

    timeargsgroup.add_argument('-i', '--interval', dest='interval', action='store', type=float, default=2,
                               help='Watch mode interval in seconds')

    timeargsgroup.add_argument('--max_lag', type=float, default=2.3,
                          help='Maximum seconds between trace and when it is written to disk. Under 2s risks misses, but reduces dups.')

    flagsgroup = parser.add_argument_group("Flags")

    flagsgroup.add_argument('--color', dest='color', action='store_true',
                            help='Use colors for formatted output')

    flagsgroup.add_argument('--reliable', dest='reliable', action='store_true',
                            help='Do not return traces fom time frames that may have lost info for one or more CPUs. This guarantees there are no holes in the time sequence.')
    
    flagsgroup.add_argument('--nogreet', dest='nogreet', action='store_true',
                            help='Hide channel greeting messages.')

    flagsgroup.add_argument('--print-date', dest='print_date', action='store_true',
                            help='Print date in addition to the time')

    flagsgroup.add_argument('--statistics', dest='statistics', action='store_true',
                            help='Run in statistics collection mode')

    flagsgroup.add_argument('--silent', dest='silent', action='store_true',
                            help='Supress wrapper progress messages')

    channelsgroup = parser.add_argument_group("Channels")
    channelsgroup.add_argument('-l', '--log_channels', dest='channels', nargs='*',
                               action=ExtendAction, default=[], type=str,
                               help='One or more channels to dump.')
    channelsgroup.add_argument('--clnt', '--client', dest='channels',
                               action=ExtendConstAction, const=['nvmeibc_trace_long', 'nvmeibc_trace_short', 'nvmeibc_trace_eph', 'nvmeibc_trace_eter'],
                               help='Alias for all client standard channels.')
    channelsgroup.add_argument('--srv', '--server', dest='channels',
                               action=ExtendConstAction, const=['nvmeibs_trace_long', 'nvmeibs_trace_short', 'nvmeibs_trace_eph', 'nvmeibs_trace_eter'],
                               help='Alias for all server standard channels.')
    channelsgroup.add_argument('--goodpath', dest='channels',
                               action=ExtendConstAction, const=['nvmeibc_trace_goodpath', 'nvmeibs_trace_goodpath'],
                               help='Alias for client goodpath channel.')
    channelsgroup.add_argument('--metrics', dest='channels',
                               action=ExtendConstAction, const=['nvmeibc_trace_metrics', 'nvmeibs_trace_metrics', 'nvmeshum.metrics.binlog'],
                               help='Alias for client metrics channel.')
    channelsgroup.add_argument('--common', dest='channels',
                               action=ExtendConstAction, const=['nvmeibm_trace_long', 'nvmeibm_trace_eter'],
                               help='Alias for common channel.')
    channelsgroup.add_argument('--public', dest='channels',
                               action=ExtendConstAction, const=['nvmeibp_trace_long', 'nvmeibp_trace_eter'],
                               help='Alias for common channel.')
    channelsgroup.add_argument('--toma', dest='channels',
                               action=TomaShortcutAction, const=['toma.binlog', 'toma.eter.binlog'],
                               help='Alias for all toma standard channels.')
    channelsgroup.add_argument('--toma_util', dest='channels',
                               action=ExtendConstAction, const=['toma_util.binlog', 'toma_util.eter.binlog'],
                               help='Alias for all toma_util standard channels.')
    channelsgroup.add_argument('--nvmeshum', dest='channels',
                               action=ExtendConstAction, const=['nvmeshum.long.binlog', 'nvmeshum.eter.binlog', 'nvmeshum.goodpath.binlog'],
                               help='Alias for all toma standard channels.')

    channelsgroup.add_argument('--eter', dest='channels',
                               action=ExtendConstAction, const=['nvmeibc_trace_eter'],
                               help='Alias client eternal (primarily control path) channel.')
    channelsgroup.add_argument('--siw', dest='channels',
                               action=ExtendConstAction, const=['nvmeib_pipe_trace_long'],
                               help='Alias for siw traces channel.')
    channelsgroup.add_argument('--sim', '--simulator', dest='channels',
                               action=ExtendConstAction, const=['longterm.binlog', 'goodpath.binlog', 'ephemeral.binlog', 'eternal.binlog'],
                               help='Alias for all toma standard channels.')

    optionalgroup = parser.add_argument_group("Optional")
    optionalgroup.add_argument('--dict_preload', dest='dicts', nargs='*',
                               action='store', type=str, default=['none'],
                               help='List of dictionary files to preload')

    optionalgroup.add_argument('--fmtlib_preload', dest='fmtlib', nargs='?',
                               action='store', type=str,
                               help='Path to libfmtrs.so to preload')

    filtersgroup = parser.add_argument_group("Filters")

    filtersgroup.add_argument('-f', '--filter', dest='filter', action='store', nargs='*',
                              type=str, default=None,
                              help=textwrap.dedent('''R|\
Filter string to apply or "-" to read filters from stdin.
Filter syntax:
 * Expression @TOKEN = x, for example @VERSION = 15 or @NAME = "Johny".
   Will select all traces with argument with specified value.
 * Expression @COMPOSITE_TOKEN {@X = y ...}, for example @GOODPATH_IO_DUMP {@VOL_ID = 10, @START_LBA = 20}".
   Will select all composite trace that match the filter.
 * Expression func = "x", for example func = "execute_bio".
   Will select all traces invoked from the given function.
 * Expression trace = "x", for example trace = __D_trace_main.
   Will select all traces with specified id.
   Special trace id value: "SYSTEM_TRACE" - identifies pager messages, for example lost buffers detected.
 * Expression cpu = x, for example cpu = 0.
   Will select all traces from given CPU.
 * Expression func = x, for example func = execute_bio.
   Will select all traces that originate form the given function.
 * Expression fmt =~ wildcard, for example fmt =~ *error*.
   Will select all traces whose format matches the wildcard.
 * Expression has @TOKEN, for example has @VERSION.
   Will select all prints of given token regardless of its value.
 * Logical operators AND, OR, NOT.
   Used to chain logical expressions.
 * Parentheses ().
   Used to specify expression evalution order.
 * Both uppercase (TRACE) and lowercase (trace) can be used for all keywords.
   C style logical operators (&&, ||, !) can be used.
   Tabs, spaces and newlines are valid word separators.
'''))

    # Hidden options
    parser.add_argument('--test', dest='test_mode',
                        action='store_true', help=argparse.SUPPRESS)

    parser.add_argument('--dbg', dest='dbg', type=int,
                        action='store', help=argparse.SUPPRESS)

    parser.add_argument('--no-print-cpu-id', dest='no_print_cpu_id', type=int,
                        action='store', help=argparse.SUPPRESS)


    # Add the new BW analysis arguments to a new group
    bwgroup = parser.add_argument_group("Bandwidth analysis arguments")
    bwgroup.add_argument("--bw-limit", dest="bw_limit_str", type=str, default="2MB", help="Bandwidth limit in bytes/sec (e.g., 2MB)")
    bwgroup.add_argument("--window-size", type=int, default=3, help="Sliding window size in seconds")
    bwgroup.add_argument("--inactivity-period", type=int, default=1, help="Inactivity period (seconds) between intervals that allows merging")
    bwgroup.add_argument("--top", dest="top_n", type=int, default=10, help="Show top N entries in statistics (0=all)")

    return parser


def build_cmd_line(args, silent=False, remote_host=None):
    is_remote = (remote_host is not None)

    if not is_remote:
        resulting_channels = resulting_channels_list(args.log_dirs, args.channels, is_remote)
        action = args.action
        pager = PARSER_EXE
    else:
        resulting_channels = host_channels_map(args.log_dirs, args.channels)[remote_host]
        action = 'msg-stream-bin'
        pager = REMOTE_PAGER


    # For bw-analysis, we use the CSV stats only stream
    if args.action == 'bw-analysis':
        action = 'msg-stream-csv-stats-only'

    cmd = [pager, action]
    if (args.dbg):
        cmd.append('--dbg')
        cmd.append(str(args.dbg))
    cmd.extend([','.join(args.dicts), ','.join(resulting_channels)])

    cmd.append(args.time_frame[0])
    cmd.append(args.time_frame[1])

    if (args.fmtlib):
        cmd.append('--fmtlib')
        cmd.append(args.fmtlib)

    if (args.statistics):
        cmd.append('--statistics')

    if (args.filter):
        cmd.append('-f')
        cmd.append(' '.join(args.filter))

    if (args.color):
        cmd.append('--color')

    if (args.reliable):
        cmd.append('--reliable')

    if (args.nogreet):
        cmd.append('--nogreet')

    if (args.print_date):
        cmd.append('--print-date')

    if (args.no_print_cpu_id):
        cmd.append('--no-print-cpu-id')

    if not silent:
        dbgprint('\'' + '\' \''.join(cmd) + '\'')

    if args.test_mode:
        sys.exit(0)

    return cmd


def should_try_update_pager():
    # If we are run from source directory, try to update pager build
    return not BUNDLE_MODE and os.path.isfile(os.path.join(MAKE_DIR, 'pager.c'))


def run_watch_mode(parsed_args):
    start_time = parsed_args.time_frame[0]
    if start_time == DEFAULT_TIMEFRAME[0]:
        start_time = 'now-' + str(int(parsed_args.max_lag*1000))
    parsed_args.time_frame = [start_time, 'now-0']
    parsed_args.nogreet = True
    wrprint('Running pager in the watch mode')
    while True:
        cmd = build_cmd_line(parsed_args, silent=True)
        cmd_time = time.time()
        popen = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, universal_newlines=True)
        for stdout_line in iter(popen.stdout.readline, ""):
            match = re.match(r'^.\d:\d\d:\d\d\.\d* \((\d*)\)', stdout_line)
            if match:
                parsed_args.time_frame[0] = str(int(match.group(1)) + 1)
            print(stdout_line.rstrip('\n'))

        popen.stdout.close()
        return_code = popen.wait()
        if return_code and return_code != errno.ENODATA:
            raise subprocess.CalledProcessError(
                return_code, cmd)

        # New start should be last_run - max_lag, in nanos, as string
        parsed_args.time_frame[0] = str(int((cmd_time - parsed_args.max_lag) * 1000000000))
        time.sleep(parsed_args.interval)


def run_remote_mode(args):
    with tempdir() as dirpath: # This will make sure to cleanup all the fifos under tempdir
        ssh_sessions = {}
        fifos = {}
        merger = None
        try:
            # First pass: prepare files/fifos
            for host in host_channels_map(args.log_dirs, args.channels):
                #Sync all dictionaries
                hostdirs = set([os.path.dirname(ch) for ch in host_channels_map(args.log_dirs, args.channels)[host]])
                for hostdir in hostdirs:
                    subprocess.call(['scp',
                        '{0}/dict.*.json'.format(hostdir),
                        '{0}/libfmtrs.so'.format(hostdir),
                        dirpath])
                fifos[host] = os.path.join(dirpath, host + '_fifo')
                os.mkfifo(fifos[host], 0o600)
            
            # Start merger process
            cmd = [PARSER_EXE, 'deserialize-msg-stream', dirpath]
            cmd.extend(['{0}:{1}'.format(host, fifos[host]) for host in fifos])
            merger = subprocess.Popen(cmd)
            
            # And now start the ssh workers
            for host in host_channels_map(args.log_dirs, args.channels):
                # Start workers
                cmd = ['ssh', host]
                cmd.extend(["'" + x + "'" for x in build_cmd_line(args, True, host)])
                fifo = open(fifos[host], 'wb')
                ssh_sessions[host] = subprocess.Popen(cmd, stdout=fifo)
                fifo.close()
            merger.wait()
            merger = None
            for host in ssh_sessions:
                ssh_sessions[host].wait()
        finally:
            try:
                if merger:
                    merger.kill()
                for host in ssh_sessions:
                    ssh_sessions[host].kill()
            except:
                pass


def main():
    LIBFMTRS = 'libfmtrs.so'
    parser = init_argparse()
    args = parse_args(parser)
    global SILENT
    global DBG
    SILENT = args.silent or args.statistics
    DBG = args.dbg
    DEVNULL = open(os.devnull, 'w')

    if should_try_update_pager():
        dbgprint("Attempting to update pager...")
        if subprocess.call(['make', '-C', MAKE_DIR, 'pager'], stdout=DEVNULL):
            print("Warning: could not make pager")
        copyfile(os.path.join(MAKE_DIR, 'formatters', LIBFMTRS), os.path.join(MAKE_DIR, LIBFMTRS))
        if subprocess.call(['make', '-C', os.path.join(MAKE_DIR, "formatters")], stdout=DEVNULL):
            print("Warning: could not make pager formatters library")

    if args.action == 'ssh':
        return run_remote_mode(args)

    if args.action == 'bw-analysis':
        proc = subprocess.Popen(build_cmd_line(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        intervals = run_bw_analysis_mode(proc.stdout, args)
        # return a clean exit code, and do not print the intervals
        return 0

    if args.watch:
        dbgprint('Running in watch mode')
        run_watch_mode(args)
    else:
        rv = subprocess.call(build_cmd_line(args))
        if rv == errno.ENODATA:
            wrprint('No data found.' +
                    (' Try a different timeframe.' if args.time_frame != DEFAULT_TIMEFRAME else '' +
                    ' Try a different filter.' if args.filter else '')
                    )
        elif rv:
            wrprint('Terminated with error {0}'.format(rv))
        return rv
    return 0


if __name__ == "__main__":
    sys.exit(main())
