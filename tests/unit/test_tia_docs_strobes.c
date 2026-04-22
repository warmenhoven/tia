/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored tests for TIA strobe registers.
 *
 * Source: SPG §Strobe Registers. All strobes are write-only; the value
 * written is ignored. The TIA recognizes the write address itself as the
 * trigger event.
 *
 *   $02 WSYNC  — halt CPU (RDY low) until HSYNC
 *   $03 RSYNC  — reset horizontal counter to end of scanline minus pipeline
 *   $10-14 RESx — sprite position resets (tested in test_tia_docs_respos.c)
 *   $2A HMOVE  — apply HM* deltas, start 8-clock comb
 *   $2B HMCLR  — clear all HM* registers
 *   $2C CXCLR  — clear all collision latches (tested in test_tia_docs_collisions.c)
 */

#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

/* --- WSYNC: CPU RDY stall ---
 *
 * SPG §WSYNC — "A strobe to WSYNC drives RDY low until the next HSYNC
 * (start of next scanline)." Our implementation exposes this as the
 * rdy_asserted flag polled by the bus layer. */

static int test_wsync_asserts_rdy(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 100; i++) tia_tick(&t);    /* hpos = 100 */
    tia_write(&t, 0x02, 0);                     /* WSYNC */
    ASSERT_TRUE(t.rdy_asserted);
    return 0;
}

static int test_wsync_rdy_released_at_hsync(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 100; i++) tia_tick(&t);
    tia_write(&t, 0x02, 0);
    /* Tick to the end of the scanline; RDY should release at hpos wrap. */
    for (i = 100; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(&t);
    ASSERT_EQ(t.rdy_asserted, 0);
    return 0;
}

static int test_wsync_at_hpos_0_is_noop(void)
{
    /* SPG edge case: if WSYNC is strobed when the beam is exactly at HSYNC
     * start (hpos == 0), the stall is immediately satisfied — no actual
     * stall occurs. Failing this test can manifest as sprites stretched
     * by an extra scanline in games that write WSYNC near scanline wrap
     * (Frogger's opening RAM-clear loop is a reliable repro). */
    struct tia t;
    tia_init(&t);
    /* hpos starts at 0 after init. */
    ASSERT_EQ(t.hpos, 0);
    tia_write(&t, 0x02, 0);
    ASSERT_EQ(t.rdy_asserted, 0);
    return 0;
}

/* --- RSYNC: horizontal counter reset ---
 *
 * Documented behaviour (TIA schematic / Towers die-shot analysis):
 * "RSYNC aligns the hctr to H_CLOCKS - 3, shortening the current scanline
 * to finish 3 clocks from now." Getting the direction wrong (hctr=0)
 * *extends* the scanline and breaks games that rely on RSYNC (Frogger). */

static int test_rsync_sets_hpos_near_hsync(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 120; i++) tia_tick(&t);     /* hpos = 120 */
    tia_write(&t, 0x03, 0);                      /* RSYNC */
    ASSERT_EQ(t.hpos, TIA_SCANLINE_CLOCKS - 3);  /* = 225 */
    return 0;
}

static int test_rsync_wraps_after_3_ticks(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 50; i++) tia_tick(&t);
    tia_write(&t, 0x03, 0);
    ASSERT_EQ(t.hpos, 225);
    tia_tick(&t); tia_tick(&t); tia_tick(&t);
    ASSERT_EQ(t.hpos, 0);
    return 0;
}

/* --- HMOVE: apply motion + extended HBLANK ---
 *
 * SPG §HMOVE — "A strobe to HMOVE applies the current HM* register values
 * to the corresponding object position counters, and initiates an 8-clock
 * extension of HBLANK."
 *
 * The 8-clock extension ("HMOVE comb") paints the first 8 visible pixels
 * black, which in old kernels shows as the characteristic "comb" on the
 * left edge. */

static int test_hmove_strobe_starts_8_clock_blank(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    tia_write(&t, 0x2A, 0);
    /* HMOVE is pipelined 6 clocks on real hardware before the comb
     * starts, so drain those before asserting. */
    for (i = 0; i < 6; i++) tia_tick(&t);
    ASSERT_EQ(t.hmove_blank, 8);
    return 0;
}

static int test_hmove_comb_blackens_first_8_visible(void)
{
    /* SPG: "The first 8 visible color clocks following an HMOVE strobe in
     * HBLANK display as black (HBLANK extension)." Concrete test: put a
     * sprite at visible x=0..7, strobe HMOVE in HBLANK, observe the first
     * 8 pixels render black instead of the sprite colour. */
    struct tia t;
    int i;
    uint32_t black;
    tia_init(&t);
    tia_write(&t, 0x06, 0x82);   /* COLUP0 */
    tia_write(&t, 0x09, 0x44);   /* COLUBK */
    black = t.palette[0];
    t.p0_pos = 0;
    tia_write(&t, 0x1B, 0xFF);   /* GRP0 would fill x=0..7 with P0 colour */
    for (i = 0; i < 10; i++) tia_tick(&t);  /* deep HBLANK */
    tia_write(&t, 0x2A, 0);       /* HMOVE */
    for (; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(&t);
    for (i = 0; i < 8; i++) {
        if (t.fb[i] != black) {
            fprintf(stderr, "comb: fb[%d] expected black, got %08x\n", i, t.fb[i]);
            return 1;
        }
    }
    return 0;
}

/* --- HMCLR zeroing is strobe-driven ---
 *
 * SPG §HMCLR — "A strobe to HMCLR clears HMP0, HMP1, HMM0, HMM1, and HMBL."
 * (Also tested exhaustively in test_tia_docs_hmove.c; this is a focused
 * strobe test to document the strobe mechanism.) */

static int test_hmclr_strobe_zeros_all(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x20, 0xFF);
    tia_write(&t, 0x21, 0xFF);
    tia_write(&t, 0x22, 0xFF);
    tia_write(&t, 0x23, 0xFF);
    tia_write(&t, 0x24, 0xFF);
    tia_write(&t, 0x2B, 0);
    ASSERT_EQ(t.hmp0, 0);
    ASSERT_EQ(t.hmp1, 0);
    ASSERT_EQ(t.hmm0, 0);
    ASSERT_EQ(t.hmm1, 0);
    ASSERT_EQ(t.hmbl, 0);
    return 0;
}

/* --- All strobes ignore data ---
 *
 * SPG §"Strobe Registers" — "The value written to a strobe register is
 * ignored; only the write itself is significant." */

static int test_wsync_ignores_data(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 100; i++) tia_tick(&t);
    tia_write(&t, 0x02, 0xFF);   /* data 0xFF */
    ASSERT_TRUE(t.rdy_asserted);
    tia_init(&t);
    for (i = 0; i < 100; i++) tia_tick(&t);
    tia_write(&t, 0x02, 0x00);   /* data 0x00 — same effect */
    ASSERT_TRUE(t.rdy_asserted);
    return 0;
}

static int test_cxclr_ignores_data(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    for (i = 0; i < 8; i++) t.cx[i] = 0xC0;
    tia_write(&t, 0x2C, 0xFF);   /* data ignored */
    for (i = 0; i < 8; i++) ASSERT_EQ(t.cx[i], 0);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_wsync_asserts_rdy);
    RUN_TEST(test_wsync_rdy_released_at_hsync);
    RUN_TEST(test_wsync_at_hpos_0_is_noop);
    RUN_TEST(test_rsync_sets_hpos_near_hsync);
    RUN_TEST(test_rsync_wraps_after_3_ticks);
    RUN_TEST(test_hmove_strobe_starts_8_clock_blank);
    RUN_TEST(test_hmove_comb_blackens_first_8_visible);
    RUN_TEST(test_hmclr_strobe_zeros_all);
    RUN_TEST(test_wsync_ignores_data);
    RUN_TEST(test_cxclr_ignores_data);
TEST_MAIN_END
