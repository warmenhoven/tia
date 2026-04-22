#!/usr/bin/env python3
"""
Sim2600 trace wrapper.

Loads a 2600 ROM into Greg James's transistor-level Sim2600
(tests/oracle/Sim2600), runs N TIA half-clocks, and writes a
streaming JSON trace of per-clock state. These traces are the
"golden reference" used by the oracle-backed unit tests against
our tia.c implementation.

If the native C port at tests/oracle/csim/sim2600 is available and
up-to-date, it is invoked instead of the Python oracle for a ~55×
speedup.  The two produce byte-identical NDJSON output.  Set
SIM2600_FORCE_PYTHON=1 to skip the C port and use the Python
implementation directly.

Trace format (newline-delimited JSON):

  Line 1:  header
    { "format": "sim2600-trace", "version": 1,
      "rom": "<basename>", "halfclocks": N, "every": K }

  Lines 2..:  one record per sample point (every K half-clocks)
    { "hc":     TIA half-clock count (absolute),
      "cpu_hc": 6507 half-clock count,
      "addr":   CPU address bus (16-bit),
      "data":   CPU data bus (8-bit),
      "rw":     1 = read, 0 = write,
      "sync":   6502 SYNC pad (instruction fetch),
      "rdy":    RDY pad,
      "vsync":  TIA VSYNC latch,
      "vblank": TIA VBLANK,
      "wsync":  TIA WSYNC,
      "rsync":  TIA RSYNC,
      "lum":    3-bit luminance driving pixel output,
      "col":    4-bit color counter driving pixel output,
      "rgba":   decoded pixel RGBA8 (0xRRGGBBAA, integer) }

Default --every is 2, i.e. one record per TIA color clock — the
natural cadence for pixel sampling (matches Sim2600's mainSim.py).
Use --every 1 for per-half-clock fidelity when debugging timing.
"""

import argparse
import json
import os
import subprocess
import sys

HERE     = os.path.dirname(os.path.abspath(__file__))
SIM_DIR  = os.path.normpath(os.path.join(HERE, '..', 'oracle', 'Sim2600'))
CSIM_DIR = os.path.normpath(os.path.join(HERE, '..', 'oracle', 'csim'))


def try_csim(rom, halfclocks, every, out):
    """Invoke the native C port if present + built.  Returns True on success,
    False to fall back to the Python path."""
    if os.environ.get('SIM2600_FORCE_PYTHON'):
        return False
    binary = os.path.join(CSIM_DIR, 'sim2600')
    cpu    = os.path.join(CSIM_DIR, 'chip_6502.ckt')
    tia    = os.path.join(CSIM_DIR, 'chip_TIA.ckt')
    if not (os.path.isfile(binary) and os.access(binary, os.X_OK)
            and os.path.isfile(cpu) and os.path.isfile(tia)):
        return False
    cmd = [binary, '--cpu', cpu, '--tia', tia, '--rom', rom,
           '--halfclocks', str(halfclocks), '--every', str(every),
           '--ndjson', out]
    r = subprocess.run(cmd)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser(description='Run Sim2600 and emit a golden trace.')
    ap.add_argument('--rom', required=True, help='Path to 2600 ROM (2K/4K/8K).')
    ap.add_argument('--halfclocks', type=int, required=True,
                    help='Number of TIA half-clocks to simulate.')
    ap.add_argument('--out', required=True, help='Output trace file (.ndjson).')
    ap.add_argument('--every', type=int, default=2,
                    help='Emit a record every N half-clocks (default 2 = 1/pixel).')
    ap.add_argument('--verbose', action='store_true',
                    help='Pass through Sim2600\'s own diagnostic prints.')
    args = ap.parse_args()

    if args.every < 1:
        sys.exit('--every must be >= 1')
    if args.halfclocks < 1:
        sys.exit('--halfclocks must be >= 1')
    if not os.path.isfile(args.rom):
        sys.exit('ROM not found: %s' % args.rom)
    if not os.path.isdir(SIM_DIR):
        sys.exit('Sim2600 not found at %s (fetch it first).' % SIM_DIR)

    # Prefer the native C port when available — byte-identical output, ~55× faster.
    if try_csim(args.rom, args.halfclocks, args.every, args.out):
        return

    sys.path.insert(0, SIM_DIR)

    # Sim2600 is chatty; silence its prints unless --verbose.
    saved_stdout = sys.stdout
    if not args.verbose:
        sys.stdout = open(os.devnull, 'w')
    try:
        from sim2600Console import Sim2600Console
        console = Sim2600Console(args.rom)
        tia = console.simTIA
        cpu = console.sim6507

        with open(args.out, 'w') as fp:
            header = {
                'format': 'sim2600-trace',
                'version': 1,
                'rom': os.path.basename(args.rom),
                'halfclocks': args.halfclocks,
                'every': args.every,
            }
            fp.write(json.dumps(header) + '\n')

            i = 0
            while i < args.halfclocks:
                console.advanceOneHalfClock()
                if (tia.halfClkCount % args.every) == 0:
                    rec = {
                        'hc':     tia.halfClkCount,
                        'cpu_hc': cpu.halfClkCount,
                        'addr':   cpu.getAddressBusValue(),
                        'data':   cpu.getDataBusValue(),
                        'rw':     int(cpu.isHigh(cpu.padIndRW)),
                        'sync':   int(cpu.isHigh(cpu.padIndSYNC)),
                        'rdy':    int(cpu.isHigh(cpu.padIndRDY)),
                        'vsync':  int(tia.isHigh(tia.vsync)),
                        'vblank': int(tia.isHigh(tia.vblank)),
                        'wsync':  int(tia.isHigh(tia.wsync)),
                        'rsync':  int(tia.isHigh(tia.rsync)),
                        'lum':    tia.get3BitLuminance(),
                        'col':    tia.get4BitColor(),
                        'rgba':   tia.getColorRGBA8(),
                    }
                    fp.write(json.dumps(rec) + '\n')
                i += 1
    finally:
        if not args.verbose:
            sys.stdout.close()
            sys.stdout = saved_stdout


if __name__ == '__main__':
    main()
