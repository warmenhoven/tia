"""Core-option integration tests.

Each test passes a specific options dict through with_options() and verifies
the option took effect — either via AV info (region/crop geometry), via
retro_get_region, or via a probe ROM that exposes internal state through
the pixel stream (e.g. SWCHB echoed to COLUBK for switch initial-position
checks)."""
import pathlib

import libretro

from test_input import _build_swchb_echo_rom


CORE = str(pathlib.Path(__file__).resolve().parents[2] / "tia_libretro.dylib")


def _run_with_options(rom, options, frames=15):
    """Boot a ROM with the given options dict and return the session
    wrapped in a context manager. Callers drive s.run() themselves."""
    builder = (libretro.SessionBuilder
               .defaults(CORE)
               .with_content(rom)
               .with_options(options))
    return builder.build()


def _center(shot):
    data = bytes(shot.data)
    w, h = shot.width, shot.height
    off = (h // 2 * w + w // 2) * 4
    return data[off], data[off + 1], data[off + 2]


# --- Region option ---

def test_region_force_pal_sets_timing(core_path):
    """Forcing tia_region=pal should report PAL fps / sample rate / height
    regardless of the ROM's scanline count."""
    rom = bytearray(4096)   # empty ROM is enough — we only read av_info
    with _run_with_options(rom, {"tia_region": "pal"}) as s:
        s.run()
        av = s.core.get_system_av_info()
        assert av.timing.fps == 50.0
        assert av.timing.sample_rate == 31200.0
        assert av.geometry.base_height == 274
        assert s.core.get_region() == 1   # RETRO_REGION_PAL


def test_region_force_ntsc_sets_timing(core_path):
    rom = bytearray(4096)
    with _run_with_options(rom, {"tia_region": "ntsc"}) as s:
        s.run()
        av = s.core.get_system_av_info()
        assert av.timing.fps == 60.0
        assert av.timing.sample_rate == 31400.0
        assert av.geometry.base_height == 228
        assert s.core.get_region() == 0


# --- Palette variant ---

def test_palette_variant_changes_pixel_color(core_path):
    """Load the SWCHB echo ROM (which paints the whole scanline with a
    non-zero COLUBK value derived from SWCHB) in NTSC mode under both
    palette variants and verify the rendered colour differs."""
    rom = _build_swchb_echo_rom()
    base = {"tia_region": "ntsc"}
    with _run_with_options(rom, dict(base, tia_palette="standard")) as s:
        for _ in range(6): s.run()
        standard = _center(s.video.screenshot())
    with _run_with_options(rom, dict(base, tia_palette="z26")) as s:
        for _ in range(6): s.run()
        z26 = _center(s.video.screenshot())
    assert standard != z26, f"palette variants should render differently, both got {standard}"


# --- Difficulty + TV-type initial position ---

def _swchb_bit_state(rom, options, n_frames=6):
    """Run the SWCHB echo ROM and return the observed COLUBK centre pixel.
    Callers compare centre-pixel triples for equality/difference."""
    with _run_with_options(rom, options) as s:
        for _ in range(n_frames): s.run()
        return _center(s.video.screenshot())


def test_left_diff_a_overrides_default(core_path):
    """Default is B; forcing A via the option should paint a different COLUBK."""
    rom = _build_swchb_echo_rom()
    default_b = _swchb_bit_state(rom, {})
    forced_a  = _swchb_bit_state(rom, {"tia_left_diff": "a"})
    assert default_b != forced_a, f"left-diff B default vs A override should paint different COLUBK, both got {default_b}"


def test_right_diff_b_overrides_default(core_path):
    rom = _build_swchb_echo_rom()
    a = _swchb_bit_state(rom, {"tia_right_diff": "a"})
    b = _swchb_bit_state(rom, {"tia_right_diff": "b"})
    assert a != b, f"right-diff A vs B should paint different COLUBK, both got {a}"


def test_color_bw_overrides_default(core_path):
    rom = _build_swchb_echo_rom()
    color = _swchb_bit_state(rom, {})
    bw    = _swchb_bit_state(rom, {"tia_color": "bw"})
    assert color != bw, f"color vs B&W should paint different COLUBK, both got {color}"


# --- Crop options ---

def test_crop_hoverscan_shrinks_width(core_path):
    rom = bytearray(4096)
    with _run_with_options(rom, {"tia_crop_hoverscan": "off"}) as s:
        s.run()
        assert s.core.get_system_av_info().geometry.base_width == 160
    with _run_with_options(rom, {"tia_crop_hoverscan": "on"}) as s:
        s.run()
        assert s.core.get_system_av_info().geometry.base_width == 144


def test_crop_voverscan_shrinks_height(core_path):
    rom = bytearray(4096)
    # Default (0) → full NTSC height 228.
    with _run_with_options(rom, {"tia_region": "ntsc", "tia_crop_voverscan": "0"}) as s:
        s.run()
        assert s.core.get_system_av_info().geometry.base_height == 228
    # voverscan=10 → top 10 + bottom 10 trimmed → 208.
    with _run_with_options(rom, {"tia_region": "ntsc", "tia_crop_voverscan": "10"}) as s:
        s.run()
        assert s.core.get_system_av_info().geometry.base_height == 208
    # Sanity on PAL: 274 - 2*12 = 250.
    with _run_with_options(rom, {"tia_region": "pal", "tia_crop_voverscan": "12"}) as s:
        s.run()
        assert s.core.get_system_av_info().geometry.base_height == 250
