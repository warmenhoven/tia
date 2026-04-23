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

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.normpath(os.path.join(HERE, '..', 'oracle', 'test_roms'))

ROM_SIZE = 4096
BASE_ADDR = 0xF000


def finish(code):
    """Pad `code` (list[int]) to 4K and patch the reset + IRQ vectors."""
    assert len(code) <= ROM_SIZE - 4, 'code too long (need room for vectors)'
    rom = bytearray([0xFF] * ROM_SIZE)
    rom[0:len(code)] = bytes(code)
    rom[0xFFC] = BASE_ADDR & 0xFF
    rom[0xFFD] = BASE_ADDR >> 8
    rom[0xFFE] = BASE_ADDR & 0xFF
    rom[0xFFF] = BASE_ADDR >> 8
    return bytes(rom)


def write(name, code):
    rom = finish(code)
    os.makedirs(OUT_DIR, exist_ok=True)
    path = os.path.join(OUT_DIR, name)
    with open(path, 'wb') as fp:
        fp.write(rom)
    print('%s  %d bytes of code' % (path, len(code)))


def spin(code):
    """Append `JMP $<current-addr>` so the program spins on its own JMP."""
    target = BASE_ADDR + len(code)
    code += [0x4C, target & 0xFF, target >> 8]


def nops(code, n):
    """Append `n` NOPs (each 2 cycles = 6 TIA ticks)."""
    code += [0xEA] * n


def lda_sta(code, value, addr):
    """Append `LDA #value; STA addr` (zero-page)."""
    code += [0xA9, value, 0x85, addr]


def sta(code, addr):
    """Append `STA addr` (zero-page strobe — A's value doesn't matter)."""
    code += [0x85, addr]


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
# solid_bg — COLUBK=$84 latched, then spin.
#   Exercises: background rendering via COLUBK; baseline scanline timing.
#   Oracle trace should show col=8, lum=2 on every visible clock after
#   the COLUBK write settles.
# =======================================================================
def rom_solid_bg():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK = blue
    spin(code)
    write('solid_bg.bin', code)


# =======================================================================
# rsync_late — strobe RSYNC AFTER the preamble has finished, so RSYNC's
# effect lands on a well-defined scanline distinct from the init-loop
# RSYNC at X=$03. Useful for regressions in the RSYNC hpos setpoint.
# =======================================================================
def rom_rsync_late():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)
    sta(code, 0x03)              # RSYNC strobe (A is irrelevant)
    spin(code)
    write('rsync_late.bin', code)


# =======================================================================
# playfield_stripes — PF0/PF1/PF2 patterns, no reflect.
#   Exercises: playfield bit decoding, repeat (not mirror), pixel timing
#   across the three PF registers. COLUPF orange vs COLUBK blue.
# =======================================================================
def rom_playfield_stripes():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK blue
    lda_sta(code, 0x3E, 0x08)    # COLUPF orange (col=3, lum=7)
    lda_sta(code, 0xF0, 0x0D)    # PF0 = $F0 (pixels 0-3 on)
    lda_sta(code, 0xAA, 0x0E)    # PF1 = $AA (alternating)
    lda_sta(code, 0x55, 0x0F)    # PF2 = $55 (alternating, reversed)
    spin(code)
    write('playfield_stripes.bin', code)


# =======================================================================
# playfield_reflect — same PF pattern but with CTRLPF.REF set.
#   Exercises: playfield mirroring. Right half of scanline should mirror
#   left half instead of repeating.
# =======================================================================
def rom_playfield_reflect():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)
    lda_sta(code, 0x3E, 0x08)
    lda_sta(code, 0xFF, 0x0D)    # PF0 all on
    lda_sta(code, 0xAA, 0x0E)
    lda_sta(code, 0x55, 0x0F)
    lda_sta(code, 0x01, 0x0A)    # CTRLPF: REF = 1
    spin(code)
    write('playfield_reflect.bin', code)


# =======================================================================
# playfield_score — score-mode (PF left half uses COLUP0, right uses COLUP1).
# =======================================================================
def rom_playfield_score():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK
    lda_sta(code, 0x2E, 0x06)    # COLUP0 (green lum 7)
    lda_sta(code, 0x48, 0x07)    # COLUP1 (gold lum 0)
    lda_sta(code, 0xFF, 0x0D)    # PF0
    lda_sta(code, 0xAA, 0x0E)
    lda_sta(code, 0x55, 0x0F)
    lda_sta(code, 0x02, 0x0A)    # CTRLPF: SCORE = 1
    spin(code)
    write('playfield_score.bin', code)


# =======================================================================
# resp0_visible — RESP0 strobed mid-visible, GRP0 all-ones.
#   Exercises: player 0 positioning via RESP0 strobe, player rendering.
#   NOPs before the strobe place the beam in a known area of the visible
#   region so the sprite lands somewhere observable.
# =======================================================================
def rom_resp0_visible():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK blue
    lda_sta(code, 0x46, 0x06)    # COLUP0 (light magenta)
    lda_sta(code, 0xFF, 0x1B)    # GRP0 = $FF (all 8 pixels on)
    nops(code, 6)                # 6 NOPs = 12 CPU cy = 36 TIA ticks delay
    sta(code, 0x10)              # RESP0
    spin(code)
    write('resp0_visible.bin', code)


# =======================================================================
# hmove_basic — position P0, set HMP0, strobe HMOVE, observe motion.
#   Exercises: HMOVE motion-delta application + 8-clock extended HBLANK.
# =======================================================================
def rom_hmove_basic():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)
    lda_sta(code, 0x46, 0x06)
    lda_sta(code, 0xFF, 0x1B)    # GRP0 visible
    sta(code, 0x10)              # RESP0 — anchor player at current hpos
    lda_sta(code, 0x70, 0x20)    # HMP0 = +7 (shift left 7 clocks)
    sta(code, 0x2A)              # HMOVE strobe
    spin(code)
    write('hmove_basic.bin', code)


# =======================================================================
# grp_vdel — VDELP0 + staggered GRP0/GRP1 writes to exercise the
# latch-on-paired-write mechanic. Writing GRP1 latches GRP0 (and enabl)
# into their display shadows; VDELP0 routes the shadow to the renderer.
# =======================================================================
def rom_grp_vdel():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK
    lda_sta(code, 0x46, 0x06)    # COLUP0
    lda_sta(code, 0x28, 0x07)    # COLUP1 (yellow)
    sta(code, 0x10)              # RESP0
    sta(code, 0x11)              # RESP1
    lda_sta(code, 0x01, 0x25)    # VDELP0 on (bit 0)
    lda_sta(code, 0xAA, 0x1B)    # GRP0 = $AA (raw); grp1_latch ← grp1
    lda_sta(code, 0x55, 0x1C)    # GRP1 = $55 (raw); grp0_latch ← $AA
    spin(code)
    write('grp_vdel.bin', code)


# =======================================================================
# missile_ball — enable M0 and BL, position them.
#   Exercises: ENAM0, ENABL, RESM0, RESBL, CTRLPF ball-size bits.
# =======================================================================
def rom_missile_ball():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK
    lda_sta(code, 0x46, 0x06)    # COLUP0 (M0 color)
    lda_sta(code, 0x3E, 0x08)    # COLUPF (ball color)
    lda_sta(code, 0x30, 0x0A)    # CTRLPF bits 4-5 = 11 → ball size 8
    lda_sta(code, 0x02, 0x1D)    # ENAM0 (bit 1)
    lda_sta(code, 0x02, 0x1F)    # ENABL
    sta(code, 0x12)              # RESM0
    sta(code, 0x14)              # RESBL
    spin(code)
    write('missile_ball.bin', code)


# =======================================================================
# wsync_stall — write to WSYNC and observe the CPU stall until next HSYNC.
#   Exercises: WSYNC latch, RDY assertion, stall release at scanline wrap.
# =======================================================================
def rom_wsync_stall():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK blue
    sta(code, 0x02)              # WSYNC strobe — halts CPU
    lda_sta(code, 0x3E, 0x08)    # COLUPF — lands on the line AFTER stall
    spin(code)
    write('wsync_stall.bin', code)


# =======================================================================
# vblank_on — set VBLANK bit 1; output should be forced black on every
# visible clock. Tests the VBLANK-suppression path.
# =======================================================================
def rom_vblank_on():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)    # COLUBK blue (would render if VBLANK off)
    lda_sta(code, 0x02, 0x01)    # VBLANK bit 1 on → force black
    spin(code)
    write('vblank_on.bin', code)


# =======================================================================
# vsync_pulse — briefly set VSYNC bit 1 then clear. Exercises the
# frame-boundary rising edge that the libretro layer uses to flush.
# =======================================================================
def rom_vsync_pulse():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)
    lda_sta(code, 0x02, 0x00)    # VSYNC on (bit 1)
    nops(code, 10)               # ~60 TIA ticks of VSYNC held
    lda_sta(code, 0x00, 0x00)    # VSYNC off
    spin(code)
    write('vsync_pulse.bin', code)


# =======================================================================
# nusiz_two_copies — P0 positioned, NUSIZ0 set to "two copies close".
#   Exercises: NUSIZ player-copy decoding.
# =======================================================================
def rom_nusiz_two_copies():
    code = list(PREAMBLE)
    lda_sta(code, 0x84, 0x09)
    lda_sta(code, 0x46, 0x06)
    lda_sta(code, 0xFF, 0x1B)    # GRP0
    lda_sta(code, 0x01, 0x04)    # NUSIZ0 = 1 (two copies, close)
    sta(code, 0x10)              # RESP0
    spin(code)
    write('nusiz_two_copies.bin', code)


ALL = [
    rom_solid_bg,
    rom_rsync_late,
    rom_playfield_stripes,
    rom_playfield_reflect,
    rom_playfield_score,
    rom_resp0_visible,
    rom_hmove_basic,
    rom_grp_vdel,
    rom_missile_ball,
    rom_wsync_stall,
    rom_vblank_on,
    rom_vsync_pulse,
    rom_nusiz_two_copies,
]


if __name__ == '__main__':
    for f in ALL:
        f()
