#!/usr/bin/env python3
"""Fetch Tom Harte's 6502 per-instruction test vectors and pack them into a
compact binary at tests/unit/roms/harte_65x02.bin for the C test harness to
consume.

Usage:
    python3 tests/tools/fetch_harte.py          # download if .bin is absent
    python3 tests/tools/fetch_harte.py --force  # rebuild even if present

Idempotent. Each opcode's JSON is ~5MB; 256 opcodes = ~1.3GB of JSON that we
stream through memory one at a time to avoid caching it. The resulting
packed binary is ~220MB (roughly 90 bytes per test × 10,000 tests × 256
opcodes). Keep it local (.gitignore'd).

Binary format:
    magic:       "HART"                        (4 bytes)
    version:     uint32 LE, currently 1        (4 bytes)
    n_opcodes:   uint32 LE (expected 256)      (4 bytes)
    for each opcode:
        op:          uint8
        n_cases:     uint32 LE
        for each case:
            case:        (see pack_case below)
"""
import argparse
import io
import json
import os
import pathlib
import struct
import sys
import urllib.request

BASE_URL = "https://raw.githubusercontent.com/SingleStepTests/65x02/main/6502/v1"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT_PATH = REPO_ROOT / "tests/unit/roms/harte_65x02.bin"


def pack_regs(r):
    # pc (LE u16), s, a, x, y, p  — 7 bytes
    return struct.pack("<HBBBBB", r["pc"], r["s"], r["a"], r["x"], r["y"], r["p"])


def pack_ram(ram):
    # u8 count + count × (addr LE u16, value u8)
    out = bytes([len(ram)])
    for addr, val in ram:
        out += struct.pack("<HB", addr, val)
    return out


def pack_cycles(cyc):
    # u8 count + count × (addr LE u16, value u8, rw u8 [0=read, 1=write])
    out = bytes([len(cyc)])
    for addr, val, rw in cyc:
        out += struct.pack("<HBB", addr, val, 1 if rw == "write" else 0)
    return out


def pack_case(c):
    buf = io.BytesIO()
    buf.write(pack_regs(c["initial"]))
    buf.write(pack_ram(c["initial"]["ram"]))
    buf.write(pack_regs(c["final"]))
    buf.write(pack_ram(c["final"]["ram"]))
    buf.write(pack_cycles(c["cycles"]))
    return buf.getvalue()


def fetch_opcode(opcode):
    url = f"{BASE_URL}/{opcode:02x}.json"
    with urllib.request.urlopen(url) as r:
        return json.loads(r.read())


def parse_opcode_list(s):
    """Parse '0xEA,0x1B,0x4C-0x50' into a sorted list of ints."""
    out = set()
    for part in s.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-")
            for v in range(int(a, 0), int(b, 0) + 1):
                out.add(v)
        else:
            out.add(int(part, 0))
    for v in out:
        if not 0 <= v <= 0xFF:
            raise ValueError(f"opcode {v:#x} out of range")
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--force", action="store_true",
                    help="rebuild even if the target file already exists")
    ap.add_argument("--opcodes", type=str, default=None,
                    help="comma-separated opcodes or ranges (e.g. '0xEA' or "
                         "'0x00-0x0F,0x1B') — defaults to all 256. Useful for "
                         "iterating on the C test harness with a small file.")
    ap.add_argument("--out", type=pathlib.Path, default=OUT_PATH,
                    help=f"output binary path (default: {OUT_PATH})")
    args = ap.parse_args()

    if args.out.exists() and not args.force:
        print(f"{args.out} already exists ({args.out.stat().st_size:,} bytes).")
        print("Pass --force to rebuild.")
        return 0

    opcodes = parse_opcode_list(args.opcodes) if args.opcodes else list(range(256))

    args.out.parent.mkdir(parents=True, exist_ok=True)
    out = io.BytesIO()
    out.write(b"HART")
    out.write(struct.pack("<II", 1, len(opcodes)))

    total_cases = 0
    try:
        for i, op in enumerate(opcodes):
            cases = fetch_opcode(op)
            out.write(bytes([op]))
            out.write(struct.pack("<I", len(cases)))
            for c in cases:
                out.write(pack_case(c))
            total_cases += len(cases)
            if (i + 1) % 16 == 0 or i == len(opcodes) - 1:
                print(f"  {i + 1:3d}/{len(opcodes)} opcodes packed"
                      f" ({total_cases:,} cases so far,"
                      f" ~{out.tell() / (1024 * 1024):.1f} MB)",
                      flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted; leaving target unwritten", file=sys.stderr)
        return 130
    except Exception as e:
        print(f"\nfetch failed for opcode {op:02x}: {e}", file=sys.stderr)
        return 1

    data = out.getvalue()
    args.out.write_bytes(data)
    print(f"\nwrote {args.out}: {len(data):,} bytes, "
          f"{total_cases:,} cases across {len(opcodes)} opcodes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
