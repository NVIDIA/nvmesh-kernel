#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

"""
Read highres_histogram data from stdin and print an ASCII histogram with percentile summary.

Accepts three input formats (auto-detected):

1. Pager text (--mode msg-stream-txt) line:
	16:44:27... name=wq.wait_time, labels=..., tsc_khz=2400013, max=6395522, bins=[0,66,...]

2. Pager JSON (--mode msg-stream-json), one JSON object per line:
	{"HIGHRES_HISTOGRAM.TSC_KHZ": "2400013", "HIGHRES_HISTOGRAM.HIST_BINS": "[0,66,...]", ...}

3. JDR JSON (procfs), aggregated:
	{"metrics.all_cpus": [{"values": [...], "max_ticks": 123, "tsc_khz": 2400000, ...}]}

4. JDR JSON (procfs), per-CPU:
	{"metrics.cpu0": [...], "metrics.cpu1": [...], ...}

Multiple lines are supported — each trace line or pager JSON line produces one histogram.

Tick values are converted to SI time units (ns, us, ms, s) with appropriate precision
based on magnitude (e.g. 1.2us, 15.3ms, 1.23s).

Usage:
	./pager --mode msg-stream-json \
		--dict_preload 99bin/debug/dict.5.json \
		--log_channels metrics.binlog \
		| nvmesh_highres_hist.py

	cat /proc/nvmeibc/wq_metrics_info | nvmesh_highres_hist.py

	cat /proc/nvmeibc/wq_metrics_pcpu_info | nvmesh_highres_hist.py
"""

from __future__ import annotations

import json
import re
import sys
from typing import TypedDict

MAX_BAR_WIDTH: int = 60
HIGHRES_HISTOGRAM_SHIFT: int = 7

RE_NAME = re.compile(r'\bname=(\S+?)(?:,|$)')
RE_LABELS = re.compile(r'\blabels=(.+?)(?:,\s*tsc_khz=|,\s*max=|,\s*bins=|$)')
RE_TSC = re.compile(r'\btsc_khz=(\d+)')
RE_MAX = re.compile(r'\bmax=(\d+)')
RE_BINS = re.compile(r'\bbins=\[([^\]]*)\]')
RE_METRICS_CPU = re.compile(r'^metrics\.cpu\d+$')


class Metric(TypedDict):
	name: str
	labels: str
	values: list[int]
	max_ticks: int
	tsc_khz: int


def format_time(ns: float) -> str:
	if ns < 1_000:
		return f'{ns:.1f}ns'
	if ns < 1_000_000:
		return f'{ns / 1_000:.1f}us'
	if ns < 1_000_000_000:
		return f'{ns / 1_000_000:.1f}ms'
	return f'{ns / 1_000_000_000:.2f}s'


def ticks_to_ns(ticks: int, tsc_khz: int) -> float:
	if tsc_khz == 0:
		raise ValueError('tsc_khz must be non-zero')
	return float(ticks) * 1_000_000 / tsc_khz


def bin_range_ticks(i: int) -> tuple[int, int]:
	if i == 0:
		return (0, (1 << (HIGHRES_HISTOGRAM_SHIFT + 1)) - 1)
	lo = 1 << (i + HIGHRES_HISTOGRAM_SHIFT)
	hi = (1 << (i + HIGHRES_HISTOGRAM_SHIFT + 1)) - 1
	return (lo, hi)


def percentile_bin(bins: list[int], total: int, pct: int) -> int:
	threshold = total * pct / 100.0
	cumulative = 0
	for i, count in enumerate(bins):
		cumulative += count
		if cumulative >= threshold:
			return i
	return len(bins) - 1


def parse_trace_text_line(line: str) -> Metric | None:
	"""Parse a binary trace text line like:
	16:44:27... name=wq.wait_time, labels=module=nvmeibc;reason=resubmit, tsc_khz=2400013, max=6395522, bins=[0,66,...]
	"""
	name_m = RE_NAME.search(line)
	labels_m = RE_LABELS.search(line)
	tsc_m = RE_TSC.search(line)
	max_m = RE_MAX.search(line)
	bins_m = RE_BINS.search(line)

	if not (tsc_m and max_m and bins_m):
		return None

	bins_str = bins_m.group(1)
	values = [int(x) for x in bins_str.split(',') if x.strip()] if bins_str.strip() else []

	return {
		'name': name_m.group(1) if name_m else '?',
		'labels': labels_m.group(1).rstrip(', ') if labels_m else '',
		'values': values,
		'max_ticks': int(max_m.group(1)),
		'tsc_khz': int(tsc_m.group(1)),
	}


def parse_pager_json(obj: dict[str, str]) -> Metric | None:
	"""Parse pager --mode msg-stream-json output like:
	{"HIGHRES_HISTOGRAM.TSC_KHZ": "2400013", "HIGHRES_HISTOGRAM.HIST_BINS": "[0,66,...]", ...}
	"""
	tsc_str = obj.get('HIGHRES_HISTOGRAM.TSC_KHZ', '')
	max_str = obj.get('HIGHRES_HISTOGRAM.MAX_TICKS', '')
	bins_str = obj.get('HIGHRES_HISTOGRAM.HIST_BINS', '')
	name = obj.get('HIGHRES_HISTOGRAM.METRIC_NAME', '?')
	labels = obj.get('HIGHRES_HISTOGRAM.METRIC_LABELS', '')

	if not (tsc_str and max_str and bins_str):
		return None

	values = json.loads(bins_str)

	return {
		'name': name,
		'labels': labels,
		'values': values,
		'max_ticks': int(max_str),
		'tsc_khz': int(tsc_str),
	}


def print_histogram(metric: Metric) -> None:
	name: str = metric.get('name', '?')
	labels: str = metric.get('labels', '')

	required_fields = {'values': list, 'max_ticks': int, 'tsc_khz': int}
	missing = [f for f in required_fields if f not in metric]
	if missing:
		raise ValueError(f'metric is missing required field(s): {", ".join(missing)}')
	bad_type = [
		f'{f} (expected {t.__name__}, got {type(metric[f]).__name__})'
		for f, t in required_fields.items()
		if not isinstance(metric[f], t)
	]
	if bad_type:
		raise ValueError(f'metric has invalid field type(s): {", ".join(bad_type)}')

	bins: list[int] = metric['values']
	max_ticks: int = metric['max_ticks']
	tsc_khz: int = metric['tsc_khz']

	total: int = sum(bins)
	if total == 0:
		print(f'{name} ({labels}): no samples')
		return

	peak: int = max(bins)
	max_ticks_str: str = format_time(ticks_to_ns(max_ticks, tsc_khz))

	print(f'  {name}  {labels}')
	print(f'  samples: {total}   max: {max_ticks_str} ({max_ticks} ticks)   tsc_khz: {tsc_khz}')
	print()

	# print histogram bars
	for i, count in enumerate(bins):
		if count == 0 and i > 0:
			# skip leading/trailing zeros, but show interior zeros
			all_before_zero = all(b == 0 for b in bins[:i])
			all_after_zero = all(b == 0 for b in bins[i:])
			if all_before_zero or all_after_zero:
				continue

		_, hi_ticks = bin_range_ticks(i)
		hi_ns: float = ticks_to_ns(hi_ticks, tsc_khz)

		if i == 0:
			label = f'{"0":>10s}'
		else:
			label = f'{format_time(hi_ns):>10s}'

		bar_len: int = int(count / peak * MAX_BAR_WIDTH) if peak > 0 else 0
		bar: str = '#' * bar_len
		pct: float = count / total * 100

		print(f'  {label} |{bar:<{MAX_BAR_WIDTH}s}| {count:>7d}  ({pct:5.1f}%)')

	print()

	# print percentiles
	print('  Percentiles:')
	for p in [50, 75, 90, 95, 99, 100]:
		if p == 100:
			print(f'    p{p:<3d}  {max_ticks_str}')
		else:
			bi: int = percentile_bin(bins, total, p)
			_, hi_ticks = bin_range_ticks(bi)
			print(f'    p{p:<3d}  <= {format_time(ticks_to_ns(hi_ticks, tsc_khz))}')
	print()


def _try_jdr_json(obj: dict[str, list[Metric]]) -> bool:
	"""Try to print histograms from a JDR JSON object. Returns True if handled."""
	if 'metrics.all_cpus' in obj:
		for m in obj['metrics.all_cpus']:
			print_histogram(m)
		return True

	cpu_keys = sorted((k for k in obj if RE_METRICS_CPU.match(k)), key=lambda k: int(k.split('cpu')[1]))
	if cpu_keys:
		for key in cpu_keys:
			print(f'--- {key} ---')
			for m in obj[key]:
				print_histogram(m)
		return True

	return False


def process_line(line: str) -> None:
	"""Process a single pager JSON or trace text line."""
	if line.startswith('{'):
		try:
			obj = json.loads(line)
		except json.JSONDecodeError:
			return

		# Single-line JDR JSON
		if _try_jdr_json(obj):
			return

		# Pager JSON
		if 'HIGHRES_HISTOGRAM.HIST_BINS' in obj:
			m = parse_pager_json(obj)
			if m:
				print_histogram(m)
			return

	# Trace text line
	m = parse_trace_text_line(line)
	if m:
		print_histogram(m)


def main() -> None:
	# Peek at first non-empty line to detect format
	first_line = ''
	for line in sys.stdin:
		first_line = line.strip()
		if first_line:
			break

	if not first_line:
		print('error: empty input', file=sys.stderr)
		sys.exit(1)

	# Multiline JDR JSON starts with a single '{'
	if first_line == '{':
		text = first_line + '\n' + sys.stdin.read()
		obj = json.loads(text)
		if isinstance(obj, dict) and _try_jdr_json(obj):
			return
		return

	# Stream line-by-line for pager JSON and trace text
	process_line(first_line)
	for line in sys.stdin:
		line = line.strip()
		if line:
			process_line(line)


if __name__ == '__main__':
	main()
