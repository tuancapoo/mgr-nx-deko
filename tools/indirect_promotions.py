#!/usr/bin/env python3
"""Makes app/indirect_promotions.txt (the list tools/indirect_calls.py --promote applies) from a census of the
game's indirect calls (docs/enhancements.md, "The second CPU round of 2026-10-12", candidate 6).

The census is the file a PC run writes with sa2_diag_indirect_census = "<file>" (app/src/cpu/sa2_codegen_checks.cpp):
one line per call site, `<function> <site> <calls> <target>:<calls> ... other:<calls>`. A site is promoted when
  - it took at least --min-share of all indirect calls of the run (default 0.02 %: a guard on a site that hardly
    runs buys nothing and costs a compare), and
  - its most frequent targets cover it: one target with at least --min-single of its calls (default 99.5 %), or
    the two or three most frequent together with at least that much (each further one at least --min-next,
    default 5 %, of the site), or, failing that, one target with at least --min-dominant (default 70 %): the rest
    then pays one compare more than before, which the guarded majority outweighs, and
  - those targets are functions of the generated tree (a call into another module or a kernel thunk stays indirect).
A guard compares the target, so a site that goes elsewhere in a scene the census did not see is still correct: it
takes the old path, a compare or two later. The list therefore only has to be good, not complete.

  python tools/indirect_promotions.py --census out/pc/indirect_census.txt --gen app/generated/default
                                      [--out app/indirect_promotions.txt] [--min-share 0.02] [--min-single 99.5]
Several --census files are added up (attract loop, a stage, a two-player race ...).
"""
import argparse
import os
import re
import sys

FUNC = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{$', re.M)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--census', required=True, nargs='+')
    ap.add_argument('--gen', required=True)
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'app',
                                                  'indirect_promotions.txt'))
    ap.add_argument('--min-share', type=float, default=0.02, help='percent of all indirect calls')
    ap.add_argument('--min-single', type=float, default=99.5, help='percent of the site its targets must cover')
    ap.add_argument('--min-next', type=float, default=5.0, help='percent of the site a second or third target needs')
    ap.add_argument('--min-dominant', type=float, default=70.0,
                    help='percent of the site one target needs to be guarded alone when the others are many')
    a = ap.parse_args()
    defined = set()
    for f in os.listdir(a.gen):
        if re.match(r'\w+_recomp\.\d+\.cpp$', f):
            defined.update(FUNC.findall(open(os.path.join(a.gen, f), encoding='utf-8').read()))
    sites = {}  # (function, site) -> [total, {target: calls}, other]
    for path in a.census:
        for line in open(path, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            parts = line.split()
            key = (parts[0].upper(), int(parts[1]))
            s = sites.setdefault(key, [0, {}, 0])
            s[0] += int(parts[2])
            for p in parts[3:]:
                name, _, count = p.partition(':')
                if name == 'other':
                    s[2] += int(count)
                else:
                    s[1][name.upper()] = s[1].get(name.upper(), 0) + int(count)
    total = sum(s[0] for s in sites.values())
    if not total:
        sys.exit('indirect_promotions: the census has no calls')
    rows = []  # (calls, key, [targets], calls the guards catch)
    single_sites = single_calls = foreign = cold = mixed = chained = dominant = 0
    for key, (calls, targets, _) in sites.items():
        ranked = sorted(targets.items(), key=lambda kv: -kv[1])
        target, top = ranked[0]
        chosen, caught = None, 0
        if 100.0 * top >= a.min_single * calls:
            chosen, caught = [target], top
            single_sites += 1
            single_calls += calls
        else:
            for n in (2, 3):
                part = ranked[:n]
                if len(part) == n and 100.0 * sum(c for _, c in part) >= a.min_single * calls and \
                        all(100.0 * c >= a.min_next * calls for _, c in part[1:]):
                    chosen, caught = [t for t, _ in part], sum(c for _, c in part)
                    break
            if chosen is None and 100.0 * top >= a.min_dominant * calls:
                chosen, caught = [target], top
        if chosen is None:
            mixed += calls
            continue
        if any(t not in defined for t in chosen):
            foreign += calls
            continue
        if 100.0 * calls < a.min_share * total:
            cold += calls
            continue
        chained += len(chosen) > 1
        dominant += len(chosen) == 1 and 100.0 * top < a.min_single * calls
        rows.append((calls, key, chosen, caught))
    rows.sort(key=lambda r: (-r[0], r[1]))
    promoted = sum(r[3] for r in rows)
    lines = ['# Indirect call sites that go to one target: tools/indirect_calls.py --promote turns each into a guarded',
             '# direct call. Written by tools/indirect_promotions.py from a PC census (sa2_diag_indirect_census); the',
             '# guard compares the target, so the list only has to be good, not complete.',
             '# Census: %d sites, %d calls. Promoted: %d sites whose guards catch %.1f %% of the calls (%d of them with'
             % (len(sites), total, len(rows), 100.0 * promoted / total, chained),
             '# two or three targets, %d with one dominant target of at least %.0f %%). Not promoted: %.1f %% of the'
             % (dominant, a.min_dominant, 100.0 * mixed / total),
             '# calls in sites with many targets, %.1f %% to targets outside the generated tree, %.1f %% in sites below'
             % (100.0 * foreign / total, 100.0 * cold / total),
             '# %.2f %% of all calls.' % a.min_share,
             '# <function> <site> <target> [<target2> [<target3>]]   # calls in the census, share the guards catch']
    for calls, (function, site), chosen, caught in sorted(rows, key=lambda r: r[1]):
        lines.append('%s %d %s   # %d, %.2f %%' % (function, site, ' '.join(chosen), calls, 100.0 * caught / calls))
    open(a.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    print('indirect_promotions: %d sites and %d calls in the census; %d sites always or nearly always go to one target '
          '(%.1f %% of the calls); promoted %d sites (%d with two or three targets, %d with a dominant one) whose '
          'guards catch %.1f %% of the calls -> %s' %
          (len(sites), total, single_sites, 100.0 * single_calls / total, len(rows), chained, dominant,
           100.0 * promoted / total, a.out))
    for calls, (function, site), chosen, caught in rows[:12]:
        print('  %s site %d -> %s  %d calls (%.2f %% of all), %.2f %% caught' %
              (function, site, ' '.join(chosen), calls, 100.0 * calls / total, 100.0 * caught / calls))


if __name__ == '__main__':
    main()
