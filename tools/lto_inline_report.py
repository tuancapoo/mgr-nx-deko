#!/usr/bin/env python3
"""What the inliner of an LTO + PGO link decided, in the profile's own numbers, and what it cost in hot code.

The Switch build writes the inputs when it is configured with -DSA2_SWITCH_LTO_DUMP=ON (app/CMakeLists.txt):
  out/nx/sa2.wpa.<N>i.inline   GCC's dump of the link's inlining pass (-fdump-ipa-inline). It ends with the calls
                               inlined and not inlined, counted as often as they ran in the profile's session, the
                               reason each remaining call site was refused, GCC's estimate of the program's time
                               weighted by the profile (before and after inlining), and one summary per function
                               that lists the copies inlined into it;
  out/nx/sa2.map               the link map: the functions GCC placed in .text.hot.* are the profile's hot code.

  python tools/lto_inline_report.py [--build out/nx] [--save NAME] [--against NAME]

--save NAME     also writes the numbers to out/lto_stats/NAME.json (the next link overwrites the dump and the map).
--against NAME  compares with a saved report: the time estimate, the calls inlined, the same hot functions measured
                in both builds, and the copies this build added, by how many copies the callee now has. A callee
                with one copy has moved into its only hot caller; one with ten copies is ten times in the
                instruction cache.

Use: to judge a link-time setting (SA2_SWITCH_INLINE_AUTO, SA2_SWITCH_INLINE_UNIT_GROWTH, SA2_SWITCH_LTO_PARTITIONS)
without a console. The estimate knows nothing about caches: on the Cortex-A57 the growth of the hot code is the
other half of the judgement, which is why both are printed side by side.
"""
import argparse
import collections
import glob
import json
import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEVKITA64 = next((d + '/devkitA64/bin/' for d in (os.environ.get('DEVKITPRO', ''), 'C:/devkitPro', '/opt/devkitpro')
                  if d and os.path.isdir(d + '/devkitA64/bin')), 'C:/devkitPro/devkitA64/bin/')
EXE = '.exe' if os.name == 'nt' else ''
GAME = re.compile(r'^(?:__imp__)?sub_[0-9A-F]{8}')
REASON = re.compile(r'^(.*?)\s*:\s+(\d+) calls, [\d.]+ freq, (\d+) count$')
ESTIMATE = re.compile(r'^Overall time estimate: ([\d.]+) weighted by profile: ([\d.]+)')
CALL = re.compile(r'^\s+(\S+)/\d+ inlined$')
BUCKETS = ('1 copy', '2-3 copies', '4-9 copies', '10 or more')


def bucket(n):
    return BUCKETS[0] if n == 1 else BUCKETS[1] if n <= 3 else BUCKETS[2] if n <= 9 else BUCKETS[3]


def read_dump(path):
    out = {'estimates': [], 'reasons': {}, 'self_size': {}, 'copies': collections.Counter()}
    stage = 0                                  # 1: the table of reasons, 2: the summaries after inlining
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if stage == 2:
                if line.startswith('IPA function summary for '):
                    cur = line.split()[4].rsplit('/', 1)[0]
                elif line.startswith('  self size:') and cur:
                    out['self_size'][cur] = int(line.split()[2])
                else:
                    m = CALL.match(line.rstrip('\n'))
                    if m:
                        out['copies'][m.group(1)] += 1
                continue
            m = ESTIMATE.match(line)
            if m:
                out['estimates'].append(float(m.group(2)))
            elif line.startswith('Inlined ') and ' + speculative ' in line:
                out['inlined'] = int(line.split()[1])
            elif line.startswith('Not inlined '):
                out['not_inlined'] = int(line.split()[2])
                out['still_indirect'] = sum(int(x) for x in re.findall(r'still indirect(?: polymorphic)? (\d+)', line))
            elif line.startswith('Why inlining failed?'):
                stage = 1
            elif stage == 1:
                if line.startswith('IPA function summary for '):
                    stage = 2
                    cur = line.split()[4].rsplit('/', 1)[0]
                    continue
                m = REASON.match(line.rstrip('\n'))
                if m and m.group(1):
                    out['reasons'][m.group(1)] = [int(m.group(2)), int(m.group(3))]
    return out


def sizes(elf):
    text = subprocess.run([DEVKITA64 + 'aarch64-none-elf-nm' + EXE, '-S', elf], capture_output=True, text=True,
                          errors='replace').stdout
    table = {}
    for line in text.split('\n'):
        parts = line.split()
        if len(parts) == 4 and parts[2] in 'tT':
            table[parts[3]] = int(parts[1], 16)
    return table


def body_calls(r):
    return r['not_inlined'] - r['reasons'].get('function body not available', [0, 0])[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--build', default=os.path.join(ROOT, 'out', 'nx'))
    ap.add_argument('--save')
    ap.add_argument('--against')
    args = ap.parse_args()
    stats_dir = os.path.join(ROOT, 'out', 'lto_stats')

    dumps = glob.glob(os.path.join(args.build, '*.wpa.*i.inline'))
    if not dumps:
        raise SystemExit('no *.wpa.*i.inline in %s: configure with -DSA2_SWITCH_LTO_DUMP=ON and link' % args.build)
    r = read_dump(dumps[0])
    elf = os.path.join(args.build, 'sa2')
    table = sizes(elf)
    r['functions'] = table
    r['game_bytes'] = sum(v for k, v in table.items() if GAME.match(k) and not k.startswith('sub_'))
    section = subprocess.run([DEVKITA64 + 'aarch64-none-elf-size' + EXE, '-A', elf], capture_output=True,
                             text=True).stdout
    r['text_bytes'] = int(re.search(r'^\.text\s+(\d+)', section, re.M).group(1))
    map_path = os.path.join(args.build, 'sa2.map')
    r['hot'] = sorted(set(re.findall(r'^ \.text\.hot\.(\S+)', open(map_path, encoding='utf-8',
                                                                 errors='replace').read(), re.M)))

    body = body_calls(r)
    print('time estimate weighted by the profile: %.4g before inlining, %.4g after (%.1f %%)' % (
        r['estimates'][0], r['estimates'][-1], 100.0 * (r['estimates'][-1] / r['estimates'][0] - 1)))
    print('calls as often as they ran: %.3g inlined, %.3g not inlined that have a body (%.1f %% inlined), '
          '%.3g indirect' % (r['inlined'], body, 100.0 * r['inlined'] / max(1, r['inlined'] + body),
                             r['still_indirect']))
    print('call sites refused, by reason (sites, calls in the session):')
    for reason, (sites, count) in sorted(r['reasons'].items(), key=lambda kv: -kv[1][1]):
        if reason != 'function body not available':
            print('  %-48s %7d  %14d' % (reason, sites, count))
    hot_bytes = sum(table.get(n, 0) for n in r['hot'])
    print('.text %d bytes; game functions %d bytes; hot functions (.text.hot): %d, %d bytes' % (
        r['text_bytes'], r['game_bytes'], len(r['hot']), hot_bytes))
    print('inlined copies: %d of %d functions' % (sum(r['copies'].values()), len(r['copies'])))

    if args.against:
        b = json.load(open(os.path.join(stats_dir, args.against + '.json')))
        both = sorted(set(r['hot']) | set(b.get('hot', [])))
        mine = sum(table.get(n, 0) for n in both)
        theirs = sum(b['functions'].get(n, 0) for n in both)
        print('\nagainst %s:' % args.against)
        print('  time estimate after inlining: %+.2f %%' % (100.0 * (r['estimates'][-1] / b['estimates'][-1] - 1)))
        print('  calls not inlined that have a body: %.3g -> %.3g (%+.1f %%)' % (
            body_calls(b), body, 100.0 * (body / max(1, body_calls(b)) - 1)))
        print('  the %d functions hot in either build: %d -> %d bytes (%+.1f %%)' % (
            len(both), theirs, mine, 100.0 * (mine / max(1, theirs) - 1)))
        print('  .text: %d -> %d bytes (%+.1f %%); game functions: %+.1f %%' % (
            b['text_bytes'], r['text_bytes'], 100.0 * (r['text_bytes'] / b['text_bytes'] - 1),
            100.0 * (r['game_bytes'] / b['game_bytes'] - 1)))
        if b.get('copies'):
            size = dict(b.get('self_size', {}))
            size.update(r['self_size'])
            rows = collections.OrderedDict((k, [0, 0, 0]) for k in BUCKETS)
            for callee, n in r['copies'].items():
                added = n - b['copies'].get(callee, 0)
                if added > 0:
                    row = rows[bucket(n)]
                    row[0] += 1
                    row[1] += added
                    row[2] += added * size.get(callee, 0)
            total = sum(row[2] for row in rows.values())
            print('  copies added, by how many copies the callee now has (callees, copies, size units):')
            for k, (callees, copies, units) in rows.items():
                print('    %-11s %6d %7d %9d  (%.0f %%)' % (k, callees, copies, units, 100.0 * units / max(1, total)))
    if args.save:
        os.makedirs(stats_dir, exist_ok=True)
        json.dump(r, open(os.path.join(stats_dir, args.save + '.json'), 'w'))
        print('saved: out/lto_stats/%s.json' % args.save)


if __name__ == '__main__':
    main()
