#!/usr/bin/env python3
"""Flush-mode switches and runtime calls of a census run, per Swap and per draw.

A run with sa2_diag_runtime_census (PC builds) logs every 10 s
    [census] guest code per second: N flush-mode switches ...
    [census] runtime calls per second: guest clock N ...; critical sections entered N ...; events set N, ...
and the renderer logs `[native] 10.0 s: N Swaps, N draws ...` on its own 10 s clock. A lockstep run does not keep
real time, so "per second" says little there: this script adds up the reports of a time span and divides by the
Swaps and draws of the same span.

  python tools/census_summary.py out/pc/logs/sa2_300.log [--from-s 60] [--to-s 150] [--capture]
--from-s / --to-s: seconds since the first log line (default: the whole run).
--capture: the span of the run's lockstep capture (from its "[capture] recording" line to its "[capture] video"
line): the same game frames in every run of the same recording, so two builds can be compared.
"""
import argparse
import datetime
import re
import sys

STAMP = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\]')
FLUSH = re.compile(r'\[census\] guest code per second: (\d+) flush-mode switches')
CALLS = re.compile(r'\[census\] (?:runtime calls|any-mode float helpers) per second: (.*)$')
NATIVE = re.compile(r'\[native\] (\d+\.\d+) s: (\d+) Swaps, (\d+) draws')
MISMATCH = re.compile(r'flush-mode flow.*?(\d+) mismatch')
NUMBER = re.compile(r'([a-z][a-z ]*?) (\d+)(?: \(|[,;]|$)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--from-s', type=float, default=0.0)
    ap.add_argument('--to-s', type=float, default=1e9)
    ap.add_argument('--period', type=float, default=10.0, help='seconds between two census reports')
    ap.add_argument('--capture', action='store_true', help='only the span of the lockstep capture')
    a = ap.parse_args()
    if a.capture:
        start = end = first = None
        for line in open(a.log, encoding='utf-8', errors='replace'):
            m = STAMP.match(line)
            if not m:
                continue
            t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f')
            if first is None:
                first = t
            if '[capture] recording' in line and start is None:
                start = (t - first).total_seconds()
            if '[capture] video' in line:
                end = (t - first).total_seconds()
        if start is None or end is None:
            sys.exit('no complete capture in this log')
        a.from_s, a.to_s = start, end
    first = None
    switches = 0.0
    reports = 0
    swaps = draws = 0
    native_s = 0.0
    calls = {}
    for line in open(a.log, encoding='utf-8', errors='replace'):
        m = STAMP.match(line)
        if not m:
            continue
        t = datetime.datetime.strptime(m.group(1), '%Y-%m-%d %H:%M:%S.%f')
        if first is None:
            first = t
        at = (t - first).total_seconds()
        if at < a.from_s or at > a.to_s:
            continue
        m = FLUSH.search(line)
        if m:
            switches += int(m.group(1)) * a.period
            reports += 1
            continue
        m = CALLS.search(line)
        if m:
            for name, value in NUMBER.findall(m.group(1)):
                calls[name.strip()] = calls.get(name.strip(), 0.0) + int(value) * a.period
            continue
        m = NATIVE.search(line)
        if m:
            native_s += float(m.group(1))
            swaps += int(m.group(2))
            draws += int(m.group(3))
    if not reports or not swaps:
        sys.exit('no census reports or no Swaps in that span')
    span = reports * a.period
    print(f'{a.log}: {reports} census reports ({span:.0f} s), {swaps} Swaps and {draws} draws in {native_s:.0f} s of '
          f'renderer reports')
    # The two report clocks are not the same thread: scale the Swaps and draws to the census span.
    scale = span / native_s if native_s else 1.0
    s, d = swaps * scale, draws * scale
    print(f'  flush-mode switches: {switches / span:,.0f} per second, {switches / s:,.0f} per Swap, '
          f'{switches / d:.2f} per draw')
    for name, total in calls.items():
        print(f'  {name}: {total / span:,.0f} per second, {total / s:,.1f} per Swap')


if __name__ == '__main__':
    main()
