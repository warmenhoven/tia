#!/usr/bin/env python3
"""
Generate tiny 2600 test ROMs for the Sim2600 oracle harness.

Each ROM exercises one TIA feature in isolation so that when the
comparator reports divergence we can attribute it to a specific register
or timing path. ROMs are assembled "by hand" — a full dasm toolchain is
overkill for <50-byte programs. A `bytes(...)` helper lays opcodes out
sequentially; the reset/IRQ vectors are patched in at the tail.

Output layout (4K ROMs, padded with $FF):
  $F000 + offset → program bytes (execution begins here)
  $FFFC/$FFFD   → reset vector = $F000
  $FFFE/$FFFF   → IRQ/BRK vector = $F000

Run:  python3 build_test_roms.py  →  tests/oracle/test_roms/*.bin
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, '..', 'oracle', 'test_roms'))

ROM_SIZE = 4096
BASE_ADDR = 0xF000


def finish(code):
    """Pad `code` (list[int]) to 4K and patch the reset + IRQ vectors."""
    assert len(code) <= ROM_SIZE - 4, 'code too long (need room for vectors)'
    rom = bytearray([0xFF] * ROM_SIZE)
    rom[0:len(code)] = bytes(code)
    rom[0xFFC] = BASE_ADDR & 0xFF       # reset vec lo
    rom[0xFFD] = BASE_ADDR >> 8          # reset vec hi
    rom[0xFFE] = BASE_ADDR & 0xFF       # IRQ vec lo
    rom[0xFFF] = BASE_ADDR >> 8
    return bytes(rom)


def write(name, code):
    rom = finish(code)
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, name)
    with open(path, 'wb') as fp:
        fp.write(rom)
    print('%s  %d bytes of code' % (path, len(code)))


# -----------------------------------------------------------------------
# Standard 2600 startup preamble: SEI/CLD, loop X=$7F down to $00 writing
# zero into every TIA write register ($00-$2C) and low RAM, then TXS to
# set the stack (DEX wraps 0→$FF, so X=$FF on exit).
#
# Why the zero pass: Sim2600's transistor-level TIA boots with the same
# latch state real silicon would — the DAC output is suppressed until
# software explicitly clears the control registers. Without this preamble
# a later COLUBK write has no visible effect.
#
# BPL (not BNE) relies on X hitting $FF after DEX of $00, flipping N=1,
# falling through. Loop order $7F→$00 means HMCLR ($2B) fires before
# HMOVE ($2A) so no spurious sprite motion. 12 bytes ($F000..$F00B).
# -----------------------------------------------------------------------
PREAMBLE = [
    0x78,              # SEI
    0xD8,              # CLD
    0xA2, 0x7F,        # LDX #$7F
    0xA9, 0x00,        # LDA #$00
    # CLEAR (at $F006):
    0x95, 0x00,        # STA $00,X
    0xCA,              # DEX
    0x10, 0xFB,        # BPL CLEAR          ; -5 → $F006
    0x9A,              # TXS                ; S = X = $FF
]


# =======================================================================
# ROM #1: solid_bg — COLUBK latched, then infinite loop.
#   Tests that a single background register write propagates to every
#   visible pixel on every scanline, forever. The oracle trace should
#   show col=8, lum=2 on every visible clock after the write settles.
# =======================================================================
def rom_solid_bg():
    # PREAMBLE ends at $F00C. LDA/STA take 4 bytes → JMP lives at $F010
    # and spins on itself.
    code = list(PREAMBLE) + [
        0xA9, 0x84,        # LDA #$84   ; blue, lum 2  (col=8, lum=2)
        0x85, 0x09,        # STA $09    ; COLUBK
        0x4C, 0x10, 0xF0,  # JMP $F010  ; spin
    ]
    write('solid_bg.bin', code)


if __name__ == '__main__':
    rom_solid_bg()
