import libretro


def test_api_version(core):
    assert core.api_version() == libretro.API_VERSION


def test_system_info(core):
    info = core.get_system_info()
    assert info.library_name.decode() == "tia"
    assert info.library_version.decode()
    exts = info.valid_extensions.decode()
    assert "a26" in exts and "bin" in exts


def test_session_runs_one_frame(core_path):
    dummy_rom = bytearray(4096)
    session = (
        libretro.SessionBuilder.defaults(core_path).with_content(dummy_rom).build()
    )
    with session as s:
        s.run()
        shot = s.video.screenshot()
        assert shot is not None
