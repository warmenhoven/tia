# libretro-tia

An Atari 2600 libretro core, written from scratch in C89. Cycle-accurate
CPU/TIA/RIOT emulation, automatic region detection, and a broad mapper
set. Not a port — everything in `src/` is an original implementation.

## Build

```
make              # release .dylib / .so / .dll
make DEBUG=1      # debug build with -O0 -g
```

The top-level `Makefile` is the standard libretro single-file build;
platform is auto-detected from `uname`.

## Install / use

Copy the built `tia_libretro.<ext>` into RetroArch's `cores/` directory
along with `tia_libretro.info`. RetroArch then exposes it as
**"Atari - 2600 (Tia)"** in the content loader.

ROMs: accepts `.a26` and `.bin`. No header, no fingerprint file —
mapper detection is automatic from size plus heuristic byte-pattern
scans for ambiguous sizes.

## Supported cartridge mappers

| Family            | Mappers                                       |
|-------------------|-----------------------------------------------|
| Plain ROM         | 2K, 4K                                        |
| Atari bank-switch | F8, F6, F4 (+ SuperChip RAM variants)         |
| 3rd-party         | 3F (Tigervision), E0 (Parker Bros),<br>FE (Activision), FA (CBS RAM+),<br>E7 (M-Network), F0 (Megaboy), DPC (Pitfall II) |

Not yet supported: AR (Supercharger), DPC+, CDF/CDFJ, BUS, and the
long-tail exotic mappers.

## Supported controllers

- **Joystick** — standard digital stick + fire
- **Paddle** — two paddles per port, analog stick → pot emulation
- **Driving controller** — gray-code quadrature (Indy 500)
- **Keypad** — 12-button matrix (Star Raiders, Codebreaker, etc.)

Console switches (Reset, Select, two Difficulty toggles, TV-type
toggle) are mapped to retropad buttons per the common 2600 touch
overlay; OSD feedback on latch changes.

## Core options

| Key                       | Values                                 | Default      |
|---------------------------|----------------------------------------|--------------|
| `tia_region`              | `auto, ntsc, pal, pal60, secam`        | `auto`       |
| `tia_palette`             | `standard, z26`                        | `standard`   |
| `tia_left_diff`           | `a, b`                                 | `a`          |
| `tia_right_diff`          | `a, b`                                 | `a`          |
| `tia_color`               | `color, bw`                            | `color`      |
| `tia_crop_hoverscan`      | `off, on`                              | `off`        |
| `tia_crop_voverscan`      | `0, 2, 4, …, 24` (rows)                | `0`          |
| `tia_paddle_sensitivity`  | `1..10` (5 = neutral)                  | `5`          |
| `tia_paddle_deadzone`     | `0..30` (% of stick range)             | `0`          |

Difficulty/TV-type options set only the *initial* latch; runtime
button toggles work on top. Region auto-detect locks NTSC vs PAL from
the game's scanline count after ~5 frames.

## Project layout

```
src/
  cpu.c / cpu.h          MOS 6502 core (full 16-bit address)
  bus.c / bus.h          6507 address decode + cycle accounting
  tia.c / tia.h          TIA: video, audio, input
  riot.c / riot.h        6532 RIOT: RAM, I/O, timer
  cart.c / cart.h        Mapper implementations
  libretro.c / .h        libretro API surface
tests/
  unit/                  C unit tests (plain Makefile)
  int/                   Python integration tests (pytest + libretro.py)
  tools/                 Test-data fetchers (Harte corpus)
```

## Testing

```
# C unit tests — doc-anchored TIA behaviour, CPU Harte vectors,
# RIOT timer, cart mappers, etc.
make -C tests/unit test

# libretro integration tests — exercise retro_* API via Python
tests/int/.venv/bin/python -m pytest tests/int

# Compatibility sweep against a ROM collection (set ATARI_ROM_DIR)
ATARI_ROM_DIR=/path/to/collection \
  tests/int/.venv/bin/python tmp/test_the_world.py
```

Harte CPU vectors live in `tests/unit/roms/harte_65x02.bin` (fetched
on demand by `tests/tools/fetch_harte.py` — ~160 MB, not in git).

## License

GPL-2.0-or-later. See `COPYING`.

## Credits

- [Stella](https://stella-emu.github.io/) — extensive reference for TIA
  behaviour, cartridge-mapper specifics, and palette tables (both the
  Stella and z26 variants).
- Tom Harte's [`processor_tests`](https://github.com/SingleStepTests/65x02) —
  per-instruction 6502 validation corpus.
- Kevin Horton, Andrew Towers, and the broader 2600 homebrew community —
  hardware documentation and reverse-engineering work that this core
  depends on.
