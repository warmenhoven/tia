#!/usr/bin/env python3
"""Run the Python oracle for N half-clocks and dump wire states + pixels
in the same byte layout as the C port's --trace/--pixels output.

Layout per half-clock in the trace file:
  byte[0 .. numCPUWires)          - CPU wire.state values
  byte[numCPUWires .. +numTIAWires) - TIA wire.state values
Preceded by one extra dump of the post-init state (before any half-clock)."""

import argparse
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIM_DIR = os.path.join(os.path.dirname(HERE), 'Sim2600')
sys.path.insert(0, SIM_DIR)

import params  # noqa: E402
from sim2600Console import Sim2600Console  # noqa: E402


def dump_states(f, cs):
    # wire.state is always 0..255; wireList entries with None aren't in our
    # C port either, but Python's loader (bug'd null-detection) never
    # produces None in practice, so we can iterate by index.
    buf = bytearray(len(cs.wireList))
    for i, w in enumerate(cs.wireList):
        buf[i] = w.state if w is not None else 0
    f.write(buf)
    # Transistor on/off (NmosFet.GATE_HIGH == 1).
    tbuf = bytearray(len(cs.transistorList))
    for i, t in enumerate(cs.transistorList):
        tbuf[i] = t.gateState if t is not None else 0
    f.write(tbuf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=params.romFile)
    ap.add_argument('--halfclocks', type=int, default=2000)
    ap.add_argument('--trace')
    ap.add_argument('--pixels')
    ap.add_argument('--vtrace',
                    help='dump state after every recalc call (diag)')
    ap.add_argument('--debug-vstep', type=int, default=-1,
                    help='verbose flood trace during this vstep')
    args = ap.parse_args()

    vtraceF = open(args.vtrace, 'wb') if args.vtrace else None

    # Install vtrace hook BEFORE constructing Sim2600Console so the init
    # sequence's recalc calls are captured too.
    if vtraceF:
        from circuitSimulatorBase import CircuitSimulatorBase
        from circuitSimulatorUsingLists import CircuitSimulator
        _cpu_ref = [None]
        _tia_ref = [None]
        _vstep = [0]
        _debug_active = [False]

        def _hook(self):
            if _cpu_ref[0] is None or _tia_ref[0] is None:
                return
            which = 'CPU' if self is _cpu_ref[0] else 'TIA'
            print(f'vstep {_vstep[0]}: {which} recalc done, '
                  f'totalRecalc={self.numWiresRecalculated}',
                  file=sys.stderr)
            _vstep[0] += 1
            dump_states(vtraceF, _cpu_ref[0])
            dump_states(vtraceF, _tia_ref[0])
            _debug_active[0] = (_vstep[0] == args.debug_vstep)

        orig_doIter = CircuitSimulatorBase.doRecalcIterations
        def wrapped_doIter(self):
            if _debug_active[0]:
                pass  # no-op; debug hooks below fire naturally
            orig_doIter(self)
            _hook(self)
        CircuitSimulatorBase.doRecalcIterations = wrapped_doIter

        # Wrap addWireToGroupList and turnTransistorOn/Off for the target vstep.
        orig_add = CircuitSimulator.addWireToGroupList
        def wrap_add(self, wireIndex):
            if _debug_active[0] and self.lastWireGroupState[wireIndex] != self.lastChipGroupState:
                w = self.wireList[wireIndex]
                print(f'  add {wireIndex}  state=0x{w.state:02X} pulled=0x{w.pulled:02X}',
                      file=sys.stderr)
            orig_add(self, wireIndex)
        CircuitSimulator.addWireToGroupList = wrap_add

        orig_on = CircuitSimulator.turnTransistorOn
        def wrap_on(self, t):
            if _debug_active[0]:
                print(f'  ON  t={t.index} s1={t.side1WireIndex} '
                      f's2={t.side2WireIndex} gate={t.gateWireIndex}',
                      file=sys.stderr)
            orig_on(self, t)
        CircuitSimulator.turnTransistorOn = wrap_on

        orig_off = CircuitSimulator.turnTransistorOff
        def wrap_off(self, t):
            if _debug_active[0]:
                print(f'  OFF t={t.index} s1={t.side1WireIndex} '
                      f's2={t.side2WireIndex} gate={t.gateWireIndex}',
                      file=sys.stderr)
            orig_off(self, t)
        CircuitSimulator.turnTransistorOff = wrap_off

        orig_dwr = CircuitSimulator.doWireRecalc
        def wrap_dwr(self, wireIndex):
            if _debug_active[0]:
                print(f'== recalc wire {wireIndex}', file=sys.stderr)
            orig_dwr(self, wireIndex)
            if _debug_active[0]:
                print(f'   groupValue=0x{self.groupValue:X} '
                      f'groupCount={self.groupListLastIndex}',
                      file=sys.stderr)
        CircuitSimulator.doWireRecalc = wrap_dwr
    else:
        _cpu_ref = _tia_ref = None

    if os.environ.get('SIM_INIT_PROBE'):
        # Match the C init-probe: print chip name + cumulative recalc count
        # after every recalc call during Sim2600Console construction.
        from circuitSimulatorBase import CircuitSimulatorBase
        from sim6502 import Sim6502
        from simTIA import SimTIA
        Sim6502._chip_name = '6502'
        SimTIA._chip_name  = 'TIA'

        _orig = CircuitSimulatorBase.doRecalcIterations
        def _probe(self):
            _orig(self)
            print(f'  probe {getattr(self, "_chip_name", "?")} '
                  f'count={self.numWiresRecalculated}', file=sys.stderr)
        CircuitSimulatorBase.doRecalcIterations = _probe

    sim = Sim2600Console(args.rom)
    cpu = sim.sim6507
    tia = sim.simTIA

    if vtraceF:
        _cpu_ref[0] = cpu
        _tia_ref[0] = tia
        # The init calls have already fired the hook — but _*_ref was None,
        # so nothing got written.  We don't miss anything, because the
        # init-time states aren't the divergence point.

    traceF  = open(args.trace,  'wb') if args.trace  else None
    pixelsF = open(args.pixels, 'wb') if args.pixels else None

    if traceF:
        dump_states(traceF, cpu)
        dump_states(traceF, tia)

    t0 = time.monotonic()
    for i in range(args.halfclocks):
        sim.advanceOneHalfClock()
        if pixelsF and tia.isLow(tia.padIndCLK0):
            rgba = tia.getColorRGBA8()
            pixelsF.write(struct.pack('>I', rgba))
        if traceF:
            dump_states(traceF, cpu)
            dump_states(traceF, tia)
    elapsed = time.monotonic() - t0

    print(f'python: {args.halfclocks} half-clocks in {elapsed:.3f} s '
          f'= {args.halfclocks/elapsed:.0f} hc/s', file=sys.stderr)

    if traceF:  traceF.close()
    if pixelsF: pixelsF.close()
    if vtraceF: vtraceF.close()


if __name__ == '__main__':
    main()
