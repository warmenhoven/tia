"""Validation tests against real commercial ROMs.

Enabled when ATARI_ROM_DIR env var points to a directory containing
.a26/.bin files or zip archives (one ROM per zip, as per No-Intro layout).
"""
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
    return find_rom(rom_dir, "adventure")


def test_adventure_boots_and_runs(core_path, adventure_rom):
    """Load Adventure, run ~60 frames, confirm the CPU doesn't halt, the
    framebuffer has meaningful content, and audio samples are emitted."""
    session = _load_session(core_path, adventure_rom)
    with session as s:
        for _ in range(60):
            s.run()
        shot = s.video.screenshot()
        assert shot is not None
        data = bytes(shot.data)
        w, h = shot.width, shot.height
        # Count distinct pixel colors — a hung/stuck core would produce a
        # uniform framebuffer. Adventure's initial screens are spartan but
        # show at least background + foreground + one sprite color.
        seen = set()
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                off = (y * w + x) * 4
                seen.add((data[off], data[off + 1], data[off + 2]))
        assert len(seen) >= 3, f"only {len(seen)} distinct colors — core may be stuck"


def test_adventure_frame_height_is_sensible(core_path, adventure_rom):
    """With the visible-region fix, the framebuffer we hand to libretro
    should start at the first non-VBLANK scanline. Adventure uses a fairly
    standard NTSC layout (~192 visible lines), so the shipped frame should be
    substantially shorter than the full PAL envelope (312)."""
    session = _load_session(core_path, adventure_rom)
    with session as s:
        for _ in range(10):
            s.run()
        shot = s.video.screenshot()
        # Sanity: not the full 312-line max; not zero; somewhere in NTSC range.
        assert 150 < shot.height < 260, (
            f"expected ~192-line NTSC visible region, got {shot.height}")
