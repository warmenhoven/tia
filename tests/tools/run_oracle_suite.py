#!/usr/bin/env python3
"""
Run every test ROM in tests/oracle/test_roms/ through both the Sim2600
oracle (csim if present, falls back to Python) and our run_tia_c, then
diff the traces and emit a per-ROM report.

Gold traces are cached under tmp/oracle/<rom-stem>_gold.ndjson. Cache is
invalidated when the ROM is newer than the cached gold file.

Usage:
  run_oracle_suite.py                  # default: 20000 halfclocks each
  run_oracle_suite.py --halfclocks N   # override trace length
  run_oracle_suite.py --rom NAME       # run just one ROM
  run_oracle_suite.py --rebuild-gold   # force csim re-run
  run_oracle_suite.py --fields vsync,vblank,lum,col  # compare fields

Exit status:
  0  — every ROM matched (0 mismatches across the suite)
  1  — at least one ROM has oracle divergence
  2  — usage / setup error (no ROMs, tools missing, etc.)
"""

import argparse
import glob
import json
import os
import subprocess
import sys

HERE     = os.path.dirname(os.path.abspath(__file__))
TIA_ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ROM_DIR  = os.path.join(TIA_ROOT, 'tests/oracle/test_roms')
TMP_DIR  = os.path.join(TIA_ROOT, 'tmp/oracle/suite')
RUN_SIM  = os.path.join(HERE, 'run_sim2600.py')
RUN_TIA  = os.path.join(HERE, 'run_tia_c')
COMPARE  = os.path.join(HERE, 'compare_traces.py')

DEFAULT_FIELDS = 'lum,col'
DEFAULT_HC     = 20000


def need_regen(out_path, src_path):
    if not os.path.isfile(out_path):
        return True
    return os.path.getmtime(src_path) > os.path.getmtime(out_path)


def generate_gold(rom_path, out_path, halfclocks, force):
    if not force and not need_regen(out_path, rom_path):
        return 0
    cmd = ['python3', RUN_SIM, '--rom', rom_path,
           '--halfclocks', str(halfclocks), '--out', out_path]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode('utf-8', 'replace'))
    return r.returncode


def generate_ours(rom_path, out_path, halfclocks):
    cmd = [RUN_TIA, '--rom', rom_path,
           '--halfclocks', str(halfclocks), '--out', out_path]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr.decode('utf-8', 'replace'))
    return r.returncode


def diff_traces(gold, ours, fields):
    """Return (compared, matched, mismatches, first_diff_hc_or_None)."""
    with open(gold) as f:
        f.readline()   # header
        gold_recs = {json.loads(l)['hc']: json.loads(l)
                     for l in f if l.strip()}
    with open(ours) as f:
        f.readline()
        our_recs = {json.loads(l)['hc']: json.loads(l)
                    for l in f if l.strip()}

    fs = tuple(x for x in fields.split(',') if x)
    common = sorted(set(gold_recs) & set(our_recs))

    compared = 0
    matched = 0
    first_diff = None
    for hc in common:
        if hc < 4:                      # skip startup-noise sample
            continue
        compared += 1
        g, o = gold_recs[hc], our_recs[hc]
        if all(g.get(k) == o.get(k) for k in fs):
            matched += 1
        elif first_diff is None:
            first_diff = hc
    return compared, matched, compared - matched, first_diff


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument('--halfclocks', type=int, default=DEFAULT_HC)
    ap.add_argument('--rom', default=None,
                    help='Run just this ROM (basename, with or without .bin).')
    ap.add_argument('--rebuild-gold', action='store_true')
    ap.add_argument('--fields', default=DEFAULT_FIELDS)
    args = ap.parse_args()

    if not os.path.isdir(ROM_DIR):
        sys.exit('ROM dir missing: %s (run build_test_roms.py)' % ROM_DIR)
    if not os.access(RUN_TIA, os.X_OK):
        sys.exit('%s not built (make -C tests/tools)' % RUN_TIA)

    os.makedirs(TMP_DIR, exist_ok=True)

    roms = sorted(glob.glob(os.path.join(ROM_DIR, '*.bin')))
    if args.rom:
        want = args.rom if args.rom.endswith('.bin') else args.rom + '.bin'
        roms = [r for r in roms if os.path.basename(r) == want]
        if not roms:
            sys.exit('ROM not found: %s' % want)
    if not roms:
        sys.exit('No ROMs in %s' % ROM_DIR)

    results = []
    for rom_path in roms:
        stem = os.path.splitext(os.path.basename(rom_path))[0]
        gold = os.path.join(TMP_DIR, '%s_gold.ndjson' % stem)
        ours = os.path.join(TMP_DIR, '%s_ours.ndjson' % stem)

        rc = generate_gold(rom_path, gold, args.halfclocks, args.rebuild_gold)
        if rc != 0:
            results.append((stem, 'GOLD-FAIL', 0, 0, None))
            continue
        rc = generate_ours(rom_path, ours, args.halfclocks)
        if rc != 0:
            results.append((stem, 'OURS-FAIL', 0, 0, None))
            continue

        compared, matched, missed, first_diff = diff_traces(
            gold, ours, args.fields)
        results.append((stem, 'OK', compared, matched, first_diff))

    # ------ report ------------------------------------------------------
    print('ROM                         compared  matched  missed  first_diff')
    print('-' * 75)
    tot_c, tot_m = 0, 0
    fail = False
    for stem, status, compared, matched, first_diff in results:
        missed = compared - matched
        if status != 'OK':
            print('%-26s  %s' % (stem, status))
            fail = True
            continue
        tot_c += compared
        tot_m += matched
        if missed != 0:
            fail = True
        pct = 100.0 * matched / compared if compared else 0.0
        fd = 'hc=%d' % first_diff if first_diff is not None else '—'
        print('%-26s  %8d %8d %7d  %s  (%.2f%%)' %
              (stem, compared, matched, missed, fd, pct))
    print('-' * 75)
    if tot_c:
        print('total                       %8d %8d %7d  (%.2f%%)' %
              (tot_c, tot_m, tot_c - tot_m, 100.0 * tot_m / tot_c))
    return 1 if fail else 0


if __name__ == '__main__':
    sys.exit(main())
