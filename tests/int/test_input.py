import libretro


def build_swcha_echo_rom() -> bytearray:
    """ROM that mirrors SWCHA's high nibble into COLUBK every frame.

    Pressing a P0 direction zeroes one of bits 4-7, so the rendered
    background color changes accordingly.
    """
    rom = bytearray(4096)
    def put(off, *bs):
        for i, b in enumerate(bs): rom[off + i] = b

    put(0x000, 0x78)                         # SEI
    put(0x001, 0xD8)                         # CLD
    put(0x002, 0xA2, 0xFF)                   # LDX #$FF
    put(0x004, 0x9A)                         # TXS

    # LOOP at $F005
    put(0x005, 0xA9, 0x02)                   # LDA #$02
    put(0x007, 0x85, 0x00)                   # STA VSYNC  (frame_ready fires here)
    put(0x009, 0x85, 0x02)                   # STA WSYNC
    put(0x00B, 0x85, 0x02)
    put(0x00D, 0x85, 0x02)
    put(0x00F, 0xA9, 0x00)                   # LDA #$00
    put(0x011, 0x85, 0x00)                   # STA VSYNC (off)
    put(0x013, 0xAD, 0x80, 0x02)             # LDA $0280   (SWCHA)
    put(0x016, 0x29, 0xF0)                   # AND #$F0
    put(0x018, 0x85, 0x09)                   # STA COLUBK
    put(0x01A, 0xA9, 0x02)                   # LDA #$02 (reload for WSYNC)
    put(0x01C, 0xA2, 0xE8)                   # LDX #$E8
    put(0x01E, 0x85, 0x02)                   # STA WSYNC
    put(0x020, 0xCA)                         # DEX
    put(0x021, 0xD0, 0xFB)                   # BNE -5
    put(0x023, 0x4C, 0x05, 0xF0)             # JMP LOOP

    rom[0xFFC] = 0x00                         # reset vec low -> $F000
    rom[0xFFD] = 0xF0
    rom[0xFFE] = 0x00                         # irq vec
    rom[0xFFF] = 0xF0
    return rom


def sample_center(shot):
    data = bytes(shot.data)
    w, h = shot.width, shot.height
    off = (h // 2 * w + w // 2) * 4
    return data[off], data[off + 1], data[off + 2]


def test_p0_left_changes_color(core_path):
    rom = build_swcha_echo_rom()
    states = [
        libretro.JoypadState(),                       # frame 1: cold boot
        libretro.JoypadState(),                       # frame 2: neutral
        libretro.JoypadState(left=True),              # frame 3: P0 LEFT
    ]
    driver = libretro.IterableInputDriver(input_generator=iter(states))
    session = (libretro.SessionBuilder
               .defaults(core_path)
               .with_content(rom)
               .with_input(driver)
               .build())
    with session as s:
        s.run()                                       # frame 1 boot
        s.run()                                       # frame 2 neutral
        neutral = sample_center(s.video.screenshot())
        s.run()                                       # frame 3 with LEFT pressed
        left = sample_center(s.video.screenshot())
    assert neutral != left, f"expected color change; both frames showed {neutral}"
    assert neutral != (0, 0, 0), "neutral frame unexpectedly black"
    assert left != (0, 0, 0), "left-pressed frame unexpectedly black"


def test_fire_button_reaches_inpt4(core_path):
    """Separate ROM that mirrors INPT4 (P0 fire) into COLUBK."""
    rom = bytearray(4096)
    def put(off, *bs):
        for i, b in enumerate(bs): rom[off + i] = b

    put(0x000, 0x78, 0xD8, 0xA2, 0xFF, 0x9A)  # boot
    # LOOP at $F005
    put(0x005, 0xA9, 0x02)                    # LDA #$02
    put(0x007, 0x85, 0x00)                    # STA VSYNC
    put(0x009, 0x85, 0x02, 0x85, 0x02, 0x85, 0x02)  # 3 WSYNCs
    put(0x00F, 0xA9, 0x00, 0x85, 0x00)        # VSYNC off
    # Read INPT4 and mask away the floating-bus low bits before stashing
    # it in COLUBK. INPT4 read is (pin_state<<7) | (addr_lo & 0x7F), so
    # without AND #$80 we'd get $8C/$0C instead of $80/$00.
    put(0x013, 0xA5, 0x0C)                    # LDA $0C (INPT4)
    put(0x015, 0x29, 0x80)                    # AND #$80
    put(0x017, 0x85, 0x09)                    # STA COLUBK
    put(0x019, 0xA9, 0x02, 0xA2, 0xE8)        # LDA #$02, LDX #$E8
    put(0x01D, 0x85, 0x02, 0xCA, 0xD0, 0xFB)  # 232-line WSYNC loop
    put(0x022, 0x4C, 0x05, 0xF0)              # JMP LOOP

    rom[0xFFC] = 0x00; rom[0xFFD] = 0xF0
    rom[0xFFE] = 0x00; rom[0xFFF] = 0xF0

    states = [
        libretro.JoypadState(),
        libretro.JoypadState(),                       # neutral
        libretro.JoypadState(b=True),                 # fire pressed
    ]
    driver = libretro.IterableInputDriver(input_generator=iter(states))
    session = (libretro.SessionBuilder
               .defaults(core_path)
               .with_content(rom)
               .with_input(driver)
               .build())
    with session as s:
        s.run()                                       # boot
        s.run()                                       # neutral: INPT4 = 0x80 -> COLUBK = 0x80
        neutral = sample_center(s.video.screenshot())
        s.run()                                       # fire: INPT4 = 0x00 -> COLUBK = 0x00 (black)
        fire = sample_center(s.video.screenshot())
    assert fire == (0, 0, 0), f"fire frame should be black (COLUBK=0), got {fire}"
    assert neutral != (0, 0, 0), f"neutral frame should not be black, got {neutral}"


def _build_swchb_echo_rom() -> bytearray:
    """ROM that echoes SWCHB to COLUBK every frame. Lets integration tests
    observe the console-switch latches via pixel colour."""
    rom = bytearray(4096)
    def put(off, *bs):
        for i, b in enumerate(bs): rom[off + i] = b
    put(0x000, 0x78, 0xD8, 0xA2, 0xFF, 0x9A)  # SEI, CLD, LDX #$FF, TXS
    # LOOP at $F005
    put(0x005, 0xA9, 0x02, 0x85, 0x00)        # VSYNC on
    put(0x009, 0x85, 0x02, 0x85, 0x02, 0x85, 0x02)
    put(0x00F, 0xA9, 0x00, 0x85, 0x00)        # VSYNC off
    put(0x013, 0xAD, 0x82, 0x02)              # LDA $0282 (SWCHB)
    put(0x016, 0x85, 0x09)                    # STA COLUBK
    put(0x018, 0xA9, 0x02, 0xA2, 0xE8)        # LDA #$02, LDX #$E8
    put(0x01C, 0x85, 0x02, 0xCA, 0xD0, 0xFB)  # WSYNC loop
    put(0x021, 0x4C, 0x05, 0xF0)              # JMP LOOP
    rom[0xFFC] = 0x00; rom[0xFFD] = 0xF0
    rom[0xFFE] = 0x00; rom[0xFFF] = 0xF0
    return rom


def test_console_switches_toggle_on_shoulder_buttons(core_path):
    """Per the common 2600 overlay, the left-difficulty latch is driven by
    L (set to A) and R (set to B) — an adjacent row-pair, upper/lower.
    Pressing once flips the latch; holding doesn't chatter it."""
    rom = _build_swchb_echo_rom()
    states = [
        libretro.JoypadState(),            # 0 boot
        libretro.JoypadState(),            # 1 neutral -> default (Diff B/B + color)
        libretro.JoypadState(l=True),      # 2 press L -> Left Diff A (bit 6 set)
        libretro.JoypadState(l=True),      # 3 still held: no change
        libretro.JoypadState(),            # 4 released
        libretro.JoypadState(r=True),      # 5 press R -> Left Diff B (bit 6 clear)
    ]
    driver = libretro.IterableInputDriver(input_generator=iter(states))
    session = (libretro.SessionBuilder
               .defaults(core_path)
               .with_content(rom)
               .with_input(driver)
               .build())
    def swchb_from_center(shot):
        # COLUBK bit layout: the TIA palette maps (COLUBK>>1)&0x7F to a
        # colour. We reverse-engineer by picking a unique COLUBK per
        # expected SWCHB value via a direct screenshot->hue lookup. Since
        # the test only needs to assert *different* / *same*, compare the
        # centre pixel triples directly.
        data = bytes(shot.data)
        w, h = shot.width, shot.height
        off = (h // 2 * w + w // 2) * 4
        return data[off], data[off + 1], data[off + 2]
    with session as s:
        s.run()                                       # boot
        s.run()                                       # neutral
        neutral   = swchb_from_center(s.video.screenshot())
        s.run()                                       # L pressed
        l_pressed = swchb_from_center(s.video.screenshot())
        s.run()                                       # still held
        l_held    = swchb_from_center(s.video.screenshot())
        s.run()                                       # released
        l_released = swchb_from_center(s.video.screenshot())
        s.run()                                       # R pressed
        r_pressed = swchb_from_center(s.video.screenshot())
    assert l_pressed != neutral,   "L press should flip left-diff latch to A"
    assert l_held == l_pressed,    "holding L should not chatter the latch"
    assert l_released == l_pressed, "releasing L should not flip back"
    assert r_pressed == neutral,   "R press should restore left-diff to B"
