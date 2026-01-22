#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""
nvmesh_client_upgrade_breakdown.py

A performance analysis tool for NVMesh Client Non-Disruptive Updates (NDU).
It parses system logs to construct a precise, millisecond-level timeline of the
upgrade process, distinguishing between Service Control Plane (systemd/scripts)
and Data Plane (IO connectivity) latency.

modes:
  1. run:     Executes the NDU commands live and monitors the process.
  2. analyze: Parses past NDU events from system logs. Supports both live
              system journal/pager and offline logs via --logs-dir.

ARCHITECTURE & DESIGN PATTERNS:

1. Phase-Oriented Composite Design
   The core abstraction is a 'Phase'. Phases form a hierarchy (Tree Structure).
   - BasePhase (Leaf):
     Tracks a single event with specific Start/End signatures. Validates its own
     completeness and reports warnings if logs are missing.
   - CompositePhase (Branch):
     Contains child phases. Recursively calculates total duration and, crucially,
     "Self-Time" (Overhead) by subtracting children's duration from its own.
   - ContainerPhase (Grouping):
     A specialized CompositePhase that acts purely as a logical container. It does
     not have its own log events; its interval and completeness are determined
     entirely by the aggregate state of its children. (Used for Dynamic Volumes,
     Parallel Service Restarts, etc.)
   - NDUPhase (Root):
     The coordinator. It determines the final NDU duration by comparing the
     Service Timeline vs. the Data (IO) Timeline.

2. Dynamic Volume Discovery
   Classes inheriting from 'DynamicVolumesPhase' (e.g., VolumesAttachPhase,
   VolumesDetachPhase) parse logs to dynamically discover which volumes were
   present. They act as factories, creating specific child phases for every
   discovered device.

3. Derived Logical Phases (The Data Plane)
   The 'VolumesIODisabledPhase' is a "Passive" phase. It does not parse logs directly.
   Instead, it derives the exact IO-down window for each volume by linking data
   from the Detach and Attach trees.
   - Per Volume: Defined as the span from "Disabling I/O" (Start of Detach phase)
     to "Enabling I/O" (End of Attach phase).
   - Aggregate NDU: Defined as min("Disabling I/O") .. max("Enabling I/O") across
     all volumes.

4. Execution Lifecycle (Four-Step Process)
   - Step 1: Static Tree Construction
     The tool initializes the static skeleton of the phase hierarchy (NDUPhase ->
     ClientStop/Start -> ModulesLoad, etc.) before processing any logs. This
     defines the known structure of the NDU process.

   - Step 2: Stream Processing & Dynamic Population
     A single pass over merged, chronological logs (Journal + Pager). During this
     phase, log entries are fed into the tree. This sets start/end timestamps for
     static phases and dynamically creates new child phases (e.g., specific Volume
     instances) as they are discovered in the logs.

   - Step 3: Finalization (Recursive Freeze)
     Once the stream ends (or NDU is complete), a recursive pass traverses the
     tree to freeze the state. It calculates final intervals, aggregates durations
     from children (for ContainerPhases), validates logical consistency (e.g.,
     Start < End), and determines the final status (Valid/Incomplete/Empty).

   - Step 4: Reporting
     The fully populated and validated tree is traversed one last time to generate
     the hierarchical text report, printing durations, self-times, and any
     warnings collected during finalization.

CLASS GROUPS:

  * Infrastructure:
    - LogEntry (Hierarchy): "Smart" objects that parse themselves from raw JSON.
      - BaseLogEntry (ABC)
      - JournalCTLLogEntry
      - PagerLogEntry
    - LogSource (Hierarchy): "Template Method" pattern. Base class handles
      subprocess execution and pipelining.
      - BaseLogSource (ABC)
      - JournalctlLog (Live JSON output)
      - PagerLog (Live or Offline pager.py)
      - JournalctlFileLog (Parses offline text files for offline analysis)

  * Service Phases (Control Plane):
    - ClientStopPhase: Includes Shutdown script, Module Unload, Volume Detach.
    - DependencyRestartPhase: Tracks concurrent CM/TD service restarts.
    - ClientStartPhase: Includes Module Load, Volumes Attach.

  * Module Loading Breakdown:
    - Tracks Pre-Init (User space/Linking), Core Init (Kernel), and Finalize.

  * Volume State Phases (Data Plane):
    - VolumeDetachPhase: Disabling I/O -> Detach Finished/Failed.
    - VolumeAttachPhase: Attach Finished -> CONT -> Enabling I/O.
    - VolumeIODisabledPhase: The derived IO-down span (Disabling I/O -> Enabling I/O).
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
from typing import Generator, List, Dict, Optional, Any, Iterable, Tuple


# Default window to analyze in --analyze mode if --until is omitted
DEFAULT_ANALYSIS_WINDOW_SECONDS = 300

# traces are flushed towards the trace daemon at most every two seconds
TRACE_FLUSH_SECONDS = 2

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
    def __str__(self) -> str:
        return ""

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

    def __str__(self):
        return f"[{self.timestamp.isoformat()}] {self.syslog_id} ({self.unit}): {self.message}"

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

    def __str__(self):
        return f"[{self.timestamp.isoformat()}] {self.dev_name} ({self.func_name}): {self.message}"

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
    def __init__(self):
        # use a safe past default (Jan 1, 2025)
        self.last_seen_timestamp = datetime(2025, 1, 1).astimezone()  # track state

    def get_entries(self, cmd: List[str], cwd: Optional[str] = None) -> Generator[BaseLogEntry, None, None]:
        """
        Runs a command and yields parsed LogEntry objects line by line.
        """
        logger.debug(f"Running command: {' '.join(cmd)}")

        process = None
        try:
            process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding='utf-8',
                cwd=cwd
            )

            for line in process.stdout:
                # 1. _parse_line handles garbage (catches JSONDecodeError -> returns None).
                # 2. _parse_line handles fatal errors (raises ValueError).
                # 3. get_entries simply yields valid results.
                # Call the abstract method to do the parsing
                entry = self._parse_line(line)
                if entry:
                    self.last_seen_timestamp = entry.timestamp
                    yield entry

            # read stderr after stdout is exhausted
            stderr_output, stderr_error = process.communicate()
            if stderr_output:
                # use logging for warnings
                logger.warning(f"{cmd[0]} stderr: {stderr_output.strip()}")

        except FileNotFoundError:
            logger.error(f"Command not found: {cmd[0]}")

        except ValueError as e:
            # catch fatal errors raised by _parse_line (e.g. empty message)
            logger.error(f"Fatal Log Parsing Error: {e}")
            raise

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
    def __init__(self, identifiers: List[str]):
        super().__init__()
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
        # 1. attempt parse (is it valid JSON?)
        try:
            log_json = json.loads(line.strip())
        except json.JSONDecodeError as e:
            logger.debug(f"Skipping malformed JSON line: {line.strip()} | Error: {e}")
            return None

        # 2. strict validation (Does it have the required data?)
        # this runs outside the try/catch because we want explicit ValueErrors to bubble up as fatal
        if not log_json.get("MESSAGE"):
            raise ValueError(f"Invalid Log Entry (empty message) from {type(self).__name__}. Raw: {line.strip()}")

        # 3. parse timestamp
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

    def __init__(self, logs_dir: Optional[str] = None, custom_pager: Optional[str] = None):
        super().__init__()
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
            '-f', 'has @NDU'
        ]

        # Calls the *base class* method
        yield from self.get_entries(cmd, cwd=self.pager_cwd)

    def _parse_line(self, line: str) -> Optional[BaseLogEntry]:
        # 1. attempt parse (is it valid JSON?)
        try:
            log_json = json.loads(line.strip())
        except json.JSONDecodeError as e:
            logger.debug(f"Skipping malformed JSON line: {line.strip()} | Error: {e}")
            return None

        # 2. strict validation (fatal error)
        # ensure the log has content
        if not log_json.get('message'):
            raise ValueError(f"Invalid Log Entry (empty message) from {type(self).__name__}. Raw: {line.strip()}")

        # 3. parse timestamp (skip if missing)
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
    def __init__(self, logs_dir: str, identifiers: List[str]):
        super().__init__()
        self.logs_dir = logs_dir
        self.identifiers = identifiers  # store the allow-list
        self.log_file = self._find_journal_file(logs_dir)

        # Regex for: "Nov 23 09:28:12.421787 hostname identifier: message"
        self.line_regex = re.compile(r'^([A-Z][a-z]{2}\s+\d+\s\d{2}:\d{2}:\d{2}\.\d{6})\s+\S+\s+([^:]+):\s+(.*)$')

        # Regex to extract unit from systemd messages
        self.systemd_unit_regex = re.compile(r'(Stopping|Stopped|Starting|Started)\s+([a-zA-Z0-9@_\-\.\(\) ]+)')

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

    def _parse_line_context(self, line: str, since: datetime, until: datetime, year: int) -> Tuple[Optional[BaseLogEntry], bool]:
        match = self.line_regex.match(line.strip())
        if not match:
            return None, False

        ts_str, ident_raw, msg = match.groups()

        # 1. Filter by Identifier
        # Clean "systemd[1]" -> "systemd"
        identifier = ident_raw.split('[', 1)[0]  # split always return at least one element

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

        # strict validation: Fatal if one of "our" logs has an empty message
        if not msg or not msg.strip():
            raise ValueError(f"Invalid Log Entry (empty message) from {type(self).__name__} for identifier '{identifier}'. Raw: {line.strip()}")

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
from enum import Enum

class PhaseStatus(Enum):
    PENDING = "PENDING"
    VALID = "VALID"
    INCOMPLETE = "INCOMPLETE"
    EMPTY = "EMPTY"
    INVALID = "INVALID"  # for logical contradictions and composite status

class BasePhase(ABC):
    """
    [REVISED] Abstract Base Class for a single, measurable NDU phase.
    """
    def __init__(self, name: str):
        self.name: str = name
        self._start: Optional[datetime] = None
        self._end: Optional[datetime] = None

        # Finalization State
        self.status = PhaseStatus.PENDING
        self.warnings: List[str] = []
        self._final_interval: Optional[TimeInterval] = None
        self._final_duration: int = 0
        self._self_time_ms: int = 0  # Cached self time

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
        Default implementation for completeness.
        Returns True if the phase has found both its start and end.
        """
        return bool(self._start and self._end)

    def set_start(self, ts: datetime, source_msg: Optional[str] = None, overwrite: bool = False):
        """
        Strict setter for Start Timestamp.
        Raises RuntimeError if attempting to overwrite an existing value,
        unless overwrite=True is passed.
        If 'source_msg' is provided, it logs the debug match message automatically.
        """
        if self._start is not None and not overwrite:
            # idempotency check: if exact same time, ignore (and don't spam logs)
            if self._start == ts:
                return
            self_start_str = self._start.astimezone().strftime('%H:%M:%S.%f')[:-3]
            ts_str = ts.astimezone().strftime('%H:%M:%S.%f')[:-3]
            raise RuntimeError(f"[{self.name}] start timestamp overwrite! Old: {self_start_str}, New: {ts_str}")

        self._start = ts

        # centralized Logging
        if source_msg:
            ts_str = ts.astimezone().strftime('%H:%M:%S.%f')[:-3]
            logger.debug(f"[{self.name}]: matched start: {source_msg.strip()} ({ts_str})")

    def set_end(self, ts: datetime, source_msg: Optional[str] = None, overwrite: bool = False):
        """
        Strict setter for End Timestamp.
        Raises RuntimeError if attempting to overwrite an existing value,
        unless overwrite=True is passed.
        If 'source_msg' is provided, it logs the debug match message automatically.
        """
        if self._end is not None and not overwrite:
            # idempotency check
            if self._end == ts:
                return
            self_end_str = self._end.astimezone().strftime('%H:%M:%S.%f')[:-3]
            ts_str = ts.astimezone().strftime('%H:%M:%S.%f')[:-3]
            raise RuntimeError(f"[{self.name}] end timestamp overwrite! Old: {self_end_str}, New: {ts_str}. log entries may be absent from source.")

        self._end = ts

        # centralized Logging
        if source_msg:
            ts_str = ts.astimezone().strftime('%H:%M:%S.%f')[:-3]
            logger.debug(f"[{self.name}]: matched stop: {source_msg.strip()} ({ts_str})")

    @property
    def interval(self) -> Optional[TimeInterval]:
        """
        Default interval calculation.
        """
        if self._start and self._end and self._end >= self._start:
            return TimeInterval(self._start, self._end)
        elif self._start and self._end:
            # convert to local time for display consistency with the report
            s_local = self._start.astimezone()
            e_local = self._end.astimezone()
            logger.error(f"Phase '{self.name}' has invalid interval. Start: {s_local}, End: {e_local}")
        return None

    @property
    def has_warnings(self) -> bool:
        """Returns True if this phase has any warnings."""
        return len(self.warnings) > 0

    def finalize(self):
        """
        Freezes the state of the phase.
        Calculates duration, validates timestamps, and sets status.
        """
        # 1. Get the interval (polymorphic call to property)
        interval = self.interval

        # 2. Validate and Freeze
        if interval:
            self._final_interval = interval
            self._final_duration = interval.duration_ms
            self._self_time_ms = self._final_duration  # Default for leaf
            self.status = PhaseStatus.VALID
        else:
            # If we have a start but no end (or vice versa), it's incomplete
            if self._start or self._end:
                self.status = PhaseStatus.INCOMPLETE
                missing = "end" if self._start else "start"
                self.warnings.append(f"Missing {missing} timestamp")
            else:
                self.status = PhaseStatus.EMPTY
                if self.name != "some_optional_phase":  # Optional check
                    self.warnings.append("Phase completely missing (no start/end found)")

    def print_report(self, level: int = 0):
        """
        Prints the finalized report for this phase.
        """
        indent = "  " * level

        # Format Duration
        if self.status == PhaseStatus.EMPTY:
            duration_str = "N/A"
            interval_str = ""
        else:
            duration_str = f"{self._final_duration} ms"
            interval_str = ""
            # Only print interval details if we have them (even if incomplete)
            if self._final_interval:
                # format directly instead of splitting to_str() output
                begin_str = self._final_interval.begin.astimezone().strftime('%H:%M:%S.%f')[:-3]
                end_str = self._final_interval.end.astimezone().strftime('%H:%M:%S.%f')[:-3]
                interval_str = f"({begin_str} .. {end_str})"
            elif self._start:
                # Print partial info if available
                ts = self._start.astimezone().strftime('%H:%M:%S.%f')[:-3]
                interval_str = f"({ts} .. ???)"

        # Print the line
        # Note: self_time_ms will be updated by CompositePhase for branches
        self_time_str = f"(Self: {self._self_time_ms} ms)" if self._self_time_ms != self._final_duration else ""

        print(f"{indent}- {self.name:<22} {duration_str:<10} {self_time_str:<16} {interval_str}")

        # Print Warnings
        for warning in self.warnings:
            print(f"{indent}  !! WARNING: {warning} !!")

    def to_dict(self) -> Dict[str, Any]:
        """Serializes the phase to a dictionary."""

        def format_ts(dt: Optional[datetime]) -> Optional[str]:
            if not dt:
                return None
            # 1. Convert to Local Time (matches the text report)
            local_dt = dt.astimezone()

            # 2. Format as "YYYY-MM-DD HH:MM:SS.mmm"
            # %f gives microseconds (6 digits), so we slice [:-3] to get milliseconds
            return local_dt.strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]

        return {
            "name": self.name,
            "status": self.status.value,
            "duration_ms": self._final_duration,
            "self_time_ms": self._self_time_ms,
            "start": format_ts(self._final_interval.begin) if self._final_interval else None,
            "end": format_ts(self._final_interval.end) if self._final_interval else None,
            "warnings": self.warnings
        }
# ---

class CompositePhase(BasePhase):
    """
    A BasePhase that can contain a collection of child BasePhase objects.
    Its own interval/completeness is independent of its children.
    """
    def __init__(self, name: str):
        super().__init__(name)
        self.children: Dict[str, BasePhase] = {}

    def add_child(self, child_phase: BasePhase):
        """Adds a child phase to be processed."""
        if child_phase.name in self.children:
            logger.warning(f"Duplicate child phase name: {child_phase.name}")
        self.children[child_phase.name] = child_phase

    def process_entry(self, entry: BaseLogEntry) -> bool:
        """
        Fans out the log entry to all children.
        The parent phase (e.g., SimpleSystemdPhase) is responsible for
        calling this *and* processing the entry for its *own* _start/_end.
        """
        consumed_by_child = False
        for child in self.children.values():
            if child.process_entry(entry):
                consumed_by_child = True

        # Returns True if any child consumed the log.
        return consumed_by_child

    # --- NO is_complete override ---
    # composite will use the BasePhase default (bool(self._start and self._end))
    # as some child phases (e.g. modules unload (client stop) or modules load (client start)) may not be logged

    # --- NO interval override ---
    # composite uses its own _start and _end, as the interval sum of child phases may not fully span the parent
    # The *one exception* will be NDUPhase, which *will* override this to be the min/max of its volume children

    def _calculate_min_max_from_list(self, phases: Iterable[BasePhase]) -> Optional[TimeInterval]:
        """
        Internal Helper: Calculates the interval spanning the min start and max end
        of the provided collection of phases.

        Side Effect: Updates self._start and self._end to match the calculated window.
        """
        valid_intervals = [p.interval for p in phases if p.interval is not None]
        if not valid_intervals:
            return None

        # Update internal state
        self._start = min(i.begin for i in valid_intervals)
        self._end = max(i.end for i in valid_intervals)

        return TimeInterval(self._start, self._end)

    @property
    def min_max_interval(self) -> Optional[TimeInterval]:
        """
        Calculates the interval based on the min/max of all CHILDREN.
        Derived classes can return this property in their 'interval' override.
        """
        return self._calculate_min_max_from_list(self.children.values())

    @property
    def extended_interval(self) -> Optional[TimeInterval]:
        """
        Calculates an interval that starts at the Parent's own start time,
        but extends the End time to cover the latest completing child.
        """
        if not (self._start and self._end):
            return None

        final_end = max(
            [self._end] +
            [c.interval.end for c in self.children.values() if c.status == PhaseStatus.VALID and c.interval]
        )

        return TimeInterval(self._start, final_end)

    @property
    def has_warnings(self) -> bool:
        """Returns True if this phase OR any child has warnings."""
        if super().has_warnings:
            return True
        return any(c.has_warnings for c in self.children.values())

    def get_all_child_phases(self) -> List[BasePhase]:
        """A helper to return all child phases for verbose reporting."""
        return sorted(self.children.values(), key=lambda p: p.name)

    def finalize(self):
        """
        Recursive finalization with strict child validation.
        1. Finalize all children.
        2. Calculate aggregates (children duration sum).
        3. Calculate self_time (Own - Children).
        4. Composite Validation
        """
        children_duration_sum = 0
        has_invalid_child = False
        invalid_child_reason = None

        # 1. Finalize Children first
        for child in self.children.values():
            child.finalize()

            if child.status == PhaseStatus.VALID:
                children_duration_sum += child._final_duration
            else:
                # strict validation: capture the first child that isn't VALID
                # This covers EMPTY, INCOMPLETE, and INVALID states.
                has_invalid_child = True
                if not invalid_child_reason:
                    invalid_child_reason = f"Child '{child.name}' is {child.status.value}"
                    logger.error(f"[{self.name}] failure detected: {invalid_child_reason}")

        # 2. Finalize Self (Calculate Interval & Duration)
        # This calls BasePhase.finalize() logic to freeze _final_duration
        super().finalize()

        # 3. Calculate Self Time (Delta)
        if self.status == PhaseStatus.VALID:
            self._self_time_ms = max(0, self._final_duration - children_duration_sum)
        else:
            self._self_time_ms = 0

        # 4. Composite Validation - propagate invalidity
        # If any child failed (is not VALID), the parent must also be INVALID.
        if has_invalid_child:
            # Downgrade status to INVALID (unless we were already INVALID/EMPTY?)
            # Actually, INVALID takes precedence over everything.
            self.status = PhaseStatus.INVALID
            self.warnings.append(f"Marked INVALID because: {invalid_child_reason}")

    def print_report(self, level: int = 0):
        """
        Recursive print.
        """
        # 1. Print Self
        super().print_report(level)

        # 2. Print Children (Sorted by start time)
        # We need to handle cases where interval is None for sorting
        def sort_key(c):
            if c._final_interval: return c._final_interval.begin
            if c._start: return c._start
            return datetime.max.replace(tzinfo=timezone.utc)

        sorted_children = sorted(self.children.values(), key=sort_key)

        for child in sorted_children:
            child.print_report(level + 1)

    def to_dict(self) -> Dict[str, Any]:
        data = super().to_dict()
        # Recursively serialize children
        data["children"] = [c.to_dict() for c in self.children.values()]
        return data
# ---
class ContainerPhase(CompositePhase):
    """
    A Composite Phase that acts purely as a container.
    It does not have its own Start/End log events.
    Its completion is determined solely by the state of its children.
    """
    @property
    def is_complete(self) -> bool:
        # If no children, we assume we are waiting for discovery or initialization.
        # This keeps the phase "Incomplete" to prevent premature cutoff.
        if not self.children:
            return False
        return all(c.is_complete for c in self.children.values())

    @property
    def interval(self) -> Optional[TimeInterval]:
        # Containers define their span by the min/max of their children.
        return self.min_max_interval

# Inherits from CompositePhase
class SimpleSystemdPhase(CompositePhase):
    """
    A reusable phase defined by systemd messages.
    It can now also contain child phases to break down its own interval.
    """
    def __init__(self, name: str, service_name: str, start_msg_prefix: str, end_msg_prefix: str):
        super().__init__(name) # This initializes self.children = {}
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
        if isinstance(entry, JournalCTLLogEntry) and \
                entry.syslog_id == 'systemd' and \
                entry.unit == self.service_name and \
                entry.message:

            if entry.message.startswith(self.start_msg_prefix):
                self.set_start(entry.timestamp, source_msg=entry.message)
                consumed_by_self = True
            elif entry.message.startswith(self.end_msg_prefix):
                self.set_end(entry.timestamp, source_msg=entry.message)
                consumed_by_self = True

        return consumed_by_self or consumed_by_child

class ClientStopPhase(SimpleSystemdPhase):
    """Tracks the 'client stop' phase (Stopping... to Stopped...)."""
    def __init__(self):
        super().__init__("client stop", "nvmeshclient.service", "Stopping", "Stopped")
        self.add_child(ShutdownPhase())
        self.add_child(ModulesUnLoadPhase())

class ClientStartPhase(SimpleSystemdPhase):
    """Tracks the 'client start' phase (Starting... to Started...)."""
    def __init__(self):
        super().__init__("client start", "nvmeshclient.service", "Starting", "Started")
        self.add_child(ModulesLoadPhase())
        # Volume Attaching (Dynamic Discovery)
        self.add_child(VolumesAttachPhase())

    @property
    def interval(self) -> Optional[TimeInterval]:
        """
        [OVERRIDDEN] Sets the phase end time to the completion of 'Volumes Attach'.
        This ensures the phase duration accurately reflects the time until
        I/O was restored, ignoring any subsequent systemd overhead.
        """
        # 1. Get the standard interval to establish the START time
        standard_interval = super().interval
        if not standard_interval:
            return None

        # 2. Look up the 'Volumes Attach' child phase
        vol_attach = self.children.get("Volumes Attach")

        # 3. If Volumes Attach exists and has a valid interval, use its end time
        if vol_attach and vol_attach.interval:
            # The phase ends exactly when the last volume is enabled.
            # We use the systemd start time, but the volumes' end time.
            return TimeInterval(standard_interval.begin, vol_attach.interval.end)

        # Fallback: If no volumes attached, return the standard systemd interval
        return standard_interval

    @property
    def is_complete(self) -> bool:
        # Must wait for BOTH the service to start AND all volume attach operations to finish.
        return super().is_complete and all(c.is_complete for c in self.children.values())

class DependencyRestartPhase(ContainerPhase):
    """
    Tracks the full restart of parallel services (CM and TD).
    Interval is from the *first* service stopping to the *last* service started.
    """
    def __init__(self):
        super().__init__("CM/TD restart")
        self.add_child(SimpleSystemdPhase("NVMesh CM Stop", "nvmeshcm.service", "Stopping", "Stopped"))
        self.add_child(SimpleSystemdPhase("NVMesh CM Start", "nvmeshcm.service", "Starting", "Started"))
        self.add_child(SimpleSystemdPhase("NVMesh TD Stop", "nvmeshtrace@trace_daemon.service", "Stopping", "Stopped"))
        self.add_child(SimpleSystemdPhase("NVMesh TD Start", "nvmeshtrace@trace_daemon.service", "Starting", "Started"))

# Enum: nvmeibc_mod_state
# Maps internal kernel module states to their integer values in the trace logs.
NVMEIBC_STATE_INITIALIZING = '0'  # Module is initializing, first instance created
NVMEIBC_STATE_READY        = '1'  # Module completed initialization & is operational
NVMEIBC_STATE_PREP_RM      = '2'  # Preparing for removal (cannot attach new vols)
NVMEIBC_STATE_RM_RDY       = '3'  # Ready to be removed (no vols attached)
NVMEIBC_STATE_EXITING      = '4'  # Exiting (memory kfree, proc removal)

class ShutdownPhase(CompositePhase):
    """Tracks the duration of the nvmesh_clnt_shutdown script."""
    def __init__(self):
        super().__init__("client shutdown")
        # Volume Detaching (Dynamic Discovery)
        self.add_child(VolumesDetachPhase())

    def process_entry(self, entry: BaseLogEntry) -> bool:
        consumed_by_child = super().process_entry(entry)
        consumed_by_self = False
        msg = entry.message
        if isinstance(entry, JournalCTLLogEntry) and entry.syslog_id == 'nvmeshclient' and msg:
            if "client shutdown script invoked" in msg:
                self.set_start(entry.timestamp, source_msg=entry.message)
                consumed_by_self = True
            if "client shutdown script done" in msg:
                self.set_end(entry.timestamp, source_msg=entry.message)
                consumed_by_self = True

        return consumed_by_self or consumed_by_child

    @property
    def interval(self) -> Optional[TimeInterval]:
        # Use the reusable "Async Parent" logic
        return self.extended_interval

class ModulesUnLoadPhase(BasePhase):
    """A child-phase that finds the "Starting to unload" and "Done unloading" messages from the nvmeshclient log."""
    def __init__(self):
        super().__init__("modules unload")

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if not isinstance(entry, JournalCTLLogEntry):
            return False
        msg = entry.message
        # Look for the log lines from the nvmeshclient script
        if entry.syslog_id == 'nvmeshclient' and msg:
            if "Starting to unload modules" in msg:
                self.set_start(entry.timestamp, source_msg=entry.message)
                return True
            elif "Done unloading modules" in msg:
                self.set_end(entry.timestamp, source_msg=entry.message)
                return True
        return False

class ModulesLoadPreClientInitPhase(BasePhase):
    """
    Child Phase 1: Pre-Initialization ($t1 \to t2$).

    Definitions:
      t1: "Starting to load modules" (User Space Start)
      t2: "MODULE_STATE_CHANGE state=0->0" (Kernel Init Start)

    Interval: Script execution start -> Kernel client module first instruction.
    Covers: dependency (common module) loading, modprobe overhead and kernel linking/relocation.
    """

    def __init__(self):
        super().__init__("Pre-Initialization")

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # Start ($t1): "Starting to load modules"
        if isinstance(entry, JournalCTLLogEntry) and \
                entry.syslog_id == 'nvmeshclient' and entry.message:
            if "Starting to load modules" in entry.message:
                self.set_start(entry.timestamp, source_msg=entry.message)
                return True

        # End ($t2): State 0->0 (Entering INITIALIZING)
        if isinstance(entry, PagerLogEntry) and entry.message:
            # Logic: module state=0->state=0
            target_state = f"state={NVMEIBC_STATE_INITIALIZING}->state={NVMEIBC_STATE_INITIALIZING}"

            if "MODULE_STATE_CHANGE" in entry.message and target_state in entry.message:
                self.set_end(entry.timestamp, source_msg=entry.message)
                return True

        return False

class ModulesLoadClientInitCorePhase(BasePhase):
    """
    Child of Client Init.
    Interval: "client globals create (core) - start" -> "done".
    """
    def __init__(self):
        super().__init__("Client Core Init")

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if not isinstance(entry, PagerLogEntry):
            return False

        msg = entry.message
        # Start Trigger
        if "client globals create (core) - start" in msg:
            self.set_start(entry.timestamp, source_msg=entry.message)
            return True

        # End Trigger
        if "client globals create (core) - done" in msg:
            self.set_end(entry.timestamp, source_msg=entry.message)
            return True

        return False



class ModulesLoadClientInitPhase(CompositePhase):
    """
    Child Phase 2: Kernel client initialization ($t2 \to t3$).

    Definitions:
      t2: "MODULE_STATE_CHANGE state=0->0" (Kernel Init Start)
      t3: "MODULE_STATE_CHANGE state=0->1" (Kernel Ready)

    Interval: Kernel client module init() execution -> Module Ready.
    Covers: The execution of the module's __init function (e.g., nvmeibc_init).
    """

    def __init__(self):
        super().__init__("Client Init")
        self.add_child(ModulesLoadClientInitCorePhase())

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if not isinstance(entry, PagerLogEntry):
            return False

        consumed_by_child = super().process_entry(entry)
        consumed_by_self = False
        msg = entry.message
        if "MODULE_STATE_CHANGE" not in msg:
            return False

        # Start ($t2): State 0->0 (Entering INITIALIZING)
        start_signature = f"state={NVMEIBC_STATE_INITIALIZING}->state={NVMEIBC_STATE_INITIALIZING}"
        if start_signature in msg:
            self.set_start(entry.timestamp, source_msg=entry.message)
            consumed_by_self = True

        # End ($t3): State 0->1 (Transition to READY)
        end_signature = f"state={NVMEIBC_STATE_INITIALIZING}->state={NVMEIBC_STATE_READY}"
        if end_signature in msg:
            self.set_end(entry.timestamp, source_msg=entry.message)
            return True

        return consumed_by_self or consumed_by_child

class ModulesLoadPhase(CompositePhase):
    """
    Tracks the full 'modules load' phase.
    Parent Interval: "Starting to load" ($t1$) -> "Done loading".

    Timeline:
      t1 .... (Pre-Init) .... t2 .... (Client Init) .... t3 .... (Cleanup) .... Done
    """

    def __init__(self):
        super().__init__("modules load")
        # Add the new breakdown phases
        self.add_child(ModulesLoadPreClientInitPhase())
        self.add_child(ModulesLoadClientInitPhase())

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # 1. Feed children first (so they can catch t1, t2, t3)
        consumed_by_child = super().process_entry(entry)

        # 2. Check for Parent boundaries (t1 -> Done)
        consumed_by_self = False

        if isinstance(entry, JournalCTLLogEntry):
            if entry.syslog_id == 'nvmeshclient' and entry.message:

                # Parent Start ($t1) - Same as PreInit Start
                if "Starting to load modules" in entry.message:
                    self.set_start(entry.timestamp, source_msg=entry.message)
                    consumed_by_self = True

                # Parent End ("Done") - Happens after t3 (script cleanup)
                elif "Done loading modules" in entry.message:
                    self.set_end(entry.timestamp, source_msg=entry.message)
                    consumed_by_self = True

        return consumed_by_self or consumed_by_child

class VolumeDetachPhase(BasePhase):
    """
    Tracks the detach operation for a single volume.
    Start: "Detach started"
    End:   "Detach ended" or "Detach failed"
    """

    def __init__(self, name: str):
        super().__init__(name)

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # 1. Safety and Relevance Checks
        if not isinstance(entry, PagerLogEntry):
            return False

        # Only process logs belonging to this specific volume
        if entry.dev_name != self.name:
            return False

        msg = entry.message

        # 2. Logic
        if "Disabling I/O" in msg:
            self.set_start(entry.timestamp, source_msg=entry.message)
            return True

        if "Detach finished" in msg or "Detach failed" in msg:
            self.set_end(entry.timestamp, source_msg=entry.message)
            return True

        return False

class DynamicVolumesPhase(ContainerPhase):
    """
    Base class for top-level phases that dynamically discover volumes from Pager logs.
    Handles the discovery logic and interval calculation.
    """
    def __init__(self, name: str):
        super().__init__(name)

    @abstractmethod
    def _create_volume_child(self, vol_name: str) -> BasePhase:
        """Factory method: Subclasses must return the specific child object."""
        pass

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # 1. Dynamic Discovery Logic
        if isinstance(entry, PagerLogEntry) and entry.dev_name:
            vol_name = entry.dev_name

            # Check if we already track this volume
            if vol_name not in self.children:
                # Use the factory method to create the specific type of child
                new_child = self._create_volume_child(vol_name)
                self.add_child(new_child)
                logger.debug(f"[{self.name}]: Discovered new volume: {vol_name}")

        # 2. Standard Composite processing (fan-out to children)
        return super().process_entry(entry)

    def finalize(self):
        """
        [OVERRIDDEN] Custom finalization to handle the 'no volumes' case gracefully.
        """
        # 1. Run standard logic (calculates interval, sets status, adds generic warnings)
        super().finalize()

        # 2. Check for the specific "Empty Discovery" case
        if not self.children:
            # We found no volumes. This explains why the interval is missing.
            # Replace the generic "Phase completely missing" warning with a specific one.
            self.warnings = ["No volumes discovered"]

            # Optional: If you prefer the report to show "0 ms" instead of "N/A",
            # you can force the status to VALID here.
            # For now, we leave it as EMPTY (N/A) but with the warning.
            # self.status = PhaseStatus.VALID
#
class VolumesDetachPhase(DynamicVolumesPhase):
    """
    Top-level phase that tracks Detach operations across ALL volumes.
    """
    def __init__(self):
        super().__init__("Volumes Detach")

    def _create_volume_child(self, vol_name: str) -> BasePhase:
        return VolumeDetachPhase(vol_name)

class VolumeAttachConf2LastCont(BasePhase):
    """
    Child phase 1: From 'Attach finished' to the LAST 'CONT disk'.
    """

    def __init__(self, vol_name: str):
        super().__init__(f"Config->Cont")
        self.vol_name = vol_name

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if not isinstance(entry, PagerLogEntry):
            return False

        if entry.dev_name != self.vol_name:
            return False

        msg = entry.message

        # Start condition
        if "Attach finished" in msg:
            self.set_start(entry.timestamp, source_msg=entry.message)
            return True

        # End condition (Update repeatedly to find the *last* one)
        if "CONT disk" in msg:
            self.set_end(entry.timestamp, source_msg=entry.message, overwrite=True)
            # We return True because we matched, but we rely on the parent
            # to keep feeding us so we can update _end if another CONT appears.
            return True

        return False

class VolumeAttachLastCont2IOEnabled(BasePhase):
    """
    Child phase 2: From LAST 'CONT disk' to 'Enabling I/O'.
    """

    def __init__(self, vol_name: str):
        super().__init__(f"Cont->Enabled")
        self.vol_name = vol_name

    def process_entry(self, entry: BaseLogEntry) -> bool:
        if not isinstance(entry, PagerLogEntry):
            return False

        if entry.dev_name != self.vol_name:
            return False

        msg = entry.message

        # Start condition (Update repeatedly to match the *last* CONT disk)
        # This ensures this phase starts exactly where the previous one ended
        if "CONT disk" in msg:
            self.set_start(entry.timestamp, source_msg=entry.message, overwrite=True)
            return True

        # End condition
        if "Enabling I/O" in msg:
            self.set_end(entry.timestamp, source_msg=entry.message)
            return True

        return False

class VolumeAttachPhase(ContainerPhase):
    """
    Composite phase for a SINGLE volume's attach process.
    Contains the two sub-phases defined above.
    """

    def __init__(self, vol_name: str):
        super().__init__(vol_name)
        self.vol_name = vol_name
        # Add the two specific children
        self.add_child(VolumeAttachConf2LastCont(vol_name))
        self.add_child(VolumeAttachLastCont2IOEnabled(vol_name))

    @property
    def interval(self) -> Optional[TimeInterval]:
        """
        Overridden interval: min/max of all child phases.
        (Using explicit logic since refactor is not applied yet)
        """
        valid_intervals = [c.interval for c in self.children.values() if c.interval is not None]
        if not valid_intervals:
            return None

        self._start = min(i.begin for i in valid_intervals)
        self._end = max(i.end for i in valid_intervals)

        return TimeInterval(self._start, self._end)


class VolumesAttachPhase(DynamicVolumesPhase):
    """
    Top-level phase that tracks Attach operations across ALL volumes.
    """
    def __init__(self):
        super().__init__("Volumes Attach")

    def _create_volume_child(self, vol_name: str) -> BasePhase:
        return VolumeAttachPhase(vol_name)

class VolumeIODisabledPhase(BasePhase):
    """
    Tracks the IO disabled interval for a *single* volume.
    Passive Phase: State is populated strictly from finalized Detach/Attach data.
    """
    def __init__(self, name: str, ):
        super().__init__(name)

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # Passive phase; does not consume logs.
        return False

class VolumesIODisabledPhase(ContainerPhase):
    """
    Composite phase that tracks I/O Disabled state across ALL volumes.
    Passive Phase: Populated by bridging FINALIZED data from Detach and Attach phases.
    Interval: min(disabling) .. max(enabling)
    """

    def __init__(self):
        super().__init__("IO Disabled")

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # Passive phase; does not consume logs.
        return False

    def populate_from_phases(self, detach_phase: BasePhase, attach_phase: BasePhase):
        """
        Creates IO phases using the FROZEN state of the source phases.
        This ensures consistency with earlier processing of logs/traces.
        """
        # Ensure we are working with CompositePhases that have children
        if not isinstance(detach_phase, CompositePhase):
            return

        # Iterate over Detach phases (The Source of Truth for volume existence)
        for vol_name, detach_child in detach_phase.children.items():
            io_child = VolumeIODisabledPhase(vol_name)

            # --- 1. Strict Start Time (From Detach) ---
            # We check the finalized status, not the dynamic property.
            if detach_child.status == PhaseStatus.VALID and detach_child._final_interval:
                # Best case: Detach completed successfully.
                io_child._start = detach_child._final_interval.begin
            elif detach_child._start:
                # Fallback: Detach incomplete, but we have a start timestamp.
                io_child._start = detach_child._start
            # If detach_child is EMPTY, io_child._start remains None.

            # --- 2. Strict End Time (From Attach) ---
            if isinstance(attach_phase, CompositePhase):
                attach_child = attach_phase.children.get(vol_name)

                if attach_child:
                    if attach_child.status == PhaseStatus.VALID and attach_child._final_interval:
                        # Best case: Attach completed successfully.
                        io_child._end = attach_child._final_interval.end
                    elif attach_child._end:
                        # Fallback: Attach incomplete/broken, but we have an end timestamp.
                        io_child._end = attach_child._end

            self.add_child(io_child)

class NDUPhase(CompositePhase):
    """
    The root phase.
    Children: Service phases (client stop, deps restart, client start).
    Member: io_disabled_phase (derived from volumes detach/attach phases).
    """
    def __init__(self):
        super().__init__('NDU')
        # 1. Standard Children (Service Phases)
        # (These are added via setup_ndu_phases using add_child)

        # 2. Special Member (volumes IO disabled phase)
        # This is a PASSIVE container. It is populated during finalize().
        self.io_disabled_phase = VolumesIODisabledPhase()

    def process_entry(self, entry: BaseLogEntry) -> bool:
        # Only feed Service Phases.
        return super().process_entry(entry)

    @property
    def is_complete(self) -> bool:
        """
        The NDU is complete when all Service phases are complete.
        """
        # Iterate directly to check all service children (Stop, Restart, Start)
        return all(c.is_complete for c in self.children.values())

    @property
    def interval(self) -> Optional[TimeInterval]:
        """
        [OVERRIDDEN] Determines the overall NDU time window.

        Primary: Returns the "IO Disabled" interval (if populated).
        Fallback: Returns the Service interval.
        """
        # 1. Priority: IO Disabled Interval (available after finalize)
        io_interval = self.io_disabled_phase.interval
        if io_interval:
            return io_interval

        # 2. Fallback: Service Interval
        # If no volumes, the NDU interval is the span of all service phases
        return self.min_max_interval

    @property
    def has_warnings(self) -> bool:
        """Check services (children) AND derived volumes (member)."""
        if super().has_warnings:
            return True
        return self.io_disabled_phase.has_warnings

    def find_phase(self, target_name: str) -> Optional[BasePhase]:
        """
        Public API: Find a phase by name starting from this root.
        """
        # Internal recursive helper to traverse the tree
        def _search(node: BasePhase) -> Optional[BasePhase]:
            if node.name == target_name:
                return node

            if isinstance(node, CompositePhase):
                for child in node.children.values():
                    found = _search(child)
                    if found:
                        return found
            return None

        return _search(self)

    def finalize(self):
        """
        Coordinator Finalization.
        1. Finalize Services.
        2. Derive IO Disabled phase from Detach/Attach phases.
        3. Finalize IO.
        4. Compute Grand Total.
        """
        # 1. Finalize Services (children) - this might set status=INVALID via composite logic
        super().finalize()

        # 2. Locate Source Phases for Derivation
        detach_phase = self.find_phase("Volumes Detach")
        attach_phase = self.find_phase("Volumes Attach")

        if detach_phase and attach_phase:
            logger.debug("Deriving IO Disabled phase from Detach/Attach...")
            # 3. Populate IO (The Data Transfer)
            self.io_disabled_phase.populate_from_phases(detach_phase, attach_phase)
        else:
            logger.debug("Could not find Detach/Attach phases for derivation.")

        # 4. Finalize IO (The Calculation)
        # Calculates durations and warnings for the newly created volume phases
        self.io_disabled_phase.finalize()

        # 5. Compute Grand Total NDU Time
        # Explicitly calculate Service Time from the children (Services)
        # This is safer than relying on self._final_duration from step 1
        service_interval = self.min_max_interval
        service_time = service_interval.duration_ms if service_interval else 0

        io_time = self.io_disabled_phase._final_duration

        # Logic: Use IO time if available/valid, otherwise fallback to Service time
        self._final_duration = io_time if io_time > 0 else service_time

        # 6. Set Status
        # Only set to VALID if we aren't already INVALID from step 1
        if (service_time > 0 or io_time > 0) and self.status != PhaseStatus.INVALID:
            self.status = PhaseStatus.VALID

        # Topological Validation (Split-Brain Protection)
        # Check if Client Start appears BEFORE Client Stop (impossible in a single valid NDU)
        client_stop = self.children.get("client stop")
        client_start = self.children.get("client start")

        if client_stop and client_start and client_stop._start and client_start._start:
            if client_start._start < client_stop._start:
                # We found the tail of NDU #1 and the head of NDU #2.
                msg = (f"Logical Error: 'Client Start' ({client_start._start.astimezone().strftime('%H:%M:%S')}) "
                       f"detected before 'Client Stop' ({client_stop._start.astimezone().strftime('%H:%M:%S')}). "
                       f"Analysis likely spans two different NDU runs.")
                self.warnings.insert(0, msg)  # Prepend as high priority
                self.status = PhaseStatus.INVALID

    def print_report(self, level: int = 0, verbose: bool = False):
        # Guard Clause for Invalid State
        if self.status == PhaseStatus.INVALID:
            print("\n" + "=" * 30)
            print("  NVMesh NDU Analysis (INVALID)")
            print("=" * 30 + "\n")
            for warning in self.warnings:
                print(f"!! CRITICAL FAILURE: {warning} !!")

        print("\n" + "=" * 30)
        print("  NVMesh NDU Analysis Report")
        print("=" * 30 + "\n")
        print("--- NDU Phase Breakdown ---")

        def sort_key(c):
            if c._final_interval: return c._final_interval.begin
            if c._start: return c._start
            return datetime.max.replace(tzinfo=timezone.utc)

        for child in sorted(self.children.values(), key=sort_key):
            child.print_report(level=0)

        print("---------------------------")
        service_sum = sum(c._final_duration for c in self.children.values())
        print(f"{'total NDU services:':<22} {service_sum} ms")

        io_str = f"{self.io_disabled_phase._final_duration} ms"
        if self.io_disabled_phase._final_interval:
            # Use robust formatting instead of splitting
            begin_str = self.io_disabled_phase._final_interval.begin.astimezone().strftime('%H:%M:%S.%f')[:-3]
            end_str = self.io_disabled_phase._final_interval.end.astimezone().strftime('%H:%M:%S.%f')[:-3]
            io_str += f"    ({begin_str} .. {end_str})"

        print(f"{'IO disabled:':<22} {io_str}")

        print("---------------------------")
        print(f"{'Total NDU Time (max):':<22} {self._final_duration} ms")
        print("---------------------------\n")

        if self.io_disabled_phase.children:
            print("--- verbose volume IO disabled details () ---")
            self.io_disabled_phase.print_report(level=1)  # This will print the tree of volumes

    def generate_chrome_trace(self) -> List[Dict[str, Any]]:
        trace_events = []

        # Helper to traverse the Service tree (PID 1)
        def traverse_service(phase: BasePhase):
            if phase.status == PhaseStatus.VALID and phase._final_interval:
                # Chrome Tracing uses microseconds
                ts_micros = int(phase._final_interval.begin.timestamp() * 1_000_000)
                dur_micros = int(phase._final_duration * 1000)

                trace_events.append({
                    "name": phase.name,
                    "cat": "service",
                    "ph": "X",  # Complete Event (has duration)
                    "ts": ts_micros,
                    "dur": dur_micros,
                    "pid": 1,  # Process 1: Control Plane
                    "tid": 1,
                    "args": {"warnings": phase.warnings}
                })

            if isinstance(phase, CompositePhase):
                for child in phase.children.values():
                    traverse_service(child)

        # Helper to traverse the Data tree (PID 2)
        def traverse_data(phase: BasePhase):
            if phase.status == PhaseStatus.VALID and phase._final_interval:
                ts_micros = int(phase._final_interval.begin.timestamp() * 1_000_000)
                dur_micros = int(phase._final_duration * 1000)

                trace_events.append({
                    "name": phase.name,
                    "cat": "io",
                    "ph": "X",
                    "ts": ts_micros,
                    "dur": dur_micros,
                    "pid": 2,  # Process 2: Data Plane
                    "tid": 1,
                    "args": {"vol": phase.name}
                })

            if isinstance(phase, CompositePhase):
                for child in phase.children.values():
                    traverse_data(child)

        # 1. Walk the Main Service Tree
        for child in self.children.values():
            traverse_service(child)

        # 2. Walk the Derived IO Tree
        traverse_data(self.io_disabled_phase)

        # Metadata for the viewer
        trace_events.append({"name": "process_name", "ph": "M", "pid": 1, "args": {"name": "Control Plane (Systemd)"}})
        trace_events.append({"name": "process_name", "ph": "M", "pid": 2, "args": {"name": "Data Plane (IO Latency)"}})

        return trace_events
#

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
    ndu_phase = NDUPhase()

    # 2. Create and add the main service phases as children
    ndu_phase.add_child(ClientStopPhase()) # ClientStop is now a composite
    ndu_phase.add_child(DependencyRestartPhase())
    ndu_phase.add_child(ClientStartPhase())

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
    parser.add_argument('--non-strict', action='store_true', help='Be strict w.r.t report printing.')
    parser.add_argument('--json', action='store_true', help="Output analysis result as JSON to stdout")
    parser.add_argument('--trace', metavar='FILE', help="Export Chrome Trace (Flame Graph) to the specified JSON file")
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

    except RuntimeError as e:
        # handle the Strict Setter violation
        msg = f"Fatal: Analysis Error: {e}"
        logger.error(msg)
        ndu_analysis.warnings.append(msg)
        ndu_analysis.status = PhaseStatus.INVALID
        raise

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
        )
    else:
        # Use the live command line parser
        journal_source = JournalctlLog(
            identifiers=target_identifiers,
        )

    # Pass the offline args to PagerLog
    pager_source = PagerLog(
        logs_dir=logs_dir,
        custom_pager=pager_path
    )

    log_sources = [journal_source, pager_source]

    # --- Run/Analyze Logic ---
    poll_timeout_seconds = 30  # For --run mode polling
    journal_time_format = "%Y-%m-%d %H:%M:%S.%f"

    if args.command == 'run':
        # issue the ndu and get timestamps
        since_dt, until_dt = execute_ndu_and_get_times(poll_timeout_seconds)
        # wait for the traces to be flushed to files (note: compression might delay this)
        logger.info("Waiting for traces to be flushed.")
        time.sleep(TRACE_FLUSH_SECONDS+0.5)

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

        # detect if there were no volumes attached at the start
        volumes_detach = ndu_analysis.find_phase("Volumes Detach")
        no_volumes_detected = False
        if volumes_detach and len(volumes_detach.children) == 0:
            logger.info("No volumes detected in 'Volumes Detach' phase. Skipping I/O polling.")
            no_volumes_detected = True

        # 2. Conditional Polling
        # This block is *only* entered in --run mode, and *only* if the historical pass
        # didn't find all the "I/O Enabled" events and we actually have volumes to wait for
        if args.command == 'run' and not ndu_analysis.is_complete and not no_volumes_detected:
            logger.info("Starting polling for I/O re-enable events...")
            poll_start_time = datetime.now()
            poll_interval_seconds = 0.5
            # Start from where we left off to catch late I/O events
            poll_since_dt = pager_source.last_seen_timestamp.astimezone()

            while not ndu_analysis.is_complete:
                if (datetime.now() - poll_start_time).total_seconds() > poll_timeout_seconds:
                    logger.error(f"Timed out after {poll_timeout_seconds}s waiting for I/O re-enable events.")
                    break

                poll_until_dt = datetime.now().astimezone()

                logger.info(f"Fetching more pager logs from {poll_since_dt.strftime(journal_time_format)} to {poll_until_dt.strftime(journal_time_format)}...")
                # Create the stream for *this* poll iteration
                poll_stream = pager_source.fetch_logs(poll_since_dt, poll_until_dt)

                # Process this small stream.
                process_log_stream(poll_stream, ndu_analysis, stop_on_complete=False)

                # Advance the cursor for the next iteration
                poll_since_dt = pager_source.last_seen_timestamp.astimezone()

                time.sleep(poll_interval_seconds)

    except KeyboardInterrupt:
        # This will catch the 'raise' from process_log_stream
        logger.info("\nMain processing loop interrupted.")

    logger.debug("Log processing finished.")

    # --- Finalize Step ---
    # Calculate all intervals, durations, and validations once.
    ndu_analysis.finalize()

    # --- Determine Exit Code ---
    exit_code = 0

    # 1. Critical Failure (Logical Contradictions)
    # If the analysis found logical contradictions (e.g. Start before Stop),
    # we must fail immediately, regardless of strict mode.
    if ndu_analysis.status == PhaseStatus.INVALID:
        logger.error("Analysis failed due to critical logical errors.")
        exit_code = 1
    # 2. strict policy check - fail if warnings exist unless user asked for --non-strict
    elif ndu_analysis.has_warnings:
        logger.error("Analysis failed validation checks.")
        # strict mode (default): exit 1
        # non-strict mode: keep exit 0 (allow pass)
        if not args.non_strict:
            exit_code = 1

    # --- Reporting Phase ---
    # Generate trace if requested
    if args.trace:
        trace_data = ndu_analysis.generate_chrome_trace()
        with open(args.trace, 'w') as f:
            json.dump(trace_data, f)
        logger.info(f"Trace exported to {args.trace}. Load this in ui.perfetto.dev")

    # Output Main Report (JSON or Text)
    if args.json:
        # JSON output includes status and warnings fields, so it is complete.
        print(json.dumps(ndu_analysis.to_dict(), indent=2))
    else:
        # Text output
        ndu_analysis.print_report(verbose=args.verbose)

    # --- Exit ---
    if exit_code != 0:
        sys.exit(exit_code)


if __name__ == "__main__":
    # DO NOT set a default basicConfig here.
    # main() will set it after args are parsed.
    try:
        sys.exit(main())
    except Exception as e:
        # This will catch the 'raise' from process_log_stream
        logger.exception("Main processing loop aborted due to fatal error: %s", str(e))
        sys.exit(1)
