#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Compare fio/perf run directories and print a degradation report.

The tool accepts either individual run directories containing fio.json,
flamegraph.svg and perf.data, or version directories containing run_* children.
For version directories, fio metrics and perf costs are aggregated with medians.
"""

from __future__ import annotations

import argparse
import bisect
import html
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


ACTIVE_OPS = ('read', 'write', 'trim')
DEFAULT_FOCUS_RE = r'nvme|nvmesh|nvmeib|dp_|(?:^|_)locks?(?:_|$)|_lock(?:_|$)|bio|blk|mlx|ib_|fio'
WORKLOAD_KEYS = (
	'operation',
	'jobname',
	'filename',
	'rw',
	'bs',
	'iodepth',
	'numjobs',
	'size',
	'io_size',
	'ioengine',
	'direct',
	'ramp_time',
)
FIO_METRIC_ORDER = (
	'iops',
	'bw_bytes',
	'total_ios',
	'runtime_ms',
	'lat_mean_ns',
	'clat_mean_ns',
	'slat_mean_ns',
	'usr_cpu',
	'sys_cpu',
	'ctx',
	'job_runtime_ms',
	'disk_util_pct',
)
PERCENTILE_KEYS = ('50.000000', '90.000000', '95.000000', '99.000000', '99.500000', '99.900000', '99.990000')
NEW_HOTSPOT_MIN_PCT = 0.05
ABSENT_HOTSPOT_MAX_PCT = 0.005
FRAME_RE = re.compile(r'<title>(?P<title>.*?)</title>\s*<rect\b(?P<attrs>[^>]*)', re.DOTALL)
TITLE_RE = re.compile(r'^(?P<name>.*) \((?P<samples>[0-9,]+) samples?, (?P<pct>[0-9.]+)%\)$')
ATTR_RE = re.compile(r'([a-zA-Z_:][-a-zA-Z0-9_:.]*)="([^"]*)"')


class AnalysisError(RuntimeError):
	pass


@dataclass
class Cost:
	pct: float = 0.0
	per_io: float = 0.0
	samples: float = 0.0
	seen: int = 0
	run_count: int = 1


@dataclass
class Frame:
	name: str
	samples: int
	pct: float
	x: float
	y: float
	width: float
	index: int
	parent: int | None = None
	is_leaf: bool = False

	@property
	def right(self) -> float:
		return self.x + self.width


@dataclass
class FioSummary:
	path: Path
	fio_version: str = ''
	time: str = ''
	timestamp: int | None = None
	operation: str = ''
	jobname: str = ''
	options: dict[str, str] = field(default_factory=dict)
	metrics: dict[str, float] = field(default_factory=dict)
	percentiles_ns: dict[str, float] = field(default_factory=dict)
	disk_names: list[str] = field(default_factory=list)
	warnings: list[str] = field(default_factory=list)

	def workload_value(self, key: str) -> str:
		if key == 'operation':
			return self.operation
		if key == 'jobname':
			return self.jobname
		return self.options.get(key, '')


@dataclass
class PerfHeader:
	path: Path
	fields: dict[str, str] = field(default_factory=dict)
	warnings: list[str] = field(default_factory=list)


@dataclass
class FlameSummary:
	path: Path
	frame_count: int = 0
	leaf_count: int = 0
	total_event_count: float = 0.0
	leaf_by_name: dict[str, Cost] = field(default_factory=dict)
	inclusive_by_path: dict[tuple[str, ...], Cost] = field(default_factory=dict)
	warnings: list[str] = field(default_factory=list)


@dataclass
class RunAnalysis:
	path: Path
	fio: FioSummary
	flame: FlameSummary | None = None
	perf_header: PerfHeader | None = None
	warnings: list[str] = field(default_factory=list)


@dataclass
class GroupAnalysis:
	input_path: Path
	run_paths: list[Path]
	runs: list[RunAnalysis]
	label: str
	fio: FioSummary
	leaf_by_name: dict[str, Cost]
	inclusive_by_path: dict[tuple[str, ...], Cost]
	warnings: list[str] = field(default_factory=list)

	@property
	def is_version(self) -> bool:
		return len(self.run_paths) > 1


def median(values: Iterable[float]) -> float:
	items = [value for value in values if value is not None and not math.isnan(value)]
	if not items:
		return 0.0
	return float(statistics.median(items))


def pct_delta(base: float, candidate: float) -> float | None:
	if base == 0:
		return None
	return (candidate - base) * 100.0 / base


def parse_float(value: Any, default: float = 0.0) -> float:
	try:
		return float(value)
	except (TypeError, ValueError):
		return default


def parse_int(value: Any, default: int = 0) -> int:
	try:
		return int(value)
	except (TypeError, ValueError):
		return default


def natural_run_key(path: Path) -> tuple[str, int, str]:
	match = re.match(r'^(.*?)(\d+)$', path.name)
	if match:
		return (match.group(1), int(match.group(2)), path.name)
	return (path.name, -1, path.name)


def display_path(path: Path) -> str:
	try:
		return str(path.resolve().relative_to(Path.cwd().resolve()))
	except ValueError:
		return str(path)


def markdown_escape(value: Any) -> str:
	text = str(value)
	text = text.replace('\n', ' ').replace('\r', ' ')
	text = text.replace('|', r'\|')
	return text


def code(value: Any) -> str:
	text = markdown_escape(value)
	text = text.replace('`', "'")
	return f'`{text}`'


def fmt_float(value: float, digits: int = 2) -> str:
	if value is None:
		return 'n/a'
	if math.isnan(value):
		return 'n/a'
	if abs(value) >= 1000:
		return f'{value:,.{digits}f}'
	return f'{value:.{digits}f}'


def fmt_count(value: float) -> str:
	return f'{value:,.0f}'


def fmt_pct(value: float) -> str:
	return f'{value:.2f}%'


def fmt_delta_pct(value: float | None) -> str:
	if value is None:
		return 'n/a'
	return f'{value:+.2f}%'


def fmt_ns(value: float) -> str:
	if abs(value) >= 1_000_000:
		return f'{value / 1_000_000:.3f} ms'
	if abs(value) >= 1_000:
		return f'{value / 1_000:.3f} us'
	return f'{value:.0f} ns'


def fmt_ms(value: float) -> str:
	if abs(value) >= 1000:
		return f'{value / 1000:.3f} s'
	return f'{value:.0f} ms'


def fmt_bytes_per_sec(value: float) -> str:
	units = ('B/s', 'KiB/s', 'MiB/s', 'GiB/s', 'TiB/s')
	scaled = float(value)
	for unit in units:
		if abs(scaled) < 1024 or unit == units[-1]:
			return f'{scaled:.2f} {unit}'
		scaled /= 1024.0
	return f'{value:.2f} B/s'


def format_metric(metric: str, value: float) -> str:
	if metric in ('lat_mean_ns', 'clat_mean_ns', 'slat_mean_ns') or metric.startswith('clat_p'):
		return fmt_ns(value)
	if metric in ('runtime_ms', 'job_runtime_ms'):
		return fmt_ms(value)
	if metric == 'bw_bytes':
		return fmt_bytes_per_sec(value)
	if metric in ('usr_cpu', 'sys_cpu', 'disk_util_pct'):
		return fmt_pct(value)
	if metric in ('iops', 'total_ios', 'ctx'):
		return fmt_count(value)
	return fmt_float(value)


def metric_label(metric: str) -> str:
	labels = {
		'iops': 'IOPS',
		'bw_bytes': 'Bandwidth',
		'total_ios': 'Total IOs',
		'runtime_ms': 'Operation runtime',
		'lat_mean_ns': 'Mean total latency',
		'clat_mean_ns': 'Mean completion latency',
		'slat_mean_ns': 'Mean submission latency',
		'usr_cpu': 'fio user CPU',
		'sys_cpu': 'fio system CPU',
		'ctx': 'fio context switches',
		'job_runtime_ms': 'fio aggregate job runtime',
		'disk_util_pct': 'Disk util',
	}
	if metric.startswith('clat_p'):
		return metric.replace('clat_p', 'Completion latency p')
	return labels.get(metric, metric)


def load_json_file(path: Path) -> Any:
	try:
		with path.open('r', encoding='utf-8') as fp:
			return json.load(fp)
	except json.JSONDecodeError as exc:
		raise AnalysisError(f'failed to parse JSON {display_path(path)}: {exc}') from exc
	except OSError as exc:
		raise AnalysisError(f'failed to read {display_path(path)}: {exc}') from exc


def get_latency_block(operation_stats: dict[str, Any], prefix: str) -> tuple[dict[str, Any], float]:
	for suffix, scale in (('_ns', 1.0), ('_us', 1000.0), ('_ms', 1_000_000.0), ('', 1000.0)):
		block = operation_stats.get(f'{prefix}{suffix}')
		if isinstance(block, dict):
			return block, scale
	return {}, 1.0


def weighted_latency_mean(job_stats: list[dict[str, Any]], prefix: str) -> float:
	total_weight = 0.0
	total_value = 0.0
	for stats in job_stats:
		block, scale = get_latency_block(stats, prefix)
		weight = parse_float(block.get('N'), 0.0)
		if weight <= 0:
			weight = parse_float(stats.get('total_ios'), 0.0)
		mean_value = parse_float(block.get('mean'), 0.0) * scale
		if weight <= 0:
			continue
		total_weight += weight
		total_value += mean_value * weight
	if total_weight == 0:
		return 0.0
	return total_value / total_weight


def collect_percentiles(job_stats: list[dict[str, Any]], prefix: str, warnings: list[str]) -> dict[str, float]:
	values_by_key: dict[str, list[float]] = {}
	for stats in job_stats:
		block, scale = get_latency_block(stats, prefix)
		percentiles = block.get('percentile')
		if not isinstance(percentiles, dict):
			continue
		for key, value in percentiles.items():
			values_by_key.setdefault(key, []).append(parse_float(value) * scale)
	if not values_by_key:
		return {}
	if len(job_stats) > 1:
		warnings.append('fio latency percentiles across multiple jobs are aggregated with per-percentile medians')
	return {key: median(values) for key, values in values_by_key.items()}


def parse_fio(run_dir: Path) -> FioSummary:
	fio_path = run_dir / 'fio.json'
	if not fio_path.exists():
		raise AnalysisError(f'missing required fio.json in {display_path(run_dir)}')
	data = load_json_file(fio_path)
	if not isinstance(data, dict):
		raise AnalysisError(f'fio.json is not a JSON object: {display_path(fio_path)}')

	jobs = data.get('jobs')
	if not isinstance(jobs, list) or not jobs:
		raise AnalysisError(f'fio.json has no jobs: {display_path(fio_path)}')

	warnings: list[str] = []
	op_bytes = {
		op: sum(parse_float(job.get(op, {}).get('io_bytes'), 0.0) for job in jobs if isinstance(job.get(op), dict))
		for op in ACTIVE_OPS
	}
	operation = max(op_bytes, key=op_bytes.get)
	if op_bytes[operation] <= 0:
		raise AnalysisError(f'fio.json has no active read/write/trim IO: {display_path(fio_path)}')

	active_job_stats = [job.get(operation, {}) for job in jobs if isinstance(job.get(operation), dict)]
	active_job_stats = [stats for stats in active_job_stats if parse_float(stats.get('io_bytes'), 0.0) > 0]
	if not active_job_stats:
		raise AnalysisError(f'fio.json selected {operation}, but no active job stats were found: {display_path(fio_path)}')

	global_options = data.get('global options', {})
	if not isinstance(global_options, dict):
		global_options = {}
	options = {str(key): str(value) for key, value in global_options.items()}

	jobname = str(jobs[0].get('jobname', ''))
	iops = sum(parse_float(stats.get('iops'), 0.0) for stats in active_job_stats)
	bw_bytes = sum(parse_float(stats.get('bw_bytes'), 0.0) for stats in active_job_stats)
	total_ios = sum(parse_float(stats.get('total_ios'), 0.0) for stats in active_job_stats)
	io_bytes = sum(parse_float(stats.get('io_bytes'), 0.0) for stats in active_job_stats)
	runtime_ms = max(parse_float(stats.get('runtime'), 0.0) for stats in active_job_stats)

	disk_util = data.get('disk_util', [])
	disk_names: list[str] = []
	disk_util_pct = 0.0
	disk_read_ios = 0.0
	disk_write_ios = 0.0
	disk_in_queue = 0.0
	if isinstance(disk_util, list):
		for disk in disk_util:
			if not isinstance(disk, dict):
				continue
			disk_names.append(str(disk.get('name', '')))
			disk_util_pct = max(disk_util_pct, parse_float(disk.get('util'), 0.0))
			disk_read_ios += parse_float(disk.get('read_ios'), 0.0)
			disk_write_ios += parse_float(disk.get('write_ios'), 0.0)
			disk_in_queue += parse_float(disk.get('in_queue'), 0.0)

	metrics = {
		'io_bytes': io_bytes,
		'iops': iops,
		'bw_bytes': bw_bytes,
		'total_ios': total_ios,
		'runtime_ms': runtime_ms,
		'slat_mean_ns': weighted_latency_mean(active_job_stats, 'slat'),
		'clat_mean_ns': weighted_latency_mean(active_job_stats, 'clat'),
		'lat_mean_ns': weighted_latency_mean(active_job_stats, 'lat'),
		'usr_cpu': sum(parse_float(job.get('usr_cpu'), 0.0) for job in jobs),
		'sys_cpu': sum(parse_float(job.get('sys_cpu'), 0.0) for job in jobs),
		'ctx': sum(parse_float(job.get('ctx'), 0.0) for job in jobs),
		'job_runtime_ms': max(parse_float(job.get('job_runtime'), 0.0) for job in jobs),
		'disk_util_pct': disk_util_pct,
		'disk_read_ios': disk_read_ios,
		'disk_write_ios': disk_write_ios,
		'disk_in_queue': disk_in_queue,
	}

	return FioSummary(
		path=fio_path,
		fio_version=str(data.get('fio version', '')),
		time=str(data.get('time', '')),
		timestamp=parse_int(data.get('timestamp'), None),
		operation=operation,
		jobname=jobname,
		options=options,
		metrics=metrics,
		percentiles_ns=collect_percentiles(active_job_stats, 'clat', warnings),
		disk_names=disk_names,
		warnings=warnings,
	)


def parse_frame_attrs(attrs_text: str) -> dict[str, str]:
	return {match.group(1): html.unescape(match.group(2)) for match in ATTR_RE.finditer(attrs_text)}


def parse_frames(svg_path: Path) -> tuple[list[Frame], list[str]]:
	warnings: list[str] = []
	try:
		text = svg_path.read_text(encoding='utf-8', errors='replace')
	except OSError as exc:
		raise AnalysisError(f'failed to read {display_path(svg_path)}: {exc}') from exc

	frames: list[Frame] = []
	for match in FRAME_RE.finditer(text):
		title = html.unescape(match.group('title')).strip()
		title_match = TITLE_RE.match(title)
		if not title_match:
			continue
		attrs = parse_frame_attrs(match.group('attrs'))
		try:
			x = float(attrs['x'])
			y = float(attrs['y'])
			width = float(attrs['width'])
		except (KeyError, ValueError):
			continue
		name = title_match.group('name')
		samples = int(title_match.group('samples').replace(',', ''))
		pct = float(title_match.group('pct'))
		frames.append(Frame(name=name, samples=samples, pct=pct, x=x, y=y, width=width, index=len(frames)))

	if not frames:
		warnings.append('no flamegraph frames were parsed from SVG')
	return frames, warnings


def estimate_total_event_count(frames: list[Frame]) -> float:
	estimates = []
	for frame in frames:
		if frame.pct >= 0.05:
			estimates.append(frame.samples * 100.0 / frame.pct)
	if not estimates:
		for frame in frames:
			if frame.pct > 0:
				estimates.append(frame.samples * 100.0 / frame.pct)
	if estimates:
		return median(estimates)
	return float(sum(frame.samples for frame in frames))


def mark_geometry(frames: list[Frame]) -> None:
	if not frames:
		return
	levels = sorted(set(frame.y for frame in frames))
	level_index = {level: index for index, level in enumerate(levels)}
	frames_by_y: dict[float, list[Frame]] = {}
	for frame in frames:
		frames_by_y.setdefault(frame.y, []).append(frame)
	for same_level in frames_by_y.values():
		same_level.sort(key=lambda item: (item.x, item.width))

	fudge = 0.0001
	for frame in frames:
		idx = level_index[frame.y]
		if idx > 0:
			child_level = levels[idx - 1]
			children = frames_by_y.get(child_level, [])
			child_xs = [child.x for child in children]
			start = bisect.bisect_left(child_xs, frame.x - fudge)
			frame.is_leaf = True
			for child in children[start:]:
				if child.x > frame.right + fudge:
					break
				if child.right <= frame.right + fudge:
					frame.is_leaf = False
					break
		else:
			frame.is_leaf = True

		if idx >= len(levels) - 1:
			frame.parent = None
			continue
		parent_level = levels[idx + 1]
		parents = frames_by_y.get(parent_level, [])
		best_parent: Frame | None = None
		for parent in parents:
			if parent.x > frame.x + fudge:
				break
			if parent.right + fudge < frame.right:
				continue
			if best_parent is None or parent.width < best_parent.width:
				best_parent = parent
		frame.parent = best_parent.index if best_parent is not None else None


def make_path_getter(frames: list[Frame]):
	cache: dict[int, tuple[str, ...]] = {}

	def get_path(frame_index: int) -> tuple[str, ...]:
		if frame_index in cache:
			return cache[frame_index]
		frame = frames[frame_index]
		if frame.parent is None:
			path = (frame.name,)
		else:
			path = get_path(frame.parent) + (frame.name,)
		cache[frame_index] = path
		return path

	return get_path


def parse_flamegraph(run_dir: Path, fio: FioSummary) -> FlameSummary:
	svg_path = run_dir / 'flamegraph.svg'
	frames, warnings = parse_frames(svg_path)
	total_event_count = estimate_total_event_count(frames)
	mark_geometry(frames)
	total_ios = fio.metrics.get('total_ios', 0.0)
	if total_ios <= 0:
		warnings.append('cannot normalize flamegraph samples by IO because fio total_ios is zero')
		total_ios = 1.0

	leaf_samples: dict[str, int] = {}
	for frame in frames:
		if frame.is_leaf:
			leaf_samples[frame.name] = leaf_samples.get(frame.name, 0) + frame.samples

	leaf_by_name = {
		name: Cost(
			pct=(samples * 100.0 / total_event_count if total_event_count else 0.0),
			per_io=samples / total_ios,
			samples=float(samples),
			seen=1,
			run_count=1,
		)
		for name, samples in leaf_samples.items()
	}

	path_samples: dict[tuple[str, ...], int] = {}
	get_path = make_path_getter(frames)
	for frame in frames:
		path = get_path(frame.index)
		path_samples[path] = path_samples.get(path, 0) + frame.samples

	inclusive_by_path = {
		path: Cost(
			pct=(samples * 100.0 / total_event_count if total_event_count else 0.0),
			per_io=samples / total_ios,
			samples=float(samples),
			seen=1,
			run_count=1,
		)
		for path, samples in path_samples.items()
	}

	return FlameSummary(
		path=svg_path,
		frame_count=len(frames),
		leaf_count=sum(1 for frame in frames if frame.is_leaf),
		total_event_count=total_event_count,
		leaf_by_name=leaf_by_name,
		inclusive_by_path=inclusive_by_path,
		warnings=warnings,
	)


def parse_perf_header(run_dir: Path, timeout: float) -> PerfHeader | None:
	perf_path = run_dir / 'perf.data'
	if not perf_path.exists():
		return None
	perf_bin = shutil.which('perf')
	header = PerfHeader(path=perf_path)
	if perf_bin is None:
		header.warnings.append('perf command not found; perf.data header was not parsed')
		return header
	try:
		result = subprocess.run(
			[perf_bin, 'report', '--stdio', '--header-only', '-i', str(perf_path)],
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE,
			text=True,
			timeout=timeout,
			check=False,
		)
	except subprocess.TimeoutExpired:
		header.warnings.append(f'perf header parse timed out after {timeout:g}s')
		return header
	except OSError as exc:
		header.warnings.append(f'perf header parse failed: {exc}')
		return header

	text = result.stdout + '\n' + result.stderr
	if result.returncode != 0:
		first_line = next((line.strip() for line in text.splitlines() if line.strip()), 'no output')
		header.warnings.append(f'perf header parse exited with {result.returncode}: {first_line}')

	for line in text.splitlines():
		clean = line.strip()
		if clean.startswith('#'):
			clean = clean[1:].strip()
		if not clean:
			continue
		if clean.startswith('event :') and 'name =' in clean:
			match = re.search(r'name = ([^,]+)', clean)
			if match:
				header.fields.setdefault('event', match.group(1).strip())
			continue
		match = re.match(r'([^:]+)\s*:\s*(.*)$', clean)
		if not match:
			continue
		key = match.group(1).strip()
		value = match.group(2).strip()
		if key in (
			'captured on',
			'hostname',
			'os release',
			'perf version',
			'arch',
			'nrcpus online',
			'cpudesc',
			'cmdline',
			'time of first sample',
			'time of last sample',
			'sample duration',
		):
			header.fields[key] = value
	return header


def collect_run_paths(input_path: Path) -> list[Path]:
	if not input_path.exists():
		raise AnalysisError(f'input path does not exist: {display_path(input_path)}')
	if not input_path.is_dir():
		raise AnalysisError(f'input path is not a directory: {display_path(input_path)}')
	if (input_path / 'fio.json').exists():
		return [input_path]
	run_paths = [
		child
		for child in input_path.iterdir()
		if child.is_dir() and child.name.startswith('run_') and (child / 'fio.json').exists()
	]
	run_paths.sort(key=natural_run_key)
	if not run_paths:
		raise AnalysisError(f'{display_path(input_path)} is neither a run directory nor a version directory with run_* children')
	return run_paths


def analyze_run(run_dir: Path, strict: bool, perf_timeout: float) -> RunAnalysis:
	fio = parse_fio(run_dir)
	warnings = list(fio.warnings)

	flame: FlameSummary | None = None
	svg_path = run_dir / 'flamegraph.svg'
	if svg_path.exists():
		flame = parse_flamegraph(run_dir, fio)
		warnings.extend(flame.warnings)
	elif strict:
		raise AnalysisError(f'missing optional flamegraph.svg in strict mode: {display_path(run_dir)}')
	else:
		warnings.append('missing flamegraph.svg; perf hotspot sections will be incomplete')

	perf_header: PerfHeader | None = None
	perf_path = run_dir / 'perf.data'
	if perf_path.exists():
		perf_header = parse_perf_header(run_dir, perf_timeout)
		if perf_header is not None:
			warnings.extend(perf_header.warnings)
	elif strict:
		raise AnalysisError(f'missing optional perf.data in strict mode: {display_path(run_dir)}')
	else:
		warnings.append('missing perf.data; perf capture metadata will be incomplete')

	return RunAnalysis(path=run_dir, fio=fio, flame=flame, perf_header=perf_header, warnings=warnings)


def aggregate_fio(input_path: Path, fios: list[FioSummary], warnings: list[str]) -> FioSummary:
	first = fios[0]
	for key in WORKLOAD_KEYS:
		values = {fio.workload_value(key) for fio in fios}
		if len(values) > 1:
			warnings.append(f'{display_path(input_path)} has varying fio workload field {key}: {", ".join(sorted(values))}')

	metrics: dict[str, float] = {}
	all_metric_keys = set().union(*(fio.metrics.keys() for fio in fios))
	for key in all_metric_keys:
		metrics[key] = median(fio.metrics.get(key, 0.0) for fio in fios)

	percentiles: dict[str, float] = {}
	all_percentiles = set().union(*(fio.percentiles_ns.keys() for fio in fios))
	for key in all_percentiles:
		values = [fio.percentiles_ns[key] for fio in fios if key in fio.percentiles_ns]
		percentiles[key] = median(values)

	options = dict(first.options)
	return FioSummary(
		path=input_path / 'fio.json',
		fio_version=first.fio_version,
		time=first.time,
		timestamp=first.timestamp,
		operation=first.operation,
		jobname=first.jobname,
		options=options,
		metrics=metrics,
		percentiles_ns=percentiles,
		disk_names=first.disk_names,
		warnings=[],
	)


def aggregate_costs(cost_maps: list[dict[Any, Cost]]) -> dict[Any, Cost]:
	if not cost_maps:
		return {}
	run_count = len(cost_maps)
	keys = set().union(*(cost_map.keys() for cost_map in cost_maps))
	aggregated: dict[Any, Cost] = {}
	for key in keys:
		pcts = []
		per_ios = []
		samples = []
		seen = 0
		for cost_map in cost_maps:
			cost = cost_map.get(key)
			if cost is None:
				pcts.append(0.0)
				per_ios.append(0.0)
				samples.append(0.0)
				continue
			pcts.append(cost.pct)
			per_ios.append(cost.per_io)
			samples.append(cost.samples)
			if cost.samples > 0:
				seen += 1
		aggregated[key] = Cost(
			pct=median(pcts),
			per_io=median(per_ios),
			samples=median(samples),
			seen=seen,
			run_count=run_count,
		)
	return aggregated


def analyze_group(input_path: Path, strict: bool, perf_timeout: float) -> GroupAnalysis:
	run_paths = collect_run_paths(input_path)
	runs = [analyze_run(run_path, strict=strict, perf_timeout=perf_timeout) for run_path in run_paths]
	warnings: list[str] = []
	for run in runs:
		for warning in run.warnings:
			warnings.append(f'{display_path(run.path)}: {warning}')

	fio = aggregate_fio(input_path, [run.fio for run in runs], warnings)
	leaf_maps = [run.flame.leaf_by_name for run in runs if run.flame is not None]
	inclusive_maps = [run.flame.inclusive_by_path for run in runs if run.flame is not None]
	if len(leaf_maps) != len(runs):
		warnings.append('one or more runs are missing flamegraph data')

	label = display_path(input_path)
	if len(run_paths) > 1:
		label = f'{label} median({len(run_paths)} runs)'

	return GroupAnalysis(
		input_path=input_path,
		run_paths=run_paths,
		runs=runs,
		label=label,
		fio=fio,
		leaf_by_name=aggregate_costs(leaf_maps),
		inclusive_by_path=aggregate_costs(inclusive_maps),
		warnings=warnings,
	)


def delta_rows(
	base_costs: dict[Any, Cost],
	candidate_costs: dict[Any, Cost],
	key_formatter,
	top: int,
	focus_re: re.Pattern[str] | None = None,
) -> list[dict[str, Any]]:
	rows: list[dict[str, Any]] = []
	for key in set(base_costs) | set(candidate_costs):
		if focus_re is not None and not focus_re.search(key_formatter(key, full=True)):
			continue
		base = base_costs.get(key, Cost())
		candidate = candidate_costs.get(key, Cost())
		delta_per_io = candidate.per_io - base.per_io
		delta_pct_points = candidate.pct - base.pct
		if delta_per_io <= 0 and delta_pct_points <= 0:
			continue
		rows.append(
			{
				'key': key,
				'label': key_formatter(key, full=False),
				'base_pct': base.pct,
				'candidate_pct': candidate.pct,
				'delta_pct_points': delta_pct_points,
				'base_per_io': base.per_io,
				'candidate_per_io': candidate.per_io,
				'delta_per_io': delta_per_io,
				'base_seen': base.seen,
				'candidate_seen': candidate.seen,
				'run_count': max(base.run_count, candidate.run_count),
			}
		)
	rows.sort(key=lambda row: (row['delta_per_io'], row['delta_pct_points'], row['candidate_pct']), reverse=True)
	return rows[:top]


def hotspot_rows(
	base_costs: dict[str, Cost],
	candidate_costs: dict[str, Cost],
	top: int,
	new: bool,
) -> list[dict[str, Any]]:
	rows: list[dict[str, Any]] = []
	for key in set(base_costs) | set(candidate_costs):
		base = base_costs.get(key, Cost())
		candidate = candidate_costs.get(key, Cost())
		if new:
			matches = base.pct <= ABSENT_HOTSPOT_MAX_PCT and candidate.pct >= NEW_HOTSPOT_MIN_PCT
			sort_value = candidate.per_io
		else:
			matches = candidate.pct <= ABSENT_HOTSPOT_MAX_PCT and base.pct >= NEW_HOTSPOT_MIN_PCT
			sort_value = base.per_io
		if not matches:
			continue
		rows.append(
			{
				'label': key,
				'base_pct': base.pct,
				'candidate_pct': candidate.pct,
				'base_per_io': base.per_io,
				'candidate_per_io': candidate.per_io,
				'base_seen': base.seen,
				'candidate_seen': candidate.seen,
				'run_count': max(base.run_count, candidate.run_count),
				'sort_value': sort_value,
			}
		)
	rows.sort(key=lambda row: (row['sort_value'], row['candidate_pct'] if new else row['base_pct']), reverse=True)
	return rows[:top]


def function_key_formatter(key: str, full: bool = False) -> str:
	return key


def path_key_formatter(key: tuple[str, ...], full: bool = False) -> str:
	if full:
		return ' -> '.join(key)
	if len(key) <= 6:
		return ' -> '.join(key)
	return ' -> '.join(('...',) + key[-5:])


def make_table(headers: list[str], rows: list[list[str]]) -> list[str]:
	lines = []
	lines.append('| ' + ' | '.join(headers) + ' |')
	lines.append('| ' + ' | '.join('---' for _ in headers) + ' |')
	for row in rows:
		lines.append('| ' + ' | '.join(row) + ' |')
	return lines


def workload_rows(base: GroupAnalysis, candidate: GroupAnalysis) -> tuple[list[list[str]], list[str]]:
	rows = []
	warnings = []
	for key in WORKLOAD_KEYS:
		base_value = base.fio.workload_value(key)
		candidate_value = candidate.fio.workload_value(key)
		status = 'OK' if base_value == candidate_value else 'DIFF'
		if status == 'DIFF':
			warnings.append(f'workload differs for {key}: base={base_value!r}, candidate={candidate_value!r}')
		rows.append([code(key), code(base_value or '-'), code(candidate_value or '-'), status])
	return rows, warnings


def fio_metric_rows(base: GroupAnalysis, candidate: GroupAnalysis) -> list[list[str]]:
	rows = []
	metrics = dict(base.fio.metrics)
	metrics.update(candidate.fio.metrics)
	for metric in FIO_METRIC_ORDER:
		if metric not in metrics:
			continue
		base_value = base.fio.metrics.get(metric, 0.0)
		candidate_value = candidate.fio.metrics.get(metric, 0.0)
		delta = candidate_value - base_value
		rows.append(
			[
				metric_label(metric),
				format_metric(metric, base_value),
				format_metric(metric, candidate_value),
				format_metric(metric, delta) if metric not in ('usr_cpu', 'sys_cpu', 'disk_util_pct') else f'{delta:+.2f} pp',
				fmt_delta_pct(pct_delta(base_value, candidate_value)),
			]
		)

	for percentile in PERCENTILE_KEYS:
		if percentile not in base.fio.percentiles_ns and percentile not in candidate.fio.percentiles_ns:
			continue
		base_value = base.fio.percentiles_ns.get(percentile, 0.0)
		candidate_value = candidate.fio.percentiles_ns.get(percentile, 0.0)
		delta = candidate_value - base_value
		rows.append(
			[
				metric_label(f'clat_p{percentile.rstrip("0").rstrip(".")}'),
				fmt_ns(base_value),
				fmt_ns(candidate_value),
				fmt_ns(delta),
				fmt_delta_pct(pct_delta(base_value, candidate_value)),
			]
		)
	return rows


def common_field(values: list[str]) -> str:
	non_empty = [value for value in values if value]
	if not non_empty:
		return 'n/a'
	unique = sorted(set(non_empty))
	if len(unique) == 1:
		return unique[0]
	return f'varies ({len(unique)} values)'


def perf_capture_rows(group: GroupAnalysis) -> list[list[str]]:
	headers = [run.perf_header for run in group.runs if run.perf_header is not None]
	flames = [run.flame for run in group.runs if run.flame is not None]
	values = lambda key: [header.fields.get(key, '') for header in headers]
	total_events = median(flame.total_event_count for flame in flames)
	frame_count = median(flame.frame_count for flame in flames)
	leaf_count = median(flame.leaf_count for flame in flames)
	return [
		['Input', code(group.label)],
		['Runs', fmt_count(len(group.run_paths))],
		['Event', markdown_escape(common_field(values('event')))],
		['Host', markdown_escape(common_field(values('hostname')))],
		['Kernel', markdown_escape(common_field(values('os release')))],
		['CPUs online', markdown_escape(common_field(values('nrcpus online')))],
		['Sample duration', markdown_escape(common_field(values('sample duration')))],
		['Flamegraph frames median', fmt_count(frame_count)],
		['Flamegraph leaves median', fmt_count(leaf_count)],
		['Flamegraph event count median', fmt_count(total_events)],
	]


def render_cost_rows(rows: list[dict[str, Any]]) -> list[list[str]]:
	rendered = []
	for row in rows:
		seen = f'{row["base_seen"]}/{row["candidate_seen"]} of {row["run_count"]}'
		rendered.append(
			[
				code(row['label']),
				fmt_pct(row['base_pct']),
				fmt_pct(row['candidate_pct']),
				f'{row["delta_pct_points"]:+.2f} pp',
				fmt_float(row['base_per_io'], 2),
				fmt_float(row['candidate_per_io'], 2),
				fmt_float(row['delta_per_io'], 2),
				seen,
			]
		)
	return rendered


def render_hotspot_rows(rows: list[dict[str, Any]]) -> list[list[str]]:
	rendered = []
	for row in rows:
		seen = f'{row["base_seen"]}/{row["candidate_seen"]} of {row["run_count"]}'
		rendered.append(
			[
				code(row['label']),
				fmt_pct(row['base_pct']),
				fmt_pct(row['candidate_pct']),
				fmt_float(row['base_per_io'], 2),
				fmt_float(row['candidate_per_io'], 2),
				seen,
			]
		)
	return rendered


def add_table_or_none(lines: list[str], headers: list[str], rows: list[list[str]], empty_text: str) -> None:
	if not rows:
		lines.append(empty_text)
		return
	lines.extend(make_table(headers, rows))


def build_report(base: GroupAnalysis, candidate: GroupAnalysis, top: int, focus: str) -> tuple[str, dict[str, Any]]:
	focus_re = re.compile(focus, re.IGNORECASE)
	exclusive_rows = delta_rows(base.leaf_by_name, candidate.leaf_by_name, function_key_formatter, top)
	focused_rows = delta_rows(base.leaf_by_name, candidate.leaf_by_name, function_key_formatter, top, focus_re)
	inclusive_rows = [
		row
		for row in delta_rows(base.inclusive_by_path, candidate.inclusive_by_path, path_key_formatter, top * 4)
		if isinstance(row['key'], tuple) and len(row['key']) > 1
	][:top]
	new_rows = hotspot_rows(base.leaf_by_name, candidate.leaf_by_name, top, new=True)
	disappeared_rows = hotspot_rows(base.leaf_by_name, candidate.leaf_by_name, top, new=False)

	lines: list[str] = []
	lines.append('# Fio/Perf Degradation Report')
	lines.append('')
	lines.append(f'- Base: {code(base.label)}')
	lines.append(f'- Candidate: {code(candidate.label)}')
	lines.append(f'- Aggregation: {"median by run" if base.is_version or candidate.is_version else "single run"}')
	lines.append(f'- Active fio operation: {code(base.fio.operation)} -> {code(candidate.fio.operation)}')
	lines.append(f'- Perf symbols source: {code("flamegraph.svg")}; perf.data is used for capture metadata only')
	lines.append('')

	lines.append('## Workload Compatibility')
	workload_table, workload_warnings = workload_rows(base, candidate)
	lines.extend(make_table(['Field', 'Base', 'Candidate', 'Status'], workload_table))
	lines.append('')

	lines.append('## Fio Regression Summary')
	add_table_or_none(
		lines,
		['Metric', 'Base', 'Candidate', 'Delta', 'Delta %'],
		fio_metric_rows(base, candidate),
		'No fio metrics were available.',
	)
	lines.append('')

	lines.append('## Perf Capture Summary')
	lines.extend(make_table(['Field', 'Base'], perf_capture_rows(base)))
	lines.append('')
	lines.extend(make_table(['Field', 'Candidate'], perf_capture_rows(candidate)))
	lines.append('')

	lines.append('## Top Exclusive Function Regressions')
	add_table_or_none(
		lines,
		[
			'Function',
			'Base %',
			'Candidate %',
			'Delta pp',
			'Base cycles/IO',
			'Candidate cycles/IO',
			'Delta cycles/IO',
			'Seen B/C',
		],
		render_cost_rows(exclusive_rows),
		'No exclusive function regressions were found.',
	)
	lines.append('')

	lines.append('## Top Focused NVMesh/NVMe Regressions')
	focus_display = focus.replace('`', "'")
	lines.append(f'Focus regex: `{focus_display}`')
	add_table_or_none(
		lines,
		[
			'Function',
			'Base %',
			'Candidate %',
			'Delta pp',
			'Base cycles/IO',
			'Candidate cycles/IO',
			'Delta cycles/IO',
			'Seen B/C',
		],
		render_cost_rows(focused_rows),
		'No focused regressions matched the focus regex.',
	)
	lines.append('')

	lines.append('## Top Inclusive Call-Path Regressions')
	add_table_or_none(
		lines,
		[
			'Call path',
			'Base %',
			'Candidate %',
			'Delta pp',
			'Base cycles/IO',
			'Candidate cycles/IO',
			'Delta cycles/IO',
			'Seen B/C',
		],
		render_cost_rows(inclusive_rows),
		'No inclusive call-path regressions were found.',
	)
	lines.append('')

	lines.append('## New Hotspots')
	add_table_or_none(
		lines,
		['Function', 'Base %', 'Candidate %', 'Base cycles/IO', 'Candidate cycles/IO', 'Seen B/C'],
		render_hotspot_rows(new_rows),
		f'No new leaf hotspots above {NEW_HOTSPOT_MIN_PCT:.3f}% were found.',
	)
	lines.append('')

	lines.append('## Disappeared Hotspots')
	add_table_or_none(
		lines,
		['Function', 'Base %', 'Candidate %', 'Base cycles/IO', 'Candidate cycles/IO', 'Seen B/C'],
		render_hotspot_rows(disappeared_rows),
		f'No disappeared leaf hotspots above {NEW_HOTSPOT_MIN_PCT:.3f}% were found.',
	)
	lines.append('')

	all_warnings = workload_warnings + base.warnings + candidate.warnings
	lines.append('## Warnings And Assumptions')
	if all_warnings:
		for warning in all_warnings:
			lines.append(f'- {markdown_escape(warning)}')
	else:
		lines.append('- No warnings.')
	lines.append('- Lower IOPS/bandwidth and higher latency/CPU/cycles-per-IO are treated as regressions.')
	lines.append('- Version directory comparisons use independent medians across available run_* directories.')
	lines.append('- Missing symbols in a version aggregation are counted as zero for that run before taking the median.')
	lines.append('')

	json_report = {
		'base': group_to_json(base),
		'candidate': group_to_json(candidate),
		'fio_deltas': fio_deltas_to_json(base, candidate),
		'top_exclusive_regressions': rows_to_json(exclusive_rows),
		'top_focused_regressions': rows_to_json(focused_rows),
		'top_inclusive_call_path_regressions': rows_to_json(inclusive_rows),
		'new_hotspots': rows_to_json(new_rows),
		'disappeared_hotspots': rows_to_json(disappeared_rows),
		'warnings': all_warnings,
		'assumptions': [
			'flamegraph.svg is the authoritative symbolized perf source',
			'perf.data is used only for capture metadata',
			'version directories are aggregated by median run metrics',
		],
	}
	return '\n'.join(lines), json_report


def rows_to_json(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
	json_rows = []
	for row in rows:
		out = {}
		for key, value in row.items():
			if key == 'key':
				if isinstance(value, tuple):
					out[key] = list(value)
				else:
					out[key] = value
			elif key != 'sort_value':
				out[key] = value
		json_rows.append(out)
	return json_rows


def group_to_json(group: GroupAnalysis) -> dict[str, Any]:
	perf_headers = []
	for run in group.runs:
		if run.perf_header is None:
			continue
		perf_headers.append({'path': str(run.perf_header.path), 'fields': run.perf_header.fields})
	return {
		'input': str(group.input_path),
		'label': group.label,
		'runs': [str(path) for path in group.run_paths],
		'fio': fio_to_json(group.fio),
		'perf_headers': perf_headers,
		'warnings': group.warnings,
	}


def fio_to_json(fio: FioSummary) -> dict[str, Any]:
	return {
		'path': str(fio.path),
		'fio_version': fio.fio_version,
		'time': fio.time,
		'timestamp': fio.timestamp,
		'operation': fio.operation,
		'jobname': fio.jobname,
		'options': fio.options,
		'metrics': fio.metrics,
		'percentiles_ns': fio.percentiles_ns,
		'disk_names': fio.disk_names,
	}


def fio_deltas_to_json(base: GroupAnalysis, candidate: GroupAnalysis) -> dict[str, Any]:
	deltas: dict[str, Any] = {}
	keys = set(base.fio.metrics) | set(candidate.fio.metrics)
	for key in keys:
		base_value = base.fio.metrics.get(key, 0.0)
		candidate_value = candidate.fio.metrics.get(key, 0.0)
		deltas[key] = {
			'base': base_value,
			'candidate': candidate_value,
			'delta': candidate_value - base_value,
			'delta_percent': pct_delta(base_value, candidate_value),
		}
	percentile_deltas = {}
	for key in set(base.fio.percentiles_ns) | set(candidate.fio.percentiles_ns):
		base_value = base.fio.percentiles_ns.get(key, 0.0)
		candidate_value = candidate.fio.percentiles_ns.get(key, 0.0)
		percentile_deltas[key] = {
			'base_ns': base_value,
			'candidate_ns': candidate_value,
			'delta_ns': candidate_value - base_value,
			'delta_percent': pct_delta(base_value, candidate_value),
		}
	deltas['clat_percentiles'] = percentile_deltas
	return deltas


def write_text(path: Path, text: str) -> None:
	try:
		path.parent.mkdir(parents=True, exist_ok=True)
		path.write_text(text, encoding='utf-8')
	except OSError as exc:
		raise AnalysisError(f'failed to write {display_path(path)}: {exc}') from exc


def parse_args(argv: list[str]) -> argparse.Namespace:
	parser = argparse.ArgumentParser(
		description='Compare fio.json, flamegraph.svg and perf.data from two fio/perf run or version directories.',
		formatter_class=argparse.ArgumentDefaultsHelpFormatter,
	)
	parser.add_argument('base', type=Path, help='baseline run_N directory or version directory')
	parser.add_argument('candidate', type=Path, help='candidate run_N directory or version directory')
	parser.add_argument('--top', type=int, default=20, help='number of perf rows to show per section')
	parser.add_argument('--focus', default=DEFAULT_FOCUS_RE, help='regex for focused NVMesh/NVMe hotspot section')
	parser.add_argument('--output', type=Path, help='also write the Markdown report to this path')
	parser.add_argument('--json-out', type=Path, help='write machine-readable metrics to this path')
	parser.add_argument('--strict', action='store_true', help='fail on missing optional flamegraph.svg or perf.data')
	parser.add_argument('--perf-timeout', type=float, default=5.0, help='timeout for perf header parsing per run')
	return parser.parse_args(argv)


def main(argv: list[str]) -> int:
	args = parse_args(argv)
	if args.top <= 0:
		raise AnalysisError('--top must be positive')
	try:
		re.compile(args.focus)
	except re.error as exc:
		raise AnalysisError(f'invalid --focus regex: {exc}') from exc

	base = analyze_group(args.base, strict=args.strict, perf_timeout=args.perf_timeout)
	candidate = analyze_group(args.candidate, strict=args.strict, perf_timeout=args.perf_timeout)
	report, json_report = build_report(base, candidate, top=args.top, focus=args.focus)
	print(report)

	if args.output:
		write_text(args.output, report + '\n')
	if args.json_out:
		write_text(args.json_out, json.dumps(json_report, indent=2, sort_keys=True) + '\n')
	return 0


if __name__ == '__main__':
	try:
		sys.exit(main(sys.argv[1:]))
	except AnalysisError as exc:
		print(f'error: {exc}', file=sys.stderr)
		sys.exit(2)
