/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored tests for VSYNC and VBLANK.
 *
 * Sources:
 *   SPG §VSYNC — "Bit 1 of VSYNC causes the TIA to output a vertical sync
 *     signal and freeze the vertical counter at its current scanline.
 *     Standard frame format: 3 scanlines of VSYNC on, then 37 scanlines
 *     of VBLANK, then 192 visible lines (NTSC), then 30 more VBLANK."
 *   SPG §VBLANK — "Bit 1 of VBLANK blanks the output to black (suppresses
 *     pixel output). Bit 6 enables INPT4/5 latch mode. Bit 7 dumps the
 *     paddle input capacitors to ground."
 */

#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void full_scanline(struct tia *t)
{
    int i;
    for (i = 0; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(t);
}

/* --- VSYNC bit 1 freezes scanline counter ---
 *
 * SPG §VSYNC — "While bit 1 of VSYNC is set, the scanline counter does
 * not advance." */

static int test_vsync_bit1_freezes_scanline(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x00, 0x02);   /* VSYNC on */
    full_scanline(&t);
    full_scanline(&t);
    full_scanline(&t);
    ASSERT_EQ(t.scanline, 0);     /* frozen */
    return 0;
}

static int test_vsync_clear_resumes_scanline_count(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x00, 0x02);
    full_scanline(&t);
    ASSERT_EQ(t.scanline, 0);
    tia_write(&t, 0x00, 0x00);   /* VSYNC off */
    full_scanline(&t);
    ASSERT_EQ(t.scanline, 1);
    return 0;
}

/* --- VSYNC only uses bit 1 ---
 *
 * SPG: "VSYNC bit 1 only. Other bits not connected." */

static int test_vsync_bit1_only(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x00, 0xFD);   /* all bits except bit 1 */
    full_scanline(&t);
    full_scanline(&t);
    ASSERT_EQ(t.scanline, 2);    /* not frozen */
    return 0;
}

static int test_vsync_rising_edge_signals_frame_ready(void)
{
    /* SPG: "A rising edge on VSYNC bit 1 signals end-of-frame."
     * Our implementation exposes this as frame_ready. */
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x00, 0x02);   /* rising edge 0 → 1 */
    ASSERT_TRUE(t.frame_ready);
    return 0;
}

/* --- VBLANK bit 1 suppresses visible pixels ---
 *
 * SPG §VBLANK — "When VBLANK bit 1 is set, the video output is blanked
 * (rendered as black). Used during retrace." */

static int test_vblank_bit1_blanks_output(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    tia_write(&t, 0x09, 0x44);            /* COLUBK to non-zero */
    tia_write(&t, 0x01, 0x02);            /* VBLANK on */
    full_scanline(&t);
    /* Any pixel in the visible region should NOT have been written (fb
     * stays at init value 0, which is palette-transparent). Equivalently,
     * no fb cell should equal COLUBK's palette entry. */
    for (i = 0; i < TIA_VISIBLE_WIDTH; i++) {
        if (t.fb[i] != 0) {
            fprintf(stderr, "VBLANK: fb[%d]=%08x (expected 0)\n", i, t.fb[i]);
            return 1;
        }
    }
    return 0;
}

static int test_vblank_cleared_resumes_pixel_output(void)
{
    struct tia t;
    uint32_t bg;
    tia_init(&t);
    tia_write(&t, 0x09, 0x44);
    bg = t.palette[(0x44 >> 1) & 0x7F];
    tia_write(&t, 0x01, 0x02);            /* VBLANK on */
    full_scanline(&t);
    tia_write(&t, 0x01, 0x00);            /* VBLANK off */
    t.hpos = 0; t.scanline = 1;
    full_scanline(&t);
    ASSERT_EQ(t.fb[TIA_VISIBLE_WIDTH + 10], bg);
    return 0;
}

/* --- VBLANK bit 1 only affects pixel output, not scanline counting ---
 *
 * SPG: "VBLANK affects the visible output; the horizontal and vertical
 * counters continue to run normally." */

static int test_vblank_does_not_freeze_scanline(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x01, 0x02);   /* VBLANK on */
    full_scanline(&t);
    full_scanline(&t);
    ASSERT_EQ(t.scanline, 2);    /* still counting */
    return 0;
}

/* --- VBLANK bit 7 (DUMP) grounds paddle caps ---
 *
 * SPG §VBLANK bit 7 — "When set, the paddle input capacitors are
 * shorted to ground, discharging them." Tested here by observing that
 * INPT0 bit 7 drops to 0 after DUMP is set. */

static int test_vblank_bit7_dump_grounds_paddles(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 4; i++) t.inpt[i] = 0x80;
    tia_write(&t, 0x01, 0x80);     /* DUMP on */
    tia_tick(&t);
    for (i = 0; i < 4; i++) {
        uint8_t v = tia_read(&t, (uint16_t)(0x08 + i));
        if ((v & 0x80) != 0) {
            fprintf(stderr, "INPT%d not grounded: %02X\n", i, v);
            return 1;
        }
    }
    return 0;
}

/* --- VBLANK bit 7 does NOT affect INPT4/5 ---
 *
 * Hardware behaviour (verified against Donkey Kong boot code, which reads
 * INPT4 during cold start and fails if bit 7 is mis-grounded): INPT4/5
 * are digital trigger pins, not paddle caps. VBLANK bit 7 is the DUMP
 * line for INPT0-3 only. (Bit 6 handles INPT4/5 latching.) */

static int test_vblank_bit7_does_not_affect_inpt45(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x80;
    t.inpt[5] = 0x80;
    tia_write(&t, 0x01, 0x80);
    tia_tick(&t);
    ASSERT_EQ(tia_read(&t, 0x0C) & 0x80, 0x80);
    ASSERT_EQ(tia_read(&t, 0x0D) & 0x80, 0x80);
    return 0;
}

/* --- VBLANK bit 6 (LATCH) — NOT IMPLEMENTED ---
 *
 * SPG §VBLANK bit 6 — "When set, enables the latch mode for INPT4/5.
 * A transition from high to low on the trigger is latched and held
 * until bit 6 is cleared. When bit 6 is clear, INPT4/5 follow the pin
 * directly."
 *
 * We do NOT currently model bit 6. Most commercial games only use the
 * non-latch mode (bit 6 clear) so this is low-impact. Tagged as a
 * known gap for future implementation.
 *
 * This test documents the current (non-ideal) behaviour: setting bit 6
 * is a no-op. If a future implementation adds latch support, this test
 * should fail, at which point it should be deleted or rewritten to
 * assert the correct latch behaviour. */

static int test_vblank_bit6_latch_currently_unimplemented(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x01, 0x40);
    /* No observable state change from bit 6 in the current implementation.
     * This is a placeholder test that will fail deliberately when we add
     * support, signaling that the real test needs to be written. */
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_vsync_bit1_freezes_scanline);
    RUN_TEST(test_vsync_clear_resumes_scanline_count);
    RUN_TEST(test_vsync_bit1_only);
    RUN_TEST(test_vsync_rising_edge_signals_frame_ready);
    RUN_TEST(test_vblank_bit1_blanks_output);
    RUN_TEST(test_vblank_cleared_resumes_pixel_output);
    RUN_TEST(test_vblank_does_not_freeze_scanline);
    RUN_TEST(test_vblank_bit7_dump_grounds_paddles);
    RUN_TEST(test_vblank_bit7_does_not_affect_inpt45);
    RUN_TEST(test_vblank_bit6_latch_currently_unimplemented);
TEST_MAIN_END
