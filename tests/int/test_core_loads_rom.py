import libretro


def build_solid_color_rom(color_byte: int) -> bytearray:
    """Hand-assembled 4K ROM: sets COLUBK to `color_byte` and loops on VSYNC.

    Layout (base $F000):
        $000: SEI, CLD, LDX #$FF, TXS           (stack init)
        $005: LDA #color ; STA $09              (COLUBK)
        $009: LOOP:
              LDA #$02 ; STA $00                (VSYNC on -> frame boundary)
              STA $02 x3                        (3 VSYNC scanlines)
              LDA #$00 ; STA $00                (VSYNC off)
              LDA #$02 ; LDX #$E8               (preload A=$02 for WSYNC, 232 lines)
              (inner) STA $02 ; DEX ; BNE       (WSYNC loop)
              JMP LOOP                          (back to $F009)
        $FFC: reset vector = $F000
    """
    rom = bytearray(4096)
    def put(off, *bytes_):
        for i, b in enumerate(bytes_):
            rom[off + i] = b

    put(0x000, 0x78)                      # SEI
    put(0x001, 0xD8)                      # CLD
    put(0x002, 0xA2, 0xFF)                # LDX #$FF
    put(0x004, 0x9A)                      # TXS
    put(0x005, 0xA9, color_byte)          # LDA #color
    put(0x007, 0x85, 0x09)                # STA $09 (COLUBK)

    # LOOP at $F009
    put(0x009, 0xA9, 0x02)                # LDA #$02
    put(0x00B, 0x85, 0x00)                # STA $00 (VSYNC on)
    put(0x00D, 0x85, 0x02)                # STA $02 WSYNC
    put(0x00F, 0x85, 0x02)                # STA $02
    put(0x011, 0x85, 0x02)                # STA $02 (3 vsync scanlines)

    put(0x013, 0xA9, 0x00)                # LDA #$00
    put(0x015, 0x85, 0x00)                # STA $00 (VSYNC off)

    put(0x017, 0xA9, 0x02)                # LDA #$02  (value for later WSYNCs)
    put(0x019, 0xA2, 0xE8)                # LDX #$E8  (232 lines)
    # inner at $F01B
    put(0x01B, 0x85, 0x02)                # STA $02 (WSYNC)
    put(0x01D, 0xCA)                      # DEX
    put(0x01E, 0xD0, 0xFB)                # BNE -5 -> $F01B
    put(0x020, 0x4C, 0x09, 0xF0)          # JMP $F009

    # reset vector
    rom[0xFFC] = 0x00
    rom[0xFFD] = 0xF0
    # IRQ vector (unused on 6507)
    rom[0xFFE] = 0x00
    rom[0xFFF] = 0xF0
    return rom


def test_core_renders_colubk(core_path):
    """End-to-end: build a ROM that fills the screen with a solid color,
    load it via libretro.py, run two frames (first is cold-boot empty,
    second is the painted frame), and verify the framebuffer is the right color."""
    rom = build_solid_color_rom(0x44)           # hue 4, luma 2 -> palette[0x22]
    session = libretro.SessionBuilder.defaults(core_path).with_content(rom).build()
    with session as s:
        s.run()                                  # cold boot -> first VSYNC
        s.run()                                  # second run paints a full frame
        shot = s.video.screenshot()
        assert shot is not None

        # Screenshot is RGBA byte order; palette[0x22] is 0xB03C3C.
        data = bytes(shot.data)
        w, h = shot.width, shot.height
        # Sample a pixel in the middle of the visible area (avoiding edges
        # where HMOVE comb or late-settling effects could land).
        x, y = w // 2, h // 2
        off = (y * w + x) * 4
        r, g, b, a = data[off], data[off + 1], data[off + 2], data[off + 3]
        assert (r, g, b) == (0xB0, 0x3C, 0x3C), \
            f"expected COLUBK-rendered pixel at ({x},{y})=(0xB0,0x3C,0x3C), got ({r:#x},{g:#x},{b:#x})"


def test_different_colubk_produces_different_color(core_path):
    """Sanity check: changing the COLUBK byte produces a different rendered color."""
    rom = build_solid_color_rom(0x94)            # different hue/luma
    session = libretro.SessionBuilder.defaults(core_path).with_content(rom).build()
    with session as s:
        s.run(); s.run()
        shot = s.video.screenshot()
        data = bytes(shot.data)
        w, h = shot.width, shot.height
        off = (h // 2 * w + w // 2) * 4
        # Just verify it's not the $44 color from the other test.
        assert (data[off], data[off + 1], data[off + 2]) != (0xB0, 0x3C, 0x3C)
        # And it's not black.
        assert (data[off], data[off + 1], data[off + 2]) != (0, 0, 0)
