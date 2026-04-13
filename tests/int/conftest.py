import os
import pathlib
from zipfile import ZipFile

import pytest
import libretro

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CORE_PATH = REPO_ROOT / "tia_libretro.dylib"


@pytest.fixture(scope="session")
def core_path():
    if not CORE_PATH.exists():
        pytest.skip(f"core not built at {CORE_PATH}; run `make` first")
    return str(CORE_PATH)


@pytest.fixture
def core(core_path):
    return libretro.Core(core_path)


@pytest.fixture(scope="session")
def rom_dir():
    """Path to an Atari 2600 ROM collection (zips or raw .a26/.bin).

    Set ATARI_ROM_DIR env var to enable real-ROM tests."""
    env = os.environ.get("ATARI_ROM_DIR")
    if not env:
        pytest.skip("ATARI_ROM_DIR not set; skipping real-ROM tests")
    path = pathlib.Path(env).expanduser()
    if not path.is_dir():
        pytest.skip(f"ATARI_ROM_DIR={env!r} is not a directory")
    return path


def load_rom_bytes(path: pathlib.Path) -> bytes:
    """Return ROM contents as raw bytes. Handles .a26, .bin, and .zip (treating
    the first .a26/.bin entry as the ROM)."""
    path = pathlib.Path(path)
    if path.suffix.lower() == ".zip":
        with ZipFile(path) as zf:
            candidates = [n for n in zf.namelist()
                          if n.lower().endswith((".a26", ".bin"))]
            if not candidates:
                raise ValueError(f"no .a26/.bin inside {path}")
            return zf.read(candidates[0])
    return path.read_bytes()


def find_rom(rom_dir: pathlib.Path, name_substring: str) -> pathlib.Path:
    """Locate a ROM by case-insensitive substring match on the filename."""
    name_substring = name_substring.lower()
    for p in sorted(rom_dir.iterdir()):
        if name_substring in p.name.lower():
            return p
    raise FileNotFoundError(
        f"no ROM matching {name_substring!r} in {rom_dir}")
