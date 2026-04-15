/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored tests for VDEL* (vertical-delay) double-buffering.
 *
 * Sources:
 *   SPG §VDELP0 / VDELP1 / VDELBL — "When VDEL is set, the object uses
 *     its SHADOW (latched) GRP value instead of the current GRP."
 *   SPG §GRP0/GRP1 — "Writing to GRP0 latches the current GRP1 value into
 *     GRP1's shadow register. Writing to GRP1 latches GRP0 into GRP0's
 *     shadow, AND latches ENABL into the ball's shadow."
 *   GRP0 and GRP1 writes trigger different latch sets — asymmetric by
 *   design: GRP0 write latches Player 1's pattern only; GRP1 write
 *   latches both Player 0's pattern and the ball's enable.
 *
 * This cross-object latch behavior is how Adventure, Pitfall, and other
 * games achieve smooth vertical motion with 1-scanline resolution:
 * update both sprite registers on consecutive scanlines, rely on the
 * implicit latch from the latter write to commit the former's data on
 * the next rendering pass.
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

#define BG(t)  ((t).palette[((t).colubk >> 1) & 0x7F])
#define P0(t)  ((t).palette[((t).colup0 >> 1) & 0x7F])
#define P1(t)  ((t).palette[((t).colup1 >> 1) & 0x7F])
#define PF(t)  ((t).palette[((t).colupf >> 1) & 0x7F])

static void setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x09, 0x44);
    tia_write(t, 0x06, 0x82);
    tia_write(t, 0x07, 0x24);
    tia_write(t, 0x08, 0x66);
}

/* --- VDELP0 ON: player 0 uses GRP0's shadow, not the live register.
 *
 * SPG: "When VDELP0=1, the displayed GRP0 is the value written BEFORE the
 * most recent GRP1 write." Our test: set VDELP0, write GRP0=0xFF, render
 * — nothing appears (shadow is still 0). Then write GRP1 (triggers latch),
 * render next scanline — now GRP0's shadow=0xFF, sprite renders. */

static int test_vdelp0_uses_shadow_not_live(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x25, 0x01);   /* VDELP0 */
    tia_write(&t, 0x1B, 0xFF);   /* GRP0 live = 0xFF, shadow still 0 */
    full_scanline(&t);
    ASSERT_EQ(t.fb[40], BG(t));   /* shadow is 0, sprite invisible */

    /* Writing GRP1 latches grp0 -> grp0_latch (our implementation). */
    tia_write(&t, 0x1C, 0x00);   /* triggers latch */
    t.hpos = 0; t.scanline = 1;
    full_scanline(&t);
    ASSERT_EQ(t.fb[TIA_VISIBLE_WIDTH + 40], P0(t));
    return 0;
}

/* --- VDELP1 ON: player 1 uses GRP1's shadow.
 *
 * SPG: "When VDELP1=1, the displayed GRP1 is the value written BEFORE
 * the most recent GRP0 write." */

static int test_vdelp1_uses_shadow_not_live(void)
{
    struct tia t;
    setup(&t);
    t.p1_pos = 40;
    tia_write(&t, 0x26, 0x01);   /* VDELP1 */
    tia_write(&t, 0x1C, 0xFF);   /* GRP1 live = 0xFF, shadow still 0 */
    full_scanline(&t);
    ASSERT_EQ(t.fb[40], BG(t));

    tia_write(&t, 0x1B, 0x00);   /* GRP0 write latches grp1 -> grp1_latch */
    t.hpos = 0; t.scanline = 1;
    full_scanline(&t);
    ASSERT_EQ(t.fb[TIA_VISIBLE_WIDTH + 40], P1(t));
    return 0;
}

/* --- VDELBL ON: ball uses ENABL's shadow.
 *
 * SPG: "When VDELBL=1, the ball uses the value of ENABL written BEFORE
 * the most recent GRP1 write. Writing GRP1 triggers the ENABL latch." */

static int test_vdelbl_uses_shadow_not_live(void)
{
    struct tia t;
    setup(&t);
    t.bl_pos = 50;
    tia_write(&t, 0x27, 0x01);   /* VDELBL */
    tia_write(&t, 0x1F, 0x02);   /* ENABL live = 1, shadow still 0 */
    full_scanline(&t);
    ASSERT_EQ(t.fb[50], BG(t));

    tia_write(&t, 0x1C, 0x00);   /* GRP1 write latches enabl -> enabl_latch */
    t.hpos = 0; t.scanline = 1;
    full_scanline(&t);
    ASSERT_EQ(t.fb[TIA_VISIBLE_WIDTH + 50], PF(t));
    return 0;
}

/* --- GRP1 write triggers GRP0 shadow latch (used when VDELP0 is set) ---
 *
 * SPG §GRP0 — "A write to GRP1 also triggers the latching of the current
 * GRP0 value into its vertical-delay register." */

static int test_grp1_write_latches_grp0(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x1B, 0xAA);   /* GRP0 = 0xAA */
    ASSERT_EQ(t.grp0_latch, 0);  /* not yet latched */
    tia_write(&t, 0x1C, 0x00);   /* GRP1 write: latches grp0 */
    ASSERT_EQ(t.grp0_latch, 0xAA);
    return 0;
}

/* --- GRP1 write triggers BOTH GRP0 and ENABL shadow latches ---
 *
 * SPG — "A write to GRP1 triggers latching of GRP0 AND of ENABL."
 * This dual latch enables games to use VDELP0 and VDELBL in the same
 * kernel with a single paired-register update. */

static int test_grp1_write_latches_grp0_and_enabl(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x1B, 0x55);   /* GRP0 = 0x55 */
    tia_write(&t, 0x1F, 0x02);   /* ENABL = 1 */
    /* After GRP0 write: grp1_latch may have been updated (to current grp1=0),
     * but grp0_latch and enabl_latch should not have changed. */
    ASSERT_EQ(t.grp0_latch, 0);
    ASSERT_EQ(t.enabl_latch, 0);
    tia_write(&t, 0x1C, 0x00);   /* GRP1 write triggers both */
    ASSERT_EQ(t.grp0_latch, 0x55);
    ASSERT_EQ(t.enabl_latch, 1);
    return 0;
}

/* --- VDEL off: live register used, shadow ignored ---
 *
 * Sanity check: when VDELx bit 0 is clear, the live GRP/ENABL register is
 * used and any stale shadow is ignored. */

static int test_vdelp0_off_uses_live_register(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x25, 0x00);   /* VDELP0 = 0 */
    tia_write(&t, 0x1B, 0xFF);   /* GRP0 live */
    full_scanline(&t);
    ASSERT_EQ(t.fb[40], P0(t));   /* sprite visible immediately */
    return 0;
}

static int test_vdelbl_off_uses_live_register(void)
{
    struct tia t;
    setup(&t);
    t.bl_pos = 50;
    tia_write(&t, 0x27, 0x00);   /* VDELBL = 0 */
    tia_write(&t, 0x1F, 0x02);
    full_scanline(&t);
    ASSERT_EQ(t.fb[50], PF(t));
    return 0;
}

/* --- VDEL bit 0 only; higher bits ignored ---
 *
 * SPG: "VDELPx/VDELBL use bit 0 only. Bits 7:1 are not connected." */

static int test_vdelp0_bit0_only(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x25, 0xFE);   /* bit 0 clear, rest set */
    ASSERT_EQ(t.vdelp0, 0);
    tia_write(&t, 0x25, 0x01);
    ASSERT_EQ(t.vdelp0, 1);
    tia_write(&t, 0x25, 0xFF);   /* all bits set, bit 0 included */
    ASSERT_EQ(t.vdelp0, 1);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_vdelp0_uses_shadow_not_live);
    RUN_TEST(test_vdelp1_uses_shadow_not_live);
    RUN_TEST(test_vdelbl_uses_shadow_not_live);
    RUN_TEST(test_grp1_write_latches_grp0);
    RUN_TEST(test_grp1_write_latches_grp0_and_enabl);
    RUN_TEST(test_vdelp0_off_uses_live_register);
    RUN_TEST(test_vdelbl_off_uses_live_register);
    RUN_TEST(test_vdelp0_bit0_only);
TEST_MAIN_END
