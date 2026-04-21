"""Region auto-detect smoke tests.

We load a known NTSC ROM and a known PAL ROM, run enough frames for the
TIA's auto-detect to lock (~6 VSYNCs), and confirm retro_get_region
reports the expected region.

RETRO_REGION_NTSC = 0, RETRO_REGION_PAL = 1."""
import pytest
import libretro

from conftest import load_rom_bytes, find_rom


def _load_session(core_path, rom_path):
    rom_bytes = load_rom_bytes(rom_path)
    return (libretro.SessionBuilder
            .defaults(core_path)
            .with_content(bytearray(rom_bytes))
            .build())


@pytest.fixture
def adventure_rom(rom_dir):
    return find_rom(rom_dir, "adventure (usa)")


@pytest.fixture
def pal_rom(rom_dir):
    # "Acid Drop" is an early-named Europe-only PAL homebrew and appears
    # near the top of an alphabetically-sorted No-Intro listing; using it
    # avoids ambiguity with ROMs that have both (USA) and (Europe) dumps.
    return find_rom(rom_dir, "acid drop (europe)")


def test_ntsc_rom_locks_to_ntsc(core_path, adventure_rom):
    with _load_session(core_path, adventure_rom) as s:
        for _ in range(12):
            s.run()
        assert s.core.get_region() == 0, "expected RETRO_REGION_NTSC"


def test_pal_rom_locks_to_pal(core_path, pal_rom):
    with _load_session(core_path, pal_rom) as s:
        for _ in range(12):
            s.run()
        assert s.core.get_region() == 1, "expected RETRO_REGION_PAL"
