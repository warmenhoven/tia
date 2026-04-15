/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void full_scanline(struct tia *t)
{
    int i;
    for (i = 0; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(t);
}

static int expect_range(const struct tia *t, uint16_t x_lo, uint16_t x_hi,
                        uint32_t color, const char *label)
{
    uint16_t x;
    for (x = x_lo; x <= x_hi; x++) {
        uint32_t got = t->fb[x];
        if (got != color) {
            fprintf(stderr, "%s: fb[%u] expected %08x got %08x\n",
                    label, x, color, got);
            return 1;
        }
    }
    return 0;
}

#define BG_COLOR(t)  ((t).palette[((t).colubk >> 1) & 0x7F])
#define PF_COLOR(t)  ((t).palette[((t).colupf >> 1) & 0x7F])
#define P0_COLOR(t)  ((t).palette[((t).colup0 >> 1) & 0x7F])
#define P1_COLOR(t)  ((t).palette[((t).colup1 >> 1) & 0x7F])

static void std_setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x09, 0x44);   /* COLUBK */
    tia_write(t, 0x06, 0x82);   /* COLUP0 */
    tia_write(t, 0x07, 0x24);   /* COLUP1 */
    tia_write(t, 0x08, 0x66);   /* COLUPF */
}

/* --- Basic placement via direct p0_pos --- */

static int test_grp0_all_on_draws_8_pixels(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x1B, 0xFF);  /* GRP0 = all on */
    full_scanline(&t);
    if (expect_range(&t, 0, 39,   BG_COLOR(t), "before sprite")) return 1;
    if (expect_range(&t, 40, 47,  P0_COLOR(t), "sprite body")) return 1;
    if (expect_range(&t, 48, 159, BG_COLOR(t), "after sprite")) return 1;
    return 0;
}

static int test_grp0_msb_only_is_leftmost(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x1B, 0x80);  /* bit 7 only */
    full_scanline(&t);
    ASSERT_EQ(t.fb[40], P0_COLOR(t));
    ASSERT_EQ(t.fb[41], BG_COLOR(t));
    ASSERT_EQ(t.fb[47], BG_COLOR(t));
    return 0;
}

static int test_grp0_lsb_only_is_rightmost(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x1B, 0x01);  /* bit 0 only */
    full_scanline(&t);
    ASSERT_EQ(t.fb[40], BG_COLOR(t));
    ASSERT_EQ(t.fb[46], BG_COLOR(t));
    ASSERT_EQ(t.fb[47], P0_COLOR(t));
    return 0;
}

/* --- REFP0 reflection flips horizontally --- */

static int test_refp0_flips_sprite(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x1B, 0x80);
    tia_write(&t, 0x0B, 0x08);  /* REFP0 on (bit 3) */
    full_scanline(&t);
    /* bit 7 was leftmost; now it's rightmost. */
    ASSERT_EQ(t.fb[40], BG_COLOR(t));
    ASSERT_EQ(t.fb[47], P0_COLOR(t));
    return 0;
}

/* --- NUSIZ copy modes --- */

static int test_nusiz_two_close(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x01);  /* NUSIZ0 = 2 close, 16 apart */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 20, 27, P0_COLOR(t), "copy 1")) return 1;
    if (expect_range(&t, 28, 35, BG_COLOR(t), "gap")) return 1;
    if (expect_range(&t, 36, 43, P0_COLOR(t), "copy 2")) return 1;
    return 0;
}

static int test_nusiz_two_medium(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x02);  /* 2 medium, 32 apart */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 20, 27, P0_COLOR(t), "copy 1")) return 1;
    if (expect_range(&t, 52, 59, P0_COLOR(t), "copy 2")) return 1;
    if (expect_range(&t, 28, 51, BG_COLOR(t), "gap")) return 1;
    return 0;
}

static int test_nusiz_two_wide(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x04);  /* 2 wide, 64 apart */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 20, 27, P0_COLOR(t), "copy 1")) return 1;
    if (expect_range(&t, 84, 91, P0_COLOR(t), "copy 2")) return 1;
    return 0;
}

static int test_nusiz_three_close(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x03);  /* 3 close, 16 apart */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 20, 27, P0_COLOR(t), "copy 1")) return 1;
    if (expect_range(&t, 36, 43, P0_COLOR(t), "copy 2")) return 1;
    if (expect_range(&t, 52, 59, P0_COLOR(t), "copy 3")) return 1;
    return 0;
}

static int test_nusiz_three_medium(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x06);  /* 3 medium, 32 apart */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 20, 27, P0_COLOR(t), "copy 1")) return 1;
    if (expect_range(&t, 52, 59, P0_COLOR(t), "copy 2")) return 1;
    if (expect_range(&t, 84, 91, P0_COLOR(t), "copy 3")) return 1;
    return 0;
}

/* --- NUSIZ size stretching --- */

static int test_nusiz_double_size(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x04, 0x05);  /* double size: 16-pixel sprite */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 40, 55,  P0_COLOR(t), "double body")) return 1;
    if (expect_range(&t, 56, 159, BG_COLOR(t), "after")) return 1;
    return 0;
}

static int test_nusiz_quad_size(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x04, 0x07);  /* quad size: 32-pixel sprite */
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 40, 71, P0_COLOR(t), "quad body")) return 1;
    if (expect_range(&t, 72, 159, BG_COLOR(t), "after")) return 1;
    return 0;
}

static int test_nusiz_quad_bit_granularity(void)
{
    /* In quad-size mode each GRP bit covers 4 visible pixels. bit 7 covers
     * pixels [pos..pos+3]; bit 6 covers [pos+4..pos+7]; etc. */
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x04, 0x07);
    tia_write(&t, 0x1B, 0x80);
    full_scanline(&t);
    if (expect_range(&t, 40, 43,  P0_COLOR(t), "quad bit7")) return 1;
    if (expect_range(&t, 44, 159, BG_COLOR(t), "rest")) return 1;
    return 0;
}

/* --- VDELP double-buffer --- */

static int test_vdelp0_uses_latched_gfx(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x25, 0x01);  /* VDELP0 on */
    tia_write(&t, 0x1B, 0xFF);  /* GRP0 = 0xFF — but grp0_latch is stale (0) */
    full_scanline(&t);
    if (expect_range(&t, 40, 47, BG_COLOR(t), "vdelp0 uses stale latch")) return 1;

    /* Now trigger latch by writing GRP1. grp0_latch becomes 0xFF. */
    tia_write(&t, 0x1C, 0x00);
    t.hpos = 0;
    t.scanline = 1;
    full_scanline(&t);
    if (expect_range(&t, 160, 199, BG_COLOR(t), "vdelp0 scanline 1 before sprite")) return 1;
    if (expect_range(&t, 200, 207, P0_COLOR(t), "vdelp0 after GRP1 latch")) return 1;
    return 0;
}

/* --- RESP0 positioning --- */

static int test_resp0_at_hpos_sets_p0_pos(void)
{
    /* RESP latency: the horizontal counter is reset 5 color clocks after
     * the strobe (the TIA's internal pipeline delay). Empirically verified
     * against reference renders of Adventure (yellow-castle gate). */
    struct tia t;
    int i;
    std_setup(&t);
    tia_write(&t, 0x1B, 0xFF);
    for (i = 0; i < 100; i++) tia_tick(&t);      /* hpos = 100 */
    tia_write(&t, 0x10, 0);                       /* RESP0 */
    ASSERT_EQ(t.p0_pos, 100 + 5 - TIA_HBLANK_CLOCKS);   /* 37 */
    return 0;
}

static int test_resp0_during_hblank_goes_negative(void)
{
    struct tia t;
    int i;
    std_setup(&t);
    for (i = 0; i < 20; i++) tia_tick(&t);
    tia_write(&t, 0x10, 0);
    ASSERT_TRUE(t.p0_pos < 0);
    return 0;
}

/* --- Priority --- */

static int test_p0_beats_p1(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    /* All 8 overlapping pixels should be P0 (higher priority). */
    if (expect_range(&t, 40, 47, P0_COLOR(t), "p0 over p1")) return 1;
    return 0;
}

static int test_p0_beats_playfield(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 0;                                  /* sprite at leftmost */
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);                    /* PF0 all high bits -> pf pixels 0..3 */
    full_scanline(&t);
    /* PF would want pixels 0..15 but P0 covers 0..7 — those should be P0. */
    if (expect_range(&t, 0, 7, P0_COLOR(t), "p0 over pf")) return 1;
    if (expect_range(&t, 8, 15, PF_COLOR(t), "pf alone")) return 1;
    return 0;
}

static int test_p1_beats_playfield(void)
{
    struct tia t;
    std_setup(&t);
    t.p1_pos = 0;
    tia_write(&t, 0x1C, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    if (expect_range(&t, 0, 7, P1_COLOR(t), "p1 over pf")) return 1;
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_grp0_all_on_draws_8_pixels);
    RUN_TEST(test_grp0_msb_only_is_leftmost);
    RUN_TEST(test_grp0_lsb_only_is_rightmost);
    RUN_TEST(test_refp0_flips_sprite);
    RUN_TEST(test_nusiz_two_close);
    RUN_TEST(test_nusiz_two_medium);
    RUN_TEST(test_nusiz_two_wide);
    RUN_TEST(test_nusiz_three_close);
    RUN_TEST(test_nusiz_three_medium);
    RUN_TEST(test_nusiz_double_size);
    RUN_TEST(test_nusiz_quad_size);
    RUN_TEST(test_nusiz_quad_bit_granularity);
    RUN_TEST(test_vdelp0_uses_latched_gfx);
    RUN_TEST(test_resp0_at_hpos_sets_p0_pos);
    RUN_TEST(test_resp0_during_hblank_goes_negative);
    RUN_TEST(test_p0_beats_p1);
    RUN_TEST(test_p0_beats_playfield);
    RUN_TEST(test_p1_beats_playfield);
TEST_MAIN_END
