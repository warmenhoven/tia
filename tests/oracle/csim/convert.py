#!/usr/bin/env python3
"""Convert Sim2600 .pkl chip netlists to a flat little-endian binary format
the C port can mmap/fread directly."""

import os
import pickle
import struct
import sys

# Pad/signal names the C code needs to address by string.  Everything else
# the sim references by index only.  VCC/VSS are always included via the
# fixed header slots below, not the named-pad table.
NAMED_PADS = {
    '6502': [
        'CLK0', 'CLK1OUT', 'R/W', 'RDY', 'IRQ', 'NMI', 'RES', 'SYNC',
        'AB0', 'AB1', 'AB2', 'AB3', 'AB4', 'AB5', 'AB6', 'AB7',
        'AB8', 'AB9', 'AB10', 'AB11', 'AB12', 'AB13', 'AB14', 'AB15',
        'DB0', 'DB1', 'DB2', 'DB3', 'DB4', 'DB5', 'DB6', 'DB7',
    ],
    'TIA': [
        'CLK0', 'CLK2', 'PH0', 'CS0', 'CS1', 'CS2', 'CS3', 'R/W', 'del',
        'AB0', 'AB1', 'AB2', 'AB3', 'AB4', 'AB5',
        'DB0', 'DB1', 'DB2', 'DB3', 'DB4', 'DB5', 'DB6', 'DB7',
        'I0', 'I1', 'I2', 'I3', 'I4', 'I5',
        'DB6_drvLo', 'DB6_drvHi', 'DB7_drvLo', 'DB7_drvHi',
        'RDY_lowCtrl', 'VBLANK', 'VSYNC', 'WSYNC', 'RSYNC',
        'L0_lowCtrl', 'L1_lowCtrl', 'L2_lowCtrl',
        'COLCNT_T0', 'COLCNT_T1', 'COLCNT_T2', 'COLCNT_T3',
    ],
}

MAGIC = b'CKT1'


def convert(pkl_path, kind, out_path):
    with open(pkl_path, 'rb') as f:
        d = pickle.load(f)

    numWires = d['NUM_WIRES']
    numFets = d['NUM_FETS']
    nextCtrl = d['NEXT_CTRL']
    noWire = d['NO_WIRE']
    wirePulled = d['WIRE_PULLED']
    wireCtrlFets = d['WIRE_CTRL_FETS']
    wireGates = d['WIRE_GATES']
    wireNames = d['WIRE_NAMES']
    s1s = d['FET_SIDE1_WIRE_INDS']
    s2s = d['FET_SIDE2_WIRE_INDS']
    gates = d['FET_GATE_INDS']

    if numWires >= 65535 or numFets >= 65535:
        raise SystemExit('u16 wire/fet indices are too small for this netlist')

    # Parse the flattened (count, idx..., sentinel) records.
    ct = [[] for _ in range(numWires)]
    gt = [[] for _ in range(numWires)]

    wcfi = 0
    for i in range(numWires):
        n = wireCtrlFets[wcfi]; wcfi += 1
        for _ in range(n):
            ct[i].append(wireCtrlFets[wcfi]); wcfi += 1
        if wireCtrlFets[wcfi] != nextCtrl:
            raise SystemExit(f'bad ctrlFet sentinel at wire {i}')
        wcfi += 1

    wgi = 0
    for i in range(numWires):
        n = wireGates[wgi]; wgi += 1
        for _ in range(n):
            gt[i].append(wireGates[wgi]); wgi += 1
        if wireGates[wgi] != nextCtrl:
            raise SystemExit(f'bad gate sentinel at wire {i}')
        wgi += 1

    # De-dupe, but PRESERVE Python's set-iteration order.  For floating-node
    # groups whose final polarity is decided by countWireSizes, the traversal
    # order through ctInds determines which wires end up in the group at the
    # moment countWireSizes is called — and thus the final FLOATING_HIGH vs
    # FLOATING_LOW assignment for dynamic storage nodes.  Sorting here
    # produces a different-but-valid fixpoint that visibly diverges from
    # Python on internal N-nodes.  For hash(int) == int this order is stable
    # across Python runs.
    for i in range(numWires):
        ct[i] = list(set(ct[i]))
        gt[i] = list(set(gt[i]))

    # Flat pools of transistor indices.
    ct_pool = []
    gt_pool = []
    wire_records = []
    for i in range(numWires):
        ct_off = len(ct_pool)
        gt_off = len(gt_pool)
        ct_pool.extend(ct[i])
        gt_pool.extend(gt[i])
        wire_records.append((
            int(wirePulled[i]),
            len(ct[i]),
            len(gt[i]),
            ct_off,
            gt_off,
        ))

    # Named pads needed by the console layer.
    name_to_idx = {n: i for i, n in enumerate(wireNames) if n}
    if 'VCC' not in name_to_idx or 'VSS' not in name_to_idx:
        raise SystemExit('missing VCC/VSS in netlist')

    pads = []
    for p in NAMED_PADS[kind]:
        if p in name_to_idx:
            pads.append((p, name_to_idx[p]))
        else:
            raise SystemExit(f'named pad {p!r} missing from {kind} netlist')

    # Emit.
    buf = bytearray()
    # Header (40 bytes): magic, numWires, numTrans, ctPoolLen, gtPoolLen,
    # vccIndex, gndIndex, numNamedPads, reserved
    buf += struct.pack(
        '<4sIIIIIIII',
        MAGIC,
        numWires, numFets, len(ct_pool), len(gt_pool),
        name_to_idx['VCC'], name_to_idx['VSS'],
        len(pads), 0,
    )
    # Wires (16 bytes each)
    for pulled, cc, gc, co, go in wire_records:
        buf += struct.pack('<BBHHHII', pulled, 0, cc, gc, 0, co, go)
    # Transistors (8 bytes each)
    for i in range(numFets):
        if s1s[i] == noWire:
            buf += struct.pack('<HHHH', 0xFFFF, 0xFFFF, 0xFFFF, 0)
        else:
            buf += struct.pack('<HHHH', s1s[i], s2s[i], gates[i], 0)
    # Pools
    buf += struct.pack(f'<{len(ct_pool)}H', *ct_pool)
    buf += struct.pack(f'<{len(gt_pool)}H', *gt_pool)
    # Named pads: u16 nameLen, u16 wireIndex, name bytes, pad to even.
    for name, idx in pads:
        nb = name.encode('ascii')
        if len(nb) > 255:
            raise SystemExit(f'pad name too long: {name}')
        buf += struct.pack('<HH', len(nb), idx)
        buf += nb
        if len(nb) % 2:
            buf += b'\x00'

    with open(out_path, 'wb') as f:
        f.write(buf)

    print(
        f'  wrote {out_path}  '
        f'{len(buf):,} B  '
        f'{numWires} wires  {numFets} trans  '
        f'ctPool={len(ct_pool)} gtPool={len(gt_pool)}  '
        f'pads={len(pads)}'
    )


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    sim_dir = os.path.join(os.path.dirname(here), 'Sim2600')
    convert(os.path.join(sim_dir, 'chips/net_6502.pkl'),
            '6502',
            os.path.join(here, 'chip_6502.ckt'))
    convert(os.path.join(sim_dir, 'chips/net_TIA.pkl'),
            'TIA',
            os.path.join(here, 'chip_TIA.ckt'))


if __name__ == '__main__':
    main()
