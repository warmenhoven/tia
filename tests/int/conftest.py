import pathlib
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
