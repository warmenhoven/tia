#!/usr/bin/env python3
"""
Diff two Sim2600-format NDJSON traces, typically one from
run_sim2600.py (gold) and one from run_tia_c (our implementation).

Pairs records by hc (TIA half-clock count) and reports the first clock
at which the two sides disagree on the compared fields. Fields checked
by default: vsync, vblank, wsync, lum, col (everything that is a direct
observable of TIA state). rgba is skipped because the two sides use
different palette tables / alpha conventions. rdy is skipped by default
because WSYNC-release timing may reasonably differ by one cycle across
models.

Usage:
  compare_traces.py --gold gold.ndjson --ours ours.ndjson
  compare_traces.py ... --fields vsync,vblank,lum,col,wsync
  compare_traces.py ... --skip-until 20000    # ignore reset noise
  compare_traces.py ... --max-mismatches 0    # stop at first mismatch
"""

import argparse
import json
import sys

DEFAULT_FIELDS = ('vsync', 'vblank', 'wsync', 'lum', 'col')


def load_trace(path):
    records = {}
    header = None
    with open(path) as fp:
        for lineno, line in enumerate(fp, 1):
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            if header is None:
                header = obj
                continue
            hc = obj.get('hc')
            if hc is None:
                sys.exit('%s:%d missing "hc"' % (path, lineno))
            records[hc] = obj
    if header is None:
        sys.exit('%s: empty trace file' % path)
    return header, records


def fmt_rec(r, fields):
    if r is None:
        return '(no record at this hc)'
    return ', '.join('%s=%s' % (f, r.get(f, '?')) for f in fields)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gold', required=True,
                    help='Reference trace (Sim2600 oracle).')
    ap.add_argument('--ours', required=True,
                    help='Trace from run_tia_c (our implementation).')
    ap.add_argument('--fields',
                    default=','.join(DEFAULT_FIELDS),
                    help='Comma-separated list of fields to compare.')
    ap.add_argument('--skip-until', type=int, default=0,
                    help='Ignore all records with hc < this value.')
    ap.add_argument('--max-mismatches', type=int, default=0,
                    help='Stop after N mismatches (0 = report all).')
    args = ap.parse_args()

    fields = tuple(f.strip() for f in args.fields.split(',') if f.strip())

    gh, gold = load_trace(args.gold)
    oh, ours = load_trace(args.ours)

    # Soft-check common header fields. Differing sources are expected
    # (one is tia.c, the other is sim2600). halfclocks/every should match
    # so we're comparing apples to apples.
    for key in ('halfclocks', 'every'):
        if gh.get(key) != oh.get(key):
            print('WARN: header %s differs (gold=%s ours=%s)' %
                  (key, gh.get(key), oh.get(key)), file=sys.stderr)

    common_hcs = sorted(set(gold) & set(ours))
    gold_only  = sorted(set(gold) - set(ours))
    ours_only  = sorted(set(ours) - set(gold))
    if gold_only or ours_only:
        print('NOTE: %d hc(s) only in gold, %d only in ours '
              '(first gold-only=%s, first ours-only=%s)' %
              (len(gold_only), len(ours_only),
               gold_only[0] if gold_only else '-',
               ours_only[0] if ours_only else '-'),
              file=sys.stderr)

    compared = 0
    matched  = 0
    mismatches = []
    for hc in common_hcs:
        if hc < args.skip_until:
            continue
        compared += 1
        g = gold[hc]
        o = ours[hc]
        differ = [f for f in fields if g.get(f) != o.get(f)]
        if differ:
            mismatches.append((hc, differ, g, o))
            if args.max_mismatches and len(mismatches) >= args.max_mismatches:
                break
        else:
            matched += 1

    print('compared %d records, %d matched, %d mismatches' %
          (compared, matched, len(mismatches)))

    if not mismatches:
        print('OK — no divergence on fields: %s' % ', '.join(fields))
        return 0

    # Print up to the first 5 mismatches in human form.
    for hc, differ, g, o in mismatches[:args.max_mismatches or 5]:
        print('\nhc=%d  fields=[%s]' % (hc, ','.join(differ)))
        print('  gold: %s' % fmt_rec(g, fields))
        print('  ours: %s' % fmt_rec(o, fields))

    shown = args.max_mismatches or 5
    if len(mismatches) > shown:
        print('\n... %d more mismatches not shown' % (len(mismatches) - shown))

    return 1


if __name__ == '__main__':
    sys.exit(main())
