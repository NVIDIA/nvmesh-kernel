#!/usr/bin/env python3

"""
Read a highres_histogram JSON from stdin and print an ASCII histogram with percentile summary.

Usage:
    echo '<json>' | python3 highres_hist.py
    # or pipe a full trace line — the script extracts the JSON automatically
"""

from __future__ import annotations

import json
import sys
from typing import TypedDict

MAX_BAR_WIDTH: int = 60
HIGHRES_HISTOGRAM_SHIFT: int = 7


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


def extract_json(text: str) -> str:
	# Find first '{' and match to closing '}'
	start = text.find('{')
	if start < 0:
		raise ValueError('no JSON object found in input')
	depth = 0
	for i in range(start, len(text)):
		if text[i] == '{':
			depth += 1
		elif text[i] == '}':
			depth -= 1
			if depth == 0:
				return text[start : i + 1]
	raise ValueError('unbalanced JSON braces')


def percentile_bin(bins: list[int], total: int, pct: int) -> int:
	threshold = total * pct / 100.0
	cumulative = 0
	for i, count in enumerate(bins):
		cumulative += count
		if cumulative >= threshold:
			return i
	return len(bins) - 1


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


def main() -> None:
	text: str = sys.stdin.read().strip()
	if not text:
		print('error: empty input', file=sys.stderr)
		sys.exit(1)

	raw: str = extract_json(text)
	try:
		data: dict[str, list[Metric]] = json.loads(raw)
	except json.JSONDecodeError as e:
		snippet = raw[:200] + ('...' if len(raw) > 200 else '')
		raise ValueError(f'invalid JSON: {e}; input was: {snippet}') from e

	metrics: list[Metric] = data.get('metrics.all_cpus', [])
	if not metrics:
		print('error: no metrics found in JSON', file=sys.stderr)
		sys.exit(1)

	for m in metrics:
		print_histogram(m)


if __name__ == '__main__':
	main()
