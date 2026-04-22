/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored register-read tests.
 *
 * Source: SPG §"Reading the TIA Registers". The TIA has only 14 readable
 * registers, all at the low addresses. Specific layouts:
 *
 *   Collision regs (addr 0x00-0x07): bit 7 and bit 6 hold collision flags;
 *     bits 5-0 are NOT DRIVEN by the TIA — they float at the last data-bus
 *     value (or low-6-bit approximation thereof).
 *   INPT0-3 (addr 0x08-0x0B): bit 7 reflects paddle capacitor voltage; bits
 *     6-0 float.
 *   INPT4-5 (addr 0x0C-0x0D): bit 7 reflects digital trigger pin; bits 6-0
 *     float.
 *   Addresses 0x0E and above (up to 0x3F) are undefined — reads return the
 *   floating bus.
 *
 * On real hardware the "floating bus" reflects whatever was last driven on
 * the data bus (typically the opcode/operand of the current instruction).
 * We approximate by returning (addr & 0x3F) for undefined bits — enough to
 * make games like Haunted House work without being a full data-bus model.
 * So tests check behaviors that don't depend on specific floating-bus bits.
 */

#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

/* --- Collision register layout ---
 *
 * Documented: SPG "Reading TIA Registers" table — collision bits 7 and 6
 * ONLY. Bits 5-0 are floating. */

static int test_collision_regs_only_bits_7_6_collision(void)
{
    /* After setting up a P0-P1 collision, CXPPMM bit 7 is set. No test
     * should rely on bits 5-0 (floating). We only assert bits 7:6 here. */
    struct tia t;
    int i;
    tia_init(&t);
    t.p0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    for (i = 0; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(&t);
    /* CXPPMM at addr 0x07: bit 7 set (P0-P1). Mask to bits 7:6. */
    ASSERT_EQ(tia_read(&t, 0x07) & 0xC0, 0x80);
    return 0;
}

/* --- INPT4/5 report bit 7 only ---
 *
 * SPG §INPT4/INPT5 — "Bit 7 reflects the state of input pin (digital
 * trigger). Bits 6-0 are not connected." */

static int test_inpt4_bit7_reflects_pin(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x80;              /* pin high = not pressed */
    ASSERT_EQ(tia_read(&t, 0x0C) & 0x80, 0x80);
    t.inpt[4] = 0x00;              /* pressed */
    ASSERT_EQ(tia_read(&t, 0x0C) & 0x80, 0x00);
    return 0;
}

static int test_inpt5_bit7_reflects_pin(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[5] = 0x80;
    ASSERT_EQ(tia_read(&t, 0x0D) & 0x80, 0x80);
    t.inpt[5] = 0x00;
    ASSERT_EQ(tia_read(&t, 0x0D) & 0x80, 0x00);
    return 0;
}

/* --- VBLANK bit 7 does NOT ground INPT4/5 ---
 *
 * Hardware behavior (verified against Adventure and Donkey Kong boot
 * traces): VBLANK bit 7 is the paddle DUMP line. It grounds the paddle
 * caps (INPT0-3 → 0) but leaves INPT4/5 untouched. Donkey Kong's boot
 * code reads INPT4 early and fails if we mis-ground it.
 *
 * Note: VBLANK bit 6 is the LATCH mode for INPT4/5 (latches trigger
 * state on press, held until bit 6 is cleared). We don't currently
 * model it — if we add support later, this test should still pass
 * because we test bit 7 only. */

static int test_vblank_bit7_does_not_affect_inpt45(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x80;
    t.inpt[5] = 0x80;
    tia_write(&t, 0x01, 0x80);    /* VBLANK bit 7 = DUMP */
    tia_tick(&t);                  /* let the ground signal propagate */
    ASSERT_EQ(tia_read(&t, 0x0C) & 0x80, 0x80);  /* INPT4 unaffected */
    ASSERT_EQ(tia_read(&t, 0x0D) & 0x80, 0x80);  /* INPT5 unaffected */
    return 0;
}

/* --- VBLANK bit 7 DOES ground INPT0-3 paddle caps ---
 *
 * SPG §VBLANK bit 7 DUMP — "When set, the paddle input capacitors are
 * discharged to ground. Reading INPT0-3 returns 0 in bit 7." */

static int test_vblank_bit7_grounds_paddles(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    /* Pre-charge the paddle caps */
    for (i = 0; i < 4; i++) t.inpt[i] = 0x80;
    tia_write(&t, 0x01, 0x80);     /* DUMP on */
    tia_tick(&t);                  /* process dump this tick */
    for (i = 0; i < 4; i++) {
        uint8_t v = tia_read(&t, (uint16_t)(0x08 + i));
        if ((v & 0x80) != 0) {
            fprintf(stderr, "INPT%d not grounded: %02X\n", i, v);
            return 1;
        }
    }
    return 0;
}

/* --- Address mirroring ---
 *
 * SPG §"TIA Addressing" — "TIA reads decode on A3:A0 (16 registers).
 * All other address bits are don't-cares within the chip-select region,
 * so the 14 readable registers appear at every A3:A0 match in $00-$3F
 * (and their mirrors higher up, subject to bus decode)."
 *
 * Our bus uses 13-bit address (A12:A0), and the TIA region is selected
 * when bit 12 is 0. Within the TIA region, A3:A0 selects the register.
 * Test: reading 0x0C (INPT4) and 0x2C (same low nibble, +0x20) should
 * both return the same value. */

static int test_inpt4_mirrored_at_high_nibble(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x80;
    /* A3:A0 = 0xC selects INPT4. Any address $00-$3F with low nibble
     * 0xC should give the same bit 7. */
    ASSERT_EQ(tia_read(&t, 0x0C) & 0x80, 0x80);
    ASSERT_EQ(tia_read(&t, 0x1C) & 0x80, 0x80);
    ASSERT_EQ(tia_read(&t, 0x2C) & 0x80, 0x80);
    ASSERT_EQ(tia_read(&t, 0x3C) & 0x80, 0x80);
    return 0;
}

/* --- Write-register mirroring ---
 *
 * SPG §"TIA Addressing" — write registers decode on A5:A0 (64 address
 * slots covering all 45 writable registers plus strobes).
 *
 * Test COLUBK (write-only) at 0x09 and its mirror at 0x49 (bit 6 set).
 * Our implementation handles this via (addr & 0x3F).
 *
 * The two tia_ticks after the write are NOT part of the docs claim —
 * they drain the 1-color-clock DAC pipeline delay on COLUBK that the
 * oracle shows (and which the SPG is silent on). We still verify the
 * docs claim: that a write to $49 ultimately lands in the same register
 * as a write to $09 would. */
static int test_colubk_mirror_write(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x49, 0x82);   /* mirror of COLUBK (0x09) */
    tia_tick(&t); tia_tick(&t);  /* drain DAC pipeline (see note above) */
    ASSERT_EQ(t.colubk, 0x82);
    return 0;
}

/* --- Read addresses above 0x0D are floating-bus ---
 *
 * SPG "Reading TIA Registers" — "Addresses 0x0E through 0x3F return
 * undefined data (the floating bus)."
 *
 * Our approximation: returns (addr & 0xFF) so that games like Haunted
 * House (which does `SBC $0F`) get a non-zero value. The exact bits
 * beyond that are implementation-dependent but must be non-zero. */

static int test_read_undefined_addr_is_nonzero(void)
{
    struct tia t;
    tia_init(&t);
    /* 0x0F is write-only (PF2). A read should not be zero — it reflects
     * the floating bus. */
    ASSERT_TRUE(tia_read(&t, 0x0F) != 0);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_collision_regs_only_bits_7_6_collision);
    RUN_TEST(test_inpt4_bit7_reflects_pin);
    RUN_TEST(test_inpt5_bit7_reflects_pin);
    RUN_TEST(test_vblank_bit7_does_not_affect_inpt45);
    RUN_TEST(test_vblank_bit7_grounds_paddles);
    RUN_TEST(test_inpt4_mirrored_at_high_nibble);
    RUN_TEST(test_colubk_mirror_write);
    RUN_TEST(test_read_undefined_addr_is_nonzero);
TEST_MAIN_END
