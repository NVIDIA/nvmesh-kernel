#!/usr/bin/env python3

"""
client_ndu_breakdown.py

Runs or analyzes a Non-Disruptive Update (NDU) of the NVMesh client,
providing a millisecond-precision breakdown of the NDU's distinct phases.

This script uses a "Phase-Oriented" composite design. Each "Phase" object is
responsible for finding its own start and end timestamps and can contain
child phases to create a time hierarchy.

Modes of Operation:
  1. --run:     Executes the NDU commands and then analyzes the run.
  2. --analyze: Analyzes a past NDU event from a given time window.

Object Design:
  - LogEntry (Hierarchy): "Smart" objects that parse themselves from raw JSON.
    - BaseLogEntry (ABC)
    - JournalCTLLogEntry
    - PagerLogEntry
  - LogSource (Hierarchy): "Template Method" pattern. Base class handles
    subprocess execution and pipelining.
    - BaseLogSource (ABC)
    - JournalctlLog
    - PagerLog
  - Phase (Hierarchy): "Phase-Oriented" composite design.
    - BasePhase (ABC): The "leaf" node. Finds its own interval.
    - CompositePhase: A "branch" node. Contains children and calculates its
      "self-time" (delta) by subtracting children's time from its own.
    - SimpleSystemdPhase: A "smart" composite for systemd services.
    - NDUPhase: The "root" composite, which has special rules for
      its interval (based on volumes) and completeness (our stop condition).
"""

import argparse
import subprocess
import json
import re
import time
import os
import sys
import logging
import heapq
import glob
from datetime import datetime, timezone, timedelta
from dataclasses import dataclass
from abc import ABC, abstractmethod
from typing import Generator, List, Dict, Optional, Any, Iterable

# Default window to analyze in --analyze mode if --until is omitted
DEFAULT_ANALYSIS_WINDOW_SECONDS = 300

# Setup logger. All diagnostic output goes to stderr.
# Report data will be sent to stdout via print().
logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
#  LEVEL 1: DATA CONTAINERS
# ---------------------------------------------------------------------------

@dataclass
class TimeInterval:
    """Holds a start time, end time, and calculated duration for an event."""
    begin: datetime
    end: datetime

    @property
    def duration_ms(self) -> int:
        """Calculates the duration in milliseconds."""
        if not self.begin or not self.end or self.end < self.begin:
            return 0
        delta = self.end - self.begin
        # round to the nearest millisecond.
        return int(round(delta.total_seconds() * 1000))

    def to_str(self, verbose: bool = False, pad_ms: int = 10) -> str:
        """Formats the interval as a string (e.g., '123 ms    (10:00:01.234 .. 10:00:01.357)')"""
        duration_str = f"{self.duration_ms} ms"
        # Manually pad the duration string for alignment in the summary report
        base = f"{duration_str:<{pad_ms}}"

        if verbose:
            # Format to millisecond precision, converting from UTC to local time
            begin_str = self.begin.astimezone().strftime('%H:%M:%S.%f')[:-3]
            end_str = self.end.astimezone().strftime('%H:%M:%S.%f')[:-3]
            return f"{base} ({begin_str} .. {end_str})"
        return base

# ---------------------------------------------------------------------------
#  LEVEL 2: LOG ENTRY CLASSES (Smart LogEntry Pattern)
# ---------------------------------------------------------------------------
# (No changes from original script)

class BaseLogEntry(ABC):
    """
    Abstract Base Class for all parsed log entries.
    Its __init__ calls the _parse method to populate subclass properties.
    """
    def __init__(self, timestamp: datetime, source: str, raw_data: Dict[str, Any]):
        self.timestamp: datetime = timestamp
        self.source: str = source
        # self._raw_data = raw_data # Optional: store for debugging
        self._parse(raw_data)

    @abstractmethod
    def _parse(self, data: Dict[str, Any]):
        """Subclasses implement this to parse the raw data."""
        pass

class JournalCTLLogEntry(BaseLogEntry):
    """A smart log entry that parses journalctl JSON."""
    def __init__(self, timestamp: datetime, raw_data: Dict[str, Any]):
        # Define properties before calling super()
        self.unit: Optional[str] = None
        self.message: Optional[str] = None
        self.syslog_id: Optional[str] = None
        super().__init__(timestamp, 'journalctl', raw_data)

    def _parse(self, data: Dict[str, Any]):
        """Parses the raw journalctl JSON dictionary."""
        self.unit = data.get('UNIT')
        self.message = data.get('MESSAGE')
        self.syslog_id = data.get('SYSLOG_IDENTIFIER')

class PagerLogEntry(BaseLogEntry):
    """A smart log entry that parses pager.py JSON."""
    def __init__(self, timestamp: datetime, raw_data: Dict[str, Any]):
        # Define properties before calling super()
        self.dev_name: Optional[str] = None
        self.message: Optional[str] = None
        self.func_name: Optional[str] = None
        super().__init__(timestamp, 'pager', raw_data)

    def _parse(self, data: Dict[str, Any]):
        """Parses the raw pager JSON dictionary."""
        self.dev_name = data.get('DEV_NAME')
        self.message = data.get('message') # Note: lowercase 'm'
        self.func_name = data.get('func_name')

# ---------------------------------------------------------------------------
#  LEVEL 3: LOG SOURCE CLASSES (Template Method Pattern)
# ---------------------------------------------------------------------------
# (No changes from original script)

class BaseLogSource(ABC):
    """
    Base class for log providers. Handles the subprocess execution,
    streaming (pipelining), and error handling.
    """
    def __init__(self, debug: bool = False):
        self.debug = debug

    def get_entries(self, cmd: List[str], cwd: Optional[str] = None) -> Generator[BaseLogEntry, None, None]:
        """
        Runs a command and yields parsed LogEntry objects line by line.
        """
        logger.debug(f"Running command: {' '.join(cmd)}")

        process = None
        try:
            # process is now a local variable
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding='utf-8',
                cwd=cwd
            )

            for line in process.stdout:
                try:
                    # Call the abstract method to do the parsing
                    entry = self._parse_line(line)
                    if entry:
                        yield entry
                except Exception as e:
                    # Use logging for errors
                    logger.debug(f"Skipping malformed line: {line.strip()} | Error: {e}")
                    continue

            # Read stderr after stdout is exhausted
            stderr_output, stderr_error = process.communicate()
            if stderr_output:
                # Use logging for warnings
                logger.warning(f"{cmd[0]} stderr: {stderr_output.strip()}")

        except FileNotFoundError:
            logger.error(f"Command not found: {cmd[0]}")
        except Exception as e:
            logger.error(f"Failed to run {cmd[0]}: {e}")

    @abstractmethod
    def _parse_line(self, line: str) -> Optional[BaseLogEntry]:
        """
        Parses a single line of text output from the subprocess
        and returns a specific BaseLogEntry object (or None).
        """
        pass

    @abstractmethod
    def fetch_logs(self, since: datetime, until: datetime) -> Generator[BaseLogEntry, None, None]:
        """
        Public-facing method. Subclasses will implement this
        to build their specific command and call get_entries.
        (Always requires an 'until' time).
        """
        pass


class JournalctlLog(BaseLogSource):
    """
    Implements the command-building and line-parsing for journalctl.
    """
    def __init__(self, identifiers: List[str], debug: bool = False):
        super().__init__(debug)
        self.identifiers = identifiers
        self.journal_time_format = "%Y-%m-%d %H:%M:%S.%f"

    def fetch_logs(self, since: datetime, until: datetime) -> Generator[BaseLogEntry, None, None]:
        # Formats datetimes to strings
        since_str = since.strftime(self.journal_time_format)
        until_str = until.strftime(self.journal_time_format)

        # Remove 'sudo' - user must run script with sudo if needed
        cmd = ['journalctl', '-o', 'json', '--since', since_str, '--until', until_str]

        for identifier in self.identifiers:
            cmd.append(f"SYSLOG_IDENTIFIER={identifier}")

        # Calls the *base class* method with the specific command
        yield from self.get_entries(cmd)

    def _parse_line(self, line: str) -> Optional[BaseLogEntry]:
        log_json = json.loads(line.strip()) # Can raise JSONDecodeError

        ts_microseconds = int(log_json.get('_SOURCE_REALTIME_TIMESTAMP', 0))
        if not ts_microseconds:
            ts_microseconds = int(log_json.get('__REALTIME_TIMESTAMP', 0))
        if not ts_microseconds:
            logger.warning(f"Skipping journal entry, no valid timestamp found. Line: {line.strip()}")
            return None # Skip if no timestamp

        ts = datetime.fromtimestamp(ts_microseconds / 1_000_000, tz=timezone.utc)
        return JournalCTLLogEntry(timestamp=ts, raw_data=log_json)


class PagerLog(BaseLogSource):
    """
    Implements the command-building and line-parsing for pager.py.
    """

    def __init__(self, debug: bool = False, logs_dir: Optional[str] = None, custom_pager: Optional[str] = None):
        super().__init__(debug)
        self.pager_time_format = "%Y-%m-%d %H:%M:%S"

        # Define the standard relative path once
        trace_daemon_rel_path = 'var/log/nvmesh/trace_daemon'

        if logs_dir:
            # Case 1: Offline Mode
            # Construct full path: <logs_dir>/var/log/nvmesh/trace_daemon
            base_path = os.path.join(logs_dir, trace_daemon_rel_path)

            if os.path.isdir(base_path):
                self.pager_cwd = base_path
                # Default to pager.py inside this directory (unless overridden below)
                default_pager_path = os.path.join(base_path, 'pager.py')
            else:
                # Fallback: Flat directory or non-standard structure
                logger.warning(f"Standard log structure not found at {base_path}. Using logs_dir root.")
                self.pager_cwd = logs_dir
                default_pager_path = os.path.join(logs_dir, 'pager.py')

            # Use custom pager if provided, otherwise use the default we resolved above
            self.pager_path = custom_pager if custom_pager else default_pager_path

            if self.debug:
                logger.debug(f"Using offline pager: {self.pager_path} (CWD: {self.pager_cwd})")
        else:
            # Case 2: Live System
            # Add leading slash to make it absolute: /var/log/nvmesh/trace_daemon
            self.pager_cwd = f'/{trace_daemon_rel_path}'
            self.pager_path = os.path.join(self.pager_cwd, 'pager.py')

    def fetch_logs(self, since: datetime, until: datetime) -> Generator[BaseLogEntry, None, None]:
        # Formats datetimes to strings
        since_str = since.strftime(self.pager_time_format)
        until_str = until.strftime(self.pager_time_format)

        cmd = [
            self.pager_path, '--client', '--nogreet',
            '--since', since_str, '--until', until_str,
            '--mode', 'msg-stream-json',
            '-f', 'func=__error_state_update and has @DEV_NAME'
        ]

        # Calls the *base class* method
        yield from self.get_entries(cmd, cwd=self.pager_cwd)

    def _parse_line(self, line: str) -> Optional[BaseLogEntry]:
        log_json = json.loads(line.strip()) # Can raise JSONDecodeError

        ts_nanoseconds = int(log_json.get('nanoseconds', 0))
        if not ts_nanoseconds:
            return None

        ts = datetime.fromtimestamp(ts_nanoseconds / 1_000_000_000, tz=timezone.utc)
        return PagerLogEntry(timestamp=ts, raw_data=log_json)


class JournalctlFileLog(BaseLogSource):
    """
    Parses an offline text file containing journalctl output.
    Filters logs based on the provided 'identifiers' list.
    """
    def __init__(self, logs_dir: str, identifiers: List[str], debug: bool = False):
        super().__init__(debug)
        self.logs_dir = logs_dir
        self.identifiers = identifiers  # store the allow-list
        self.log_file = self._find_journal_file(logs_dir)

        # Regex for: "Nov 23 09:28:12.421787 hostname identifier: message"
        self.line_regex = re.compile(r'^([A-Z][a-z]{2}\s+\d+\s\d{2}:\d{2}:\d{2}\.\d{6})\s+\S+\s+([^:]+):\s+(.*)$')

        # Regex to extract unit from systemd messages
        self.systemd_unit_regex = re.compile(r'(Stopping|Stopped|Starting|Started)\s+([a-zA-Z0-9@_\-\.]+service)')

    def _find_journal_file(self, logs_dir: str) -> Optional[str]:
        # Prioritize current boot log
        patterns = [
            os.path.join(logs_dir, 'journalctl_curr_boot_log*'),
            os.path.join(logs_dir, 'journalctl_prev_boot_log*'),
            os.path.join(logs_dir, 'journalctl*')
        ]
        for pattern in patterns:
            files = glob.glob(pattern)
            if files:
                return max(files, key=os.path.getsize)
        return None

    def fetch_logs(self, since: datetime, until: datetime) -> Generator[BaseLogEntry, None, None]:
        if not self.log_file:
            logger.error(f"No journalctl log file found in {self.logs_dir}")
            return

        logger.debug(f"Parsing offline journal log: {self.log_file}")

        inferred_year = since.year

        try:
            with open(self.log_file, 'r', encoding='utf-8', errors='replace') as f:
                for line in f:
                    # [OPTIMIZATION] We now receive a 'stop' signal
                    entry, should_stop = self._parse_line_context(line, since, until, inferred_year)

                    if should_stop:
                        # We passed the 'until' time, so we can stop reading the file entirely.
                        break

                    if entry:
                        yield entry

        except Exception as e:
            logger.error(f"Error parsing journal file: {e}")

    def _parse_line_context(self, line: str, since: datetime, until: datetime, year: int) -> tuple[
        Optional[BaseLogEntry], bool]:
        match = self.line_regex.match(line.strip())
        if not match:
            return None, False

        ts_str, ident_raw, msg = match.groups()

        # 1. Filter by Identifier
        # Clean "systemd[1]" -> "systemd"
        identifier = ident_raw.split('[')[0] if '[' in ident_raw else ident_raw

        # Check against the passed identifiers list
        if identifier not in self.identifiers:
            return None, False

        # 2. Parse Timestamp
        try:
            dt = datetime.strptime(ts_str, "%b %d %H:%M:%S.%f")
            dt = dt.replace(year=year).astimezone()

            if dt > until + timedelta(days=1):
                dt = dt.replace(year=year - 1)

            # 3. Time Window Filtering
            if dt < since:
                return None, False
            if dt > until:
                return None, True  # Stop reading

        except ValueError:
            return None, False

        # 4. Construct Mock JSON Data
        raw_data = {
            'SYSLOG_IDENTIFIER': identifier,
            'MESSAGE': msg,
            '__REALTIME_TIMESTAMP': int(dt.timestamp() * 1_000_000),
            'UNIT': None
        }

        #if self.debug: logger.debug(f"[_parse_line_context]: raw data is {raw_data}")

        # 5. Extract UNIT for systemd
        if identifier == 'systemd':
            unit_match = self.systemd_unit_regex.search(msg)
            if unit_match:
                raw_data['UNIT'] = unit_match.group(2)

        return JournalCTLLogEntry(timestamp=dt, raw_data=raw_data), False

    def _parse_line(self, line: str) -> Optional[BaseLogEntry]:
        # Not used directly because we utilize _parse_line_context inside fetch_logs
        return None

# ---------------------------------------------------------------------------
#  LEVEL 4: NDU PHASE CLASSES (Composite Design)
# ---------------------------------------------------------------------------

class BasePhase(ABC):
    """
    [REVISED] Abstract Base Class for a single, measurable NDU phase.
    """
    def __init__(self, name: str, debug: bool = False):
        self.name: str = name
        self.debug: bool = debug
        self._start: Optional[datetime] = None
        self._end: Optional[datetime] = None

    @abstractmethod
    def process_entry(self, entry: BaseLogEntry) -> bool:
        """
        Process a log entry.
        Returns True if the entry was consumed (matched _start or _end).
        """
        pass

    @property
    def is_complete(self) -> bool:
        """
        [NEW] Default implementation for completeness.
        Returns True if the phase has found both its start and end.
        """
        return bool(self._start and self._end)

    @property
    def interval(self) -> Optional[TimeInterval]:
        """
        Default interval calculation.
        """
        if self._start and self._end and self._end >= self._start:
            return TimeInterval(self._start, self._end)
        elif self.debug and self._start and self._end:
             logger.debug(f"Phase '{self.name}' has invalid interval. Start: {self._start}, End: {self._end}")
        return None

    def get_report_data(self) -> Dict[str, Any]:
        """
        [NEW] Recursively builds a dictionary of timing data.
        For a BasePhase (a leaf), self_time is the same as duration.
        """
        my_interval = self.interval
        my_duration = my_interval.duration_ms if my_interval else 0

        return {
            "name": self.name,
            "duration_ms": my_duration,
            "self_time_ms": my_duration,  # For a leaf, self_time IS the duration
            "interval": my_interval,
            "children": [] # BasePhase has no children
        }

# ---

class CompositePhase(BasePhase):
    """
    [NEW] A BasePhase that can contain a collection of child BasePhase objects.
    Its own interval/completeness is independent of its children.
    """
    def __init__(self, name: str, debug: bool = False):
        super().__init__(name, debug)
        self.children: Dict[str, BasePhase] = {}

    def add_child(self, child_phase: BasePhase):
        """Adds a child phase to be processed."""
        if child_phase.name in self.children:
            logger.warning(f"Duplicate child phase name: {child_phase.name}")
        self.children[child_phase.name] = child_phase

    def process_entry(self, entry: BaseLogEntry) -> bool:
        """
        [NEW] Fans out the log entry to all children.
        The parent phase (e.g., SimpleSystemdPhase) is responsible for
        calling this *and* processing the entry for its *own* _start/_end.
        """
        consumed_by_child = False
        for child in self.children.values():
            # Only send to children that are not yet complete
            if not child.is_complete:
                if child.process_entry(entry):
                    consumed_by_child = True

        # Returns True if any child consumed the log.
        return consumed_by_child

    # --- NO is_complete override ---
    # composite will use the BasePhase default (bool(self._start and self._end))
    # as some child phases (e.g. module unload (client stop) or module load (client start)) may not be logged

    # --- NO interval override ---
    # composite uses its own _start and _end, as the interval sum of child phases may not fully span the parent
    # The *one exception* will be NDUPhase, which *will* override this to be the min/max of its volume children

    def get_report_data(self) -> Dict[str, Any]:
        """
        [NEW & OVERRIDDEN] Recursively builds a dictionary, calculating
        its 'self_time' (overhead) by subtracting children's time.
        """
        my_interval = self.interval
        my_duration = my_interval.duration_ms if my_interval else 0

        child_reports = []
        children_total_duration = 0

        for child in self.children.values():
            child_report = child.get_report_data()
            children_total_duration += child_report["duration_ms"]
            child_reports.append(child_report)

        # This is the "overhead" or "delta" calculation for this level
        self_time = my_duration - children_total_duration

        return {
            "name": self.name,
            "duration_ms": my_duration,
            "self_time_ms": max(0, self_time), # Self-time can't be negative
            "interval": my_interval,
            "children": child_reports
        }

    def get_all_child_phases(self) -> List[BasePhase]:
        """A helper to return all child phases for verbose reporting."""
        return sorted(self.children.values(), key=lambda p: p.name)

# ---

# Inherits from CompositePhase
class SimpleSystemdPhase(CompositePhase):
    """
    A reusable phase defined by systemd messages.
    It can now also contain child phases to break down its own interval.
    """
    def __init__(self, name: str, service_name: str, start_msg_prefix: str, end_msg_prefix: str, debug: bool = False):
        super().__init__(name, debug) # This initializes self.children = {}
        self.service_name = service_name
        self.start_msg_prefix = start_msg_prefix
        self.end_msg_prefix = end_msg_prefix

    def process_entry(self, entry: BaseLogEntry) -> bool:
        """
        [REVISED] Processes for self AND fans out to children.
        """
        # 1. Call the base class process_entry to fan out to all children
        #    (e.g., to the "Module Load" sub-phase)
        consumed_by_child = super().process_entry(entry)

        # 2. Do its *own* work to find its start/end boundaries
        consumed_by_self = False
        if not self.is_complete: # Only check if phase isn't already done
            if isinstance(entry, JournalCTLLogEntry) and \
               entry.syslog_id == 'systemd' and \
               entry.unit == self.service_name and \
               entry.message:

                if not self._start and entry.message.startswith(self.start_msg_prefix):
                    if self.debug: logger.debug(f"[{self.name}]: Matched start log: {entry.message.strip()}")
                    self._start = entry.timestamp
                    consumed_by_self = True
                elif not self._end and entry.message.startswith(self.end_msg_prefix):
                    if self.debug: logger.debug(f"[{self.name}]: Matched stop log: {entry.message.strip()}")
                    self._end = entry.timestamp
                    consumed_by_self = True

        return consumed_by_self or consumed_by_child
class ClientStopPhase(SimpleSystemdPhase):
    """Tracks the 'client stop' phase (Stopping... to Stopped...)."""
    def __init__(self, debug: bool = False):
        super().__init__("client stop", "nvmeshclient.service", "Stopping", "Stopped", debug)
        # Add a child phase for module unloading
        self.add_child(ShutdownPhase(debug=debug))
        self.add_child(ModuleUnLoadPhase(debug=debug))

class ClientStartPhase(SimpleSystemdPhase):
    """Tracks the 'client start' phase (Starting... to Started...)."""
    def __init__(self, debug: bool = False):
        super().__init__("client start", "nvmeshclient.service", "Starting", "Started", debug)
        # Add a child phase for module loading
        self.add_child(ModuleLoadPhase(debug=debug))

class DependencyRestartPhase(CompositePhase):
    """
    Tracks the full restart of parallel services (CM and TD).
    Interval is from the *first* service stopping to the *last* service started.
    """
    def __init__(self, debug: bool = False):
        super().__init__("CM/TD restart", debug)
        # We need to find all four of these timestamps
        self._cm_stop_t: Optional[datetime] = None
        self._cm_start_t: Optional[datetime] = None
        self._td_stop_t: Optional[datetime] = None
        self._td_start_t: Optional[datetime] = None

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # Fan out to children (e..g, "NVMesh CM Stop")
        consumed_by_child = super().process_entry(entry)

        consumed_by_self = False
        if not isinstance(entry, JournalCTLLogEntry):
            return consumed_by_child

        if entry.syslog_id == 'systemd':
            if entry.unit == 'nvmeshcm.service':
                if entry.message and entry.message.startswith('Stopping') and not self._cm_stop_t:
                    self._cm_stop_t = entry.timestamp
                    consumed_by_self = True
                elif entry.message and entry.message.startswith('Started') and not self._cm_start_t:
                    self._cm_start_t = entry.timestamp
                    consumed_by_self = True
            elif entry.unit == 'nvmeshtrace@trace_daemon.service':
                if entry.message and entry.message.startswith('Stopping') and not self._td_stop_t:
                    self._td_stop_t = entry.timestamp
                    consumed_by_self = True
                elif entry.message and entry.message.startswith('Started') and not self._td_start_t:
                    self._td_start_t = entry.timestamp
                    consumed_by_self = True

        return consumed_by_self or consumed_by_child

    @property
    def interval(self) -> Optional[TimeInterval]:
        """Custom interval calculation for this complex phase."""
        all_timestamps_found = all([self._cm_stop_t, self._cm_start_t, self._td_stop_t, self._td_start_t])

        if all_timestamps_found:
            self._start = min(self._cm_stop_t, self._td_stop_t)
            self._end = max(self._cm_start_t, self._td_start_t)
            if self._end >= self._start:
                return TimeInterval(self._start, self._end)
        elif self.debug and (self._cm_stop_t or self._cm_start_t or self._td_stop_t or self._td_start_t):
            logger.debug(f"Phase '{self.name}' is incomplete. CM_Stop:{bool(self._cm_stop_t)}, CM_Start:{bool(self._cm_start_t)}, TD_Stop:{bool(self._td_stop_t)}, TD_Start:{bool(self._td_start_t)}")
        return None

    @property
    def is_complete(self) -> bool:
        """
        This composite phase is complete when its 4 markers are found.
        We do NOT check children, as per your last request.
        """
        self_complete = all([self._cm_stop_t, self._cm_start_t, self._td_stop_t, self._td_start_t])
        return self_complete

class ShutdownPhase(BasePhase):
    """Tracks the duration of the nvmesh_clnt_shutdown script."""
    def __init__(self, debug: bool = False):
        super().__init__("client shutdown", debug)

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if self.is_complete or not isinstance(entry, JournalCTLLogEntry):
            return False

        msg = entry.message
        if entry.syslog_id == 'nvmeshclient' and msg and "NDU" in msg:
            if not self._start and "client shutdown script invoked" in msg:
                self._start = entry.timestamp
                return True
            if not self._end and "client shutdown script done" in msg:
                self._end = entry.timestamp
                return True
        return False

class ModuleUnLoadPhase(BasePhase):
    """A child-phase that finds the "Starting to unload" and "Done unloading" messages from the nvmeshclient log."""
    def __init__(self, debug: bool = False):
        super().__init__("module unload", debug)

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if self.is_complete or not isinstance(entry, JournalCTLLogEntry):
            return False
        msg = entry.message
        # Look for the log lines from the nvmeshclient script
        if entry.syslog_id == 'nvmeshclient' and msg and "NDU" in msg:
            if not self._start and "Starting to unload modules" in msg:
                self._start = entry.timestamp
                return True
            elif not self._end and "Done unloading modules" in msg:
                self._end = entry.timestamp
                return True
        return False

class ModuleLoadPhase(BasePhase):
    """A child-phase that finds the "Starting to load" and "Done loading" messages from the nvmeshclient log."""
    def __init__(self, debug: bool = False):
        super().__init__("module load", debug)

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if self.is_complete or not isinstance(entry, JournalCTLLogEntry):
            return False
        msg = entry.message
        # Look for the log lines from the nvmeshclient script
        if entry.syslog_id == 'nvmeshclient' and msg and "NDU" in msg:
            if not self._start and "Starting to load modules" in msg:
                self._start = entry.timestamp
                return True
            elif not self._end and "Done loading modules" in msg:
                self._end = entry.timestamp
                return True
        return False

class VolumePhase(BasePhase):
    """
    A phase tracking the IO disabled interval for a *single* volume.
    Inherits from BasePhase, using _start for 'Disabling' and _end for 'Enabling.
    """
    def __init__(self, name: str, debug: bool = False):
        # The 'name' will be the volume's device name (e.g., 'v1')
        super().__init__(name, debug)

    def process_entry(self, entry: BaseLogEntry) -> bool:
        """Processes a PagerLogEntry to find this volume *own* start (disabled) and end (enabled)."""
        if self.is_complete or not isinstance(entry, PagerLogEntry):
            return False

        if entry.dev_name != self.name:
            return False

        consumed = False
        if not self._start and entry.message and 'Disabling I/O' in entry.message:
            logger.debug(f"[pager]: Found 'Disabling I/O' for {self.name}")
            self._start = entry.timestamp
            consumed = True
        elif not self._end and entry.message and 'Enabling I/O' in entry.message:
            logger.debug(f"[pager]: Found 'Enabling I/O' for {self.name}")
            self._end = entry.timestamp
            consumed = True

        return consumed


# [REVISED] Renamed from IODisabledPhase, inherits from CompositePhase
class NDUPhase(CompositePhase):
    """
    A special composite phase that tracks the *entire* NDU process.
    Its interval is defined by the min/max of all discovered volumes.
    Its completeness is *only* defined by all volumes being complete.
    """
    def __init__(self, debug: bool = False):
        super().__init__('NDU', debug)
        # self.children will hold *service* phases (Stop, Start, etc.)
        self.volumes: Dict[str, VolumePhase] = {} # Tracks *volume* sub-phases

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # 1. Fan out to all *service* children (Stop, Start, etc.)
        consumed_by_service_child = super().process_entry(entry)

        # 2. Do its *own* work (managing volumes)
        consumed_by_volume = False
        if isinstance(entry, PagerLogEntry):
            vol_name = entry.dev_name
            if vol_name:
                if vol_name not in self.volumes:
                    self.volumes[vol_name] = VolumePhase(name=vol_name, debug=self.debug)
                    logger.debug(f"[NDUPhase]: Discovered new volume from logs: {vol_name}")

                # Delegate to the specific volume
                # Only send if the volume phase is not yet complete
                if not self.volumes[vol_name].is_complete:
                    if self.volumes[vol_name].process_entry(entry):
                        consumed_by_volume = True

        return consumed_by_service_child or consumed_by_volume

    @property
    def is_complete(self) -> bool:
        """
        The NDU is "complete" (for polling) only when:
        1. All dynamically discovered volumes are complete (I/O re-enabled).
        2. All main service phases are complete (e.g., Client Start is "Started").
        """
        # We can't be complete if we haven't even found the volumes yet.
        if not self.volumes:
            return False
        volumes_complete = all(v.is_complete for v in self.volumes.values())

        # we can't be complete if any of the service phases are still running
        children_complete = all(c.is_complete for c in self.children.values())

        return volumes_complete and children_complete

    @property
    def interval(self) -> Optional[TimeInterval]:
        """
        [OVERRIDDEN] The NDU "IO disabled" interval is the
        min/max window of all its volume children.
        """
        valid_intervals = [v.interval for v in self.volumes.values() if v.interval is not None]
        if not valid_intervals:
            return None

        # Find the earliest start time and latest end time across all volumes
        self._start = min(i.begin for i in valid_intervals)
        self._end = max(i.end for i in valid_intervals)

        return TimeInterval(self._start, self._end)

    def get_report_data(self) -> Dict[str, Any]:
        """
        [OVERRIDDEN] Builds the final report dictionary.
        This calculates the two main timelines and the final NDU time.
        """
        # 1. Get the data for the service phases (Stop, Start, etc.)
        service_children_reports = []
        service_phases_sum_ms = 0

        # self.children holds service phases
        for child in self.children.values():
            child_report = child.get_report_data()
            service_phases_sum_ms += child_report["duration_ms"]
            service_children_reports.append(child_report)

        # 2. Get the data for the volume phases
        volume_children_reports = []
        io_interval = self.interval # This calculates the min/max volume interval
        io_duration_ms = io_interval.duration_ms if io_interval else 0

        for vol_phase in self.volumes.values():
            volume_children_reports.append(vol_phase.get_report_data())

        # 3. Calculate final NDU Time (as per your logic)
        # we consider the final ndu time the IO disabled duration if available to us
        final_ndu_time = io_duration_ms if io_duration_ms else service_phases_sum_ms

        # Build the final report structure
        return {
            "name": self.name,
            "total_ndu_time_ms": final_ndu_time,
            "service_phases_sum_ms": service_phases_sum_ms,
            "io_disabled_duration_ms": io_duration_ms,
            "io_disabled_interval": io_interval,
            "service_phases": service_children_reports,
            "volume_phases": sorted(volume_children_reports, key=lambda x: x.get("name")),
            # BasePhase properties (for consistency)
            "duration_ms": io_duration_ms,
            "self_time_ms": 0, # NDUPhase itself has no "self time"
            "interval": io_interval,
            "children": service_children_reports # for consistency
        }

# ---------------------------------------------------------------------------
#  MAIN APPLICATION LOGIC
# ---------------------------------------------------------------------------

def run_ndu_commands():
    """Executes the NDU commands. Assumes script is run with sudo."""
    logger.info("Running NDU commands...")
    commands = [
        ['/opt/nvmesh/bin/nvmesh_client_upgrade'],
    ]
    for cmd in commands:
        logger.info(f"Executing '{' '.join(cmd)}'...")
        try:
            # We assume the script is run with sudo if it needs to be
            subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=120) # Added timeout
        except FileNotFoundError:
            logger.error(f"Command not found: {cmd[0]}")
            sys.exit(1)
        except subprocess.TimeoutExpired:
            logger.error(f"Command timed out: {' '.join(cmd)}")
            sys.exit(1)
        except subprocess.CalledProcessError as e:
            logger.error(f"Command failed: {' '.join(cmd)}")
            logger.error(f"  STDOUT: {e.stdout.strip()}")
            logger.error(f"  STDERR: {e.stderr.strip()}")
            sys.exit(1)
    logger.info("NDU commands completed.")

def setup_ndu_phases(args: argparse.Namespace) -> NDUPhase:
    """Creates and returns the root NDUPhase object with all phases."""

    # 1. Create the root NDU object
    ndu_phase = NDUPhase(debug=args.debug)

    # 2. Create and add the main service phases as children
    ndu_phase.add_child(ClientStopPhase(debug=args.debug)) # ClientStop is now a composite

    dep_phase = DependencyRestartPhase(debug=args.debug)
    ndu_phase.add_child(dep_phase)

    ndu_phase.add_child(ClientStartPhase(debug=args.debug))

    # 3. Add verbose-only sub-phases
    if args.verbose:
        # Add children to the dependency phase
        dep_phase.add_child(SimpleSystemdPhase("NVMesh CM Stop", "nvmeshcm.service", "Stopping", "Stopped", debug=args.debug))
        dep_phase.add_child(SimpleSystemdPhase("NVMesh CM Start", "nvmeshcm.service", "Starting", "Started", debug=args.debug))
        dep_phase.add_child(SimpleSystemdPhase("NVMesh TD Stop", "nvmeshtrace@trace_daemon.service", "Stopping", "Stopped", debug=args.debug))
        dep_phase.add_child(SimpleSystemdPhase("NVMesh TD Start", "nvmeshtrace@trace_daemon.service", "Starting", "Started", debug=args.debug))

    return ndu_phase

def combined_log_stream(
    log_sources: List[BaseLogSource],
    since_dt: datetime,
    until_dt: datetime
) -> Generator[BaseLogEntry, None, None]:
    """
    [NEW] Fetches logs from all sources and merges them into a single,
    chronologically sorted stream using heapq.merge.
    """

    def safe_gen(source: BaseLogSource, since: datetime, until: datetime) -> Generator[BaseLogEntry, None, None]:
        """Wraps a generator to catch and log exceptions without crashing the merge."""
        try:
            yield from source.fetch_logs(since, until)
        except subprocess.CalledProcessError as e:
            # This can happen if pager.py fails, etc.
            logger.error(f"Log source command failed: {e.stderr}")
        except Exception as e:
            logger.error(f"Error in log stream generator {type(source).__name__}: {e}")

    # Create a list of generators (iterators)
    generators = []
    for source in log_sources:
        logger.debug(f"Fetching historical logs from {type(source).__name__}...")
        generators.append(safe_gen(source, since_dt, until_dt))

    # Merge the k sorted streams into one sorted stream
    # The 'key' ensures heapq compares the 'timestamp' attribute of all entry objects
    merged_stream = heapq.merge(*generators, key=lambda entry: entry.timestamp)

    yield from merged_stream

def print_report_hierarchical(report_data: Dict[str, Any], verbose: bool, level: int = 0):
    """
    [NEW] Recursively prints the hierarchical report.
    """
    indent = "  " * level

    # --- 1. Print Self ---
    name = report_data.get("name", "Unknown")
    duration = report_data.get("duration_ms", 0)
    self_time = report_data.get("self_time_ms", 0)
    interval = report_data.get("interval")

    # Don't print 0 duration for base phases, but do print 0 self_time for composites
    if duration == 0 and not report_data.get("children"):
         duration_str = "N/A"
         # Don't show interval if duration is 0
         interval_str = ""
    else:
        duration_str = f"{duration} ms"
        # Format the interval string (if verbose)
        interval_str = ""
        if verbose and interval:
            begin_str = interval.begin.astimezone().strftime('%H:%M:%S.%f')[:-3]
            end_str = interval.end.astimezone().strftime('%H:%M:%S.%f')[:-3]
            interval_str = f"({begin_str} .. {end_str})"

    # Print the line for the current phase
    label = f"{indent}- {name}:"

    # For composites, show self-time (delta). For leaves, don't.
    if report_data.get("children"):
        delta_str = f"(Self: {self_time} ms)"
        print(f"{label:<26} {duration_str:<10} {delta_str:<18} {interval_str}")
    else:
        print(f"{label:<26} {duration_str:<10} {'':<18} {interval_str}")

    # --- 2. Recurse for Children ---
    # Sort children by their interval start time, if available
    child_reports = report_data.get("children", [])
    try:
        sorted_children = sorted(child_reports, key=lambda r: r.get("interval").begin if r.get("interval") else datetime.max.replace(tzinfo=timezone.utc))
    except Exception:
        sorted_children = child_reports # Fallback if sorting fails

    for child_report in sorted_children:
        print_report_hierarchical(child_report, verbose, level + 1)


def print_report(analysis: NDUPhase, verbose: bool):
    """
    [REVISED] Main print function that orchestrates the hierarchical report.
    """

    # Get the single, consolidated data structure
    report_data = analysis.get_report_data()

    print("\n" + "="*30)
    print("  NVMesh NDU Analysis Report")
    print("="*30 + "\n")
    print("--- NDU Phase Breakdown ---")

    # --- Print Hierarchical Service Phases ---
    service_phases = report_data.get("service_phases", [])
    for phase_data in service_phases:
        print_report_hierarchical(phase_data, verbose, level=0)

    print("---------------------------")

    # --- Print Totals (from NDUPhase report) ---
    service_sum_str = f"{report_data['service_phases_sum_ms']} ms"
    print(f"{'total NDU services:':<22} {service_sum_str}")

    io_interval = report_data["io_disabled_interval"]
    io_disabled_str = io_interval.to_str(verbose) if io_interval else "N/A"
    print(f"{'IO disabled:':<22} {io_disabled_str}")

    print("---------------------------")
    ndu_time_str = f"{report_data['total_ndu_time_ms']} ms"
    print(f"{'Total NDU Time:':<22} {ndu_time_str}")
    print("---------------------------\n")

    # --- Print Verbose Volume Details ---
    if verbose:
        print("--- Verbose Volume Details (IO Disabled) ---")
        volume_phases = report_data.get("volume_phases", [])
        if not volume_phases:
            print("    (no volumes discovered)")
        for vol_data in volume_phases:
            # Use the simple recursive printer for volumes too
            print_report_hierarchical(vol_data, verbose, level=1)


def init_argparse() -> argparse.ArgumentParser:
    """
    Initializes and returns the argument parser.
    """
    parser = argparse.ArgumentParser(
        description="Run or analyze an NVMesh Client NDU.",
        formatter_class=argparse.RawTextHelpFormatter
    )

    # Add subparsers for the 'run' and 'analyze' commands
    subparsers = parser.add_subparsers(dest='command', required=True,
                                       title = 'commands',
                                       description = 'The main operation to perform.')
    # --- 'run' command parser ---
    run_parser = subparsers.add_parser('run', help='Execute the NDU commands and then analyze.')
    run_parser.set_defaults(func=handle_run_command)

    # --- 'analyze' command parser ---
    analyze_parser = subparsers.add_parser('analyze', help='Analyze a past run from a given time window.')
    analyze_parser.add_argument('--since', required=True, help='Start timestamp (e.g., "HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS").')
    analyze_parser.add_argument('--until', help=f'Optional end timestamp. If not provided, defaults to {DEFAULT_ANALYSIS_WINDOW_SECONDS} seconds after --since.')

    # offline analysis Arguments
    analyze_parser.add_argument('--logs-dir', help='Path to collected logs directory (activates offline mode).')
    analyze_parser.add_argument('--pager', help='Explicit path to pager executable (overrides default).')

    analyze_parser.set_defaults(func=handle_analyze_command)

    # --- Global arguments for all commands ---
    parser.add_argument('--verbose', '-v', action='store_true', help='Print full begin/end timestamps and sub-phases.')
    parser.add_argument('--debug', action='store_true', help='Print debug info (sent to stderr).')
    return parser

def parse_flexible_timestamp(timestamp_str: str, arg_name: str = "timestamp") -> datetime:
    """
    Parses a timestamp string that is either full ISO or just HH:MM:SS (today).
    Exits the script on a parse failure.
    """
    try:
        # Try full ISO format first (YYYY-MM-DDTHH:MM:SS)
        dt = datetime.fromisoformat(timestamp_str).astimezone()
        return dt
    except ValueError:
        # Try HH:MM:SS format (assumes today's date)
        try:
            today_str = datetime.now().strftime('%Y-%m-%d')
            dt = datetime.fromisoformat(f"{today_str}T{timestamp_str}").astimezone()
            return dt
        except Exception:
            logger.error(f"Could not parse --{arg_name} timestamp: {timestamp_str}")
            logger.error("  Please use 'HH:MM:SS' or 'YYYY-MM-DDTHH:MM:SS'")
            sys.exit(1)

def handle_analyze_command(args: argparse.Namespace) -> tuple[datetime, datetime, Optional[str], Optional[str]]:
    """
    Called by argparse if the 'analyze' command is used.
    Validates and parses the --since and --until timestamps.
    Returns (since_dt, until_dt, logs_dir, pager_path).
    """
    # 1. Parse --since (required=True by the parser)
    since_dt = parse_flexible_timestamp(args.since, arg_name="since")

    # 2. Calculate or parse --until
    if not args.until:
        until_dt = since_dt + timedelta(seconds=DEFAULT_ANALYSIS_WINDOW_SECONDS)
    else:
        until_dt = parse_flexible_timestamp(args.until, arg_name="until")

    # 3. Extract offline args
    logs_dir = args.logs_dir
    pager_path = args.pager

    return since_dt, until_dt, logs_dir, pager_path

def handle_run_command(args: argparse.Namespace) -> tuple[None, None, None, None]:
    """
    Called by argparse if the 'run' command is used.
    Timestamps will be generated later in main().
    """
    # For 'run' mode, timestamps are determined *after*
    # the NDU commands execute, so we return None here.
    return None, None, None, None

def parse_args_and_setup_logging(parser: argparse.ArgumentParser) -> tuple[argparse.Namespace, Optional[datetime], Optional[datetime], Optional[str], Optional[str]]:
    """
    Parses args, sets up logging, and calls the command-specific handler.
    Returns args, since, until, logs_dir, pager_path.
    """
    args = parser.parse_args()

    # --- Setup Logging ---
    # Use force=True to override any default config and ensure --debug level is set
    log_level = logging.DEBUG if args.debug else logging.INFO
    logging.basicConfig(level=log_level, format='%(levelname)s: %(message)s', stream=sys.stderr, force=True)

    # --- Argument Validation & Time Setup ---
    # Call the function that argparse stored in 'args.func'.
    since_dt, until_dt, logs_dir, pager_path = args.func(args)

    return args, since_dt, until_dt, logs_dir, pager_path

def execute_ndu_and_get_times(poll_timeout_seconds: int) -> tuple[datetime, datetime]:
    """
    Executes the NDU commands and returns the start time of the run
    and the calculated 'until' time for the historical log pass.
    """
    start_time = datetime.now().astimezone()  # Get local time
    run_ndu_commands()
    end_time = datetime.now().astimezone()

    since_dt = start_time
    # In --run mode, we set a wide 'until' for the historical pass
    until_dt = end_time + timedelta(seconds=poll_timeout_seconds)

    return since_dt, until_dt

def process_log_stream(
        log_stream: Generator[BaseLogEntry, None, None],
        ndu_analysis: NDUPhase,
        stop_on_complete: bool = False
) -> bool:
    """
    Feeds all entries from a given log stream into the analysis object.
    Returns True if the NDUPhase became complete during this stream.
    :param log_stream: The generator of log entries to process.
    :param ndu_analysis: The main NDU analysis object.
    :param stop_on_complete: If True, stop processing as soon as NDU is complete
                             (used for the historical/analyze optimization).
    """
    try:
        for entry in log_stream:
            # Feed every log entry to the root of the tree
            ndu_analysis.process_entry(entry)

            # Check if the *main* stop condition (all volumes + services) has been met
            if stop_on_complete and ndu_analysis.is_complete:
                logger.info("NDU complete. Stopping log processing.")
                return True  # NDU is complete

    except KeyboardInterrupt:
        logger.info("\nLog processing interrupted.")
        # Re-raise to stop any outer loops (like the polling loop)
        raise

    # Return the final completeness status after the stream is exhausted
    return ndu_analysis.is_complete

def main():
    parser = init_argparse()
    # Unpack the new variables
    args, since_dt, until_dt, logs_dir, pager_path = parse_args_and_setup_logging(parser)

    # --- This is the new "root" object of the hierarchy ---
    ndu_analysis = setup_ndu_phases(args)

    # These are the only identifiers we care about for the analysis
    target_identifiers = ['systemd', 'nvmeshclient']
    # --- Get Log Sources ---
    if logs_dir:
        logger.info(f"Using offline mode with logs directory: {logs_dir}")
        # Use the file parser for journalctl
        journal_source = JournalctlFileLog(
            logs_dir=logs_dir,
            identifiers=target_identifiers,
            debug=args.debug
        )
    else:
        # Use the live command line parser
        journal_source = JournalctlLog(
            identifiers=target_identifiers,
            debug=args.debug
        )

    # Pass the offline args to PagerLog
    pager_source = PagerLog(
        debug=args.debug,
        logs_dir=logs_dir,
        custom_pager=pager_path
    )

    log_sources = [journal_source, pager_source]

    # --- Run/Analyze Logic ---
    poll_timeout_seconds = 30  # For --run mode polling
    journal_time_format = "%Y-%m-%d %H:%M:%S.%f"

    if args.command == 'run':
        # Call the new function to get the timestamps
        since_dt, until_dt = execute_ndu_and_get_times(poll_timeout_seconds)

    logger.info(
        f"Analyzing logs from {since_dt.strftime(journal_time_format)} to {until_dt.strftime(journal_time_format)}...")

    # --- Processing Phase ---
    logger.debug("Starting combined historical log processing...")

    try:
        # 1. Run the combined historical stream
        log_stream = combined_log_stream(log_sources, since_dt, until_dt)

        # For the historical pass, we *always* stop if the NDU is complete
        process_log_stream(log_stream, ndu_analysis, stop_on_complete=True)

        logger.debug("Historical log processing finished.")

        # 2. Conditional Polling
        # This block is *only* entered in --run mode, and *only* if the
        # historical pass didn't find all the "I/O Enabled" events.
        if args.command == 'run' and not ndu_analysis.is_complete:
            logger.info("Starting polling for I/O re-enable events...")
            poll_start_time = datetime.now()
            poll_interval_seconds = 0.5
            # Start polling from the *original* start time, this is safer to catch any events we might have missed.
            poll_since_dt = since_dt

            while not ndu_analysis.is_complete:
                if (datetime.now() - poll_start_time).total_seconds() > poll_timeout_seconds:
                    logger.error(f"Timed out after {poll_timeout_seconds}s waiting for I/O re-enable events.")
                    break

                poll_until_dt = datetime.now().astimezone()

                # Create the stream for *this* poll iteration
                poll_stream = pager_source.fetch_logs(poll_since_dt, poll_until_dt)

                # Process this small stream.
                process_log_stream(poll_stream, ndu_analysis, stop_on_complete=False)

                time.sleep(poll_interval_seconds)

    except KeyboardInterrupt:
        # This will catch the 'raise' from process_log_stream
        logger.info("\nMain processing loop interrupted.")

    # --- Reporting Phase ---
    print_report(ndu_analysis, args.verbose)

if __name__ == "__main__":
    # DO NOT set a default basicConfig here.
    # main() will set it after args are parsed.
    main()
