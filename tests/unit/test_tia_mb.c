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
    tia_write(t, 0x09, 0x44);
    tia_write(t, 0x06, 0x82);
    tia_write(t, 0x07, 0x24);
    tia_write(t, 0x08, 0x66);
}

/* --- Missiles: enable and size --- */

static int test_missile_1px_default(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x1D, 0x02);     /* ENAM0 = 1 */
    full_scanline(&t);
    if (expect_range(&t, 0, 29,  BG_COLOR(t), "before")) return 1;
    ASSERT_EQ(t.fb[30], P0_COLOR(t));
    if (expect_range(&t, 31, 159, BG_COLOR(t), "after")) return 1;
    return 0;
}

static int test_missile_size_2(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x10);     /* NUSIZ0 size bits = 01 -> 2px */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 30, 31, P0_COLOR(t), "size 2")) return 1;
    ASSERT_EQ(t.fb[32], BG_COLOR(t));
    return 0;
}

static int test_missile_size_4(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x20);     /* size bits = 10 -> 4px */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 30, 33, P0_COLOR(t), "size 4")) return 1;
    ASSERT_EQ(t.fb[34], BG_COLOR(t));
    return 0;
}

static int test_missile_size_8(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x30);     /* size bits = 11 -> 8px */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 30, 37, P0_COLOR(t), "size 8")) return 1;
    ASSERT_EQ(t.fb[38], BG_COLOR(t));
    return 0;
}

static int test_missile_disabled(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x1D, 0x00);
    full_scanline(&t);
    if (expect_range(&t, 0, 159, BG_COLOR(t), "disabled")) return 1;
    return 0;
}

/* Missile participates in NUSIZ copies (same copies as paired player). */
static int test_missile_nusiz_two_close(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x01);     /* two close, 16 apart, 1px */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    ASSERT_EQ(t.fb[30], P0_COLOR(t));
    ASSERT_EQ(t.fb[46], P0_COLOR(t));
    ASSERT_EQ(t.fb[31], BG_COLOR(t));
    return 0;
}

/* --- RESMP0: lock + hide --- */

static int test_resmp0_hides_missile(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    t.m0_pos = 80;                  /* elsewhere initially */
    tia_write(&t, 0x1D, 0x02);
    tia_write(&t, 0x28, 0x02);      /* RESMP0 */
    full_scanline(&t);
    if (expect_range(&t, 0, 159, BG_COLOR(t), "resmp hides")) return 1;
    /* Missile pos should have been slaved to p0_pos + 4 during ticking. */
    ASSERT_EQ(t.m0_pos, 44);
    return 0;
}

static int test_resmp0_release_keeps_position(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x28, 0x02);      /* lock */
    full_scanline(&t);
    ASSERT_EQ(t.m0_pos, 44);
    tia_write(&t, 0x28, 0x00);      /* release */
    tia_write(&t, 0x1D, 0x02);      /* enable missile */
    t.hpos = 0; t.scanline = 1;
    full_scanline(&t);
    /* Missile now visible at last locked position (44). */
    ASSERT_EQ(t.fb[TIA_VISIBLE_WIDTH + 44], P0_COLOR(t));
    return 0;
}

/* --- RESM0 strobe --- */

static int test_resm0_sets_pos(void)
{
    struct tia t;
    int i;
    std_setup(&t);
    for (i = 0; i < 80; i++) tia_tick(&t);
    tia_write(&t, 0x12, 0);         /* RESM0 */
    ASSERT_EQ(t.m0_pos, 80 + 4 - TIA_HBLANK_CLOCKS);  /* 16 */
    return 0;
}

/* --- Ball: enable, size, color --- */

static int test_ball_1px_uses_colupf(void)
{
    struct tia t;
    std_setup(&t);
    t.bl_pos = 50;
    tia_write(&t, 0x1F, 0x02);      /* ENABL = 1 */
    full_scanline(&t);
    ASSERT_EQ(t.fb[50], PF_COLOR(t));
    ASSERT_EQ(t.fb[51], BG_COLOR(t));
    return 0;
}

static int test_ball_size_4(void)
{
    struct tia t;
    std_setup(&t);
    t.bl_pos = 50;
    tia_write(&t, 0x0A, 0x20);      /* CTRLPF bits 4-5 = 10 -> 4px */
    tia_write(&t, 0x1F, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 50, 53, PF_COLOR(t), "ball 4")) return 1;
    ASSERT_EQ(t.fb[54], BG_COLOR(t));
    return 0;
}

static int test_ball_disabled(void)
{
    struct tia t;
    std_setup(&t);
    t.bl_pos = 50;
    full_scanline(&t);
    if (expect_range(&t, 0, 159, BG_COLOR(t), "ball off")) return 1;
    return 0;
}

/* --- VDELBL --- */

static int test_vdelbl_uses_latched_enable(void)
{
    struct tia t;
    std_setup(&t);
    t.bl_pos = 50;
    tia_write(&t, 0x27, 0x01);      /* VDELBL on */
    tia_write(&t, 0x1F, 0x02);      /* ENABL on — but enabl_latch still false */
    full_scanline(&t);
    ASSERT_EQ(t.fb[50], BG_COLOR(t)); /* stale latch -> no ball */

    tia_write(&t, 0x1C, 0x00);      /* write GRP1 → latches ENABL */
    t.hpos = 0; t.scanline = 1;
    full_scanline(&t);
    ASSERT_EQ(t.fb[TIA_VISIBLE_WIDTH + 50], PF_COLOR(t));
    return 0;
}

/* --- Priority interactions --- */

static int test_missile_beats_playfield(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 0;
    tia_write(&t, 0x04, 0x30);      /* 8px missile */
    tia_write(&t, 0x1D, 0x02);      /* enable */
    tia_write(&t, 0x0D, 0xF0);      /* PF0 pixels 0..3 */
    full_scanline(&t);
    /* Missile covers pixels 0..7; playfield wants 0..15. P0/M0 priority wins. */
    if (expect_range(&t, 0, 7, P0_COLOR(t), "m0 over pf")) return 1;
    if (expect_range(&t, 8, 15, PF_COLOR(t), "pf beyond missile")) return 1;
    return 0;
}

static int test_missile_beats_opposing_player(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 40;
    t.p1_pos = 40;
    tia_write(&t, 0x1D, 0x02);      /* M0 enable */
    tia_write(&t, 0x1C, 0xFF);      /* GRP1 all on */
    full_scanline(&t);
    ASSERT_EQ(t.fb[40], P0_COLOR(t));   /* M0 wins priority over P1 */
    ASSERT_EQ(t.fb[41], P1_COLOR(t));   /* where only P1 draws */
    return 0;
}

static int test_ball_beats_bg_not_player(void)
{
    struct tia t;
    std_setup(&t);
    t.bl_pos = 40;
    t.p0_pos = 40;
    tia_write(&t, 0x0A, 0x30);      /* 8px ball */
    tia_write(&t, 0x1F, 0x02);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    /* Ball overlapping with P0 — P0 wins. */
    if (expect_range(&t, 40, 47, P0_COLOR(t), "p0 over ball")) return 1;
    return 0;
}

/* --- Serialization round-trip --- */

static int test_serialize_roundtrip_mb(void)
{
    struct tia a, b;
    uint8_t buf[256];
    size_t sz = tia_serialize_size();
    ASSERT_TRUE(sz <= sizeof(buf));

    std_setup(&a);
    a.m0_pos = 30; a.m1_pos = 50; a.bl_pos = 70;
    a.hmm0 = 0x20; a.hmm1 = 0x30; a.hmbl = 0x10;
    tia_write(&a, 0x1D, 0x02);
    tia_write(&a, 0x1F, 0x02);
    tia_write(&a, 0x27, 0x01);       /* VDELBL */
    tia_write(&a, 0x28, 0x02);       /* RESMP0 */
    tia_write(&a, 0x1C, 0x00);       /* trigger latch */

    tia_serialize(&a, buf);
    std_setup(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.m0_pos, a.m0_pos);
    ASSERT_EQ(b.m1_pos, a.m1_pos);
    ASSERT_EQ(b.bl_pos, a.bl_pos);
    ASSERT_EQ(b.hmm0, a.hmm0);
    ASSERT_EQ(b.hmm1, a.hmm1);
    ASSERT_EQ(b.hmbl, a.hmbl);
    ASSERT_EQ(b.enam0, a.enam0);
    ASSERT_EQ(b.enabl, a.enabl);
    ASSERT_EQ(b.enabl_latch, a.enabl_latch);
    ASSERT_EQ(b.vdelbl, a.vdelbl);
    ASSERT_EQ(b.resmp0, a.resmp0);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_missile_1px_default);
    RUN_TEST(test_missile_size_2);
    RUN_TEST(test_missile_size_4);
    RUN_TEST(test_missile_size_8);
    RUN_TEST(test_missile_disabled);
    RUN_TEST(test_missile_nusiz_two_close);
    RUN_TEST(test_resmp0_hides_missile);
    RUN_TEST(test_resmp0_release_keeps_position);
    RUN_TEST(test_resm0_sets_pos);
    RUN_TEST(test_ball_1px_uses_colupf);
    RUN_TEST(test_ball_size_4);
    RUN_TEST(test_ball_disabled);
    RUN_TEST(test_vdelbl_uses_latched_enable);
    RUN_TEST(test_missile_beats_playfield);
    RUN_TEST(test_missile_beats_opposing_player);
    RUN_TEST(test_ball_beats_bg_not_player);
    RUN_TEST(test_serialize_roundtrip_mb);
TEST_MAIN_END
