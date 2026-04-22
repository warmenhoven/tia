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

/* Issue an HMOVE strobe and drain the 6-clock pipeline so the motion
 * deltas are applied to the sprite positions before the caller asserts. */
static void hmove_and_drain(struct tia *t)
{
    int i;
    tia_write(t, 0x2A, 0);
    for (i = 0; i < 6; i++) tia_tick(t);
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
#define P0_COLOR(t)  ((t).palette[((t).colup0 >> 1) & 0x7F])
#define BLACK(t)     ((t).palette[0])

static void std_setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x09, 0x44);
    tia_write(t, 0x06, 0x82);
    tia_write(t, 0x07, 0x24);
    tia_write(t, 0x08, 0x66);
}

/* --- Motion decoding and application --- */

static int test_hmove_p0_positive_moves_left(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x20, 0x70);    /* HMP0 = +7 -> move left by 7 */
    hmove_and_drain(&t);
    ASSERT_EQ(t.p0_pos, 33);
    return 0;
}

static int test_hmove_p0_negative_moves_right(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x20, 0x80);    /* HMP0 = -8 -> move right by 8 */
    hmove_and_drain(&t);
    ASSERT_EQ(t.p0_pos, 48);
    return 0;
}

static int test_hmove_p0_minus_one(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x20, 0xF0);    /* HMP0 = -1 -> move right by 1 */
    hmove_and_drain(&t);
    ASSERT_EQ(t.p0_pos, 41);
    return 0;
}

static int test_hmove_zero_no_motion(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x20, 0x00);
    hmove_and_drain(&t);
    ASSERT_EQ(t.p0_pos, 40);
    return 0;
}

static int test_hmove_applies_to_all_objects(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 50; t.p1_pos = 50; t.m0_pos = 50; t.m1_pos = 50; t.bl_pos = 50;
    tia_write(&t, 0x20, 0x30);    /* HMP0 +3 */
    tia_write(&t, 0x21, 0xE0);    /* HMP1 -2 */
    tia_write(&t, 0x22, 0x10);    /* HMM0 +1 */
    tia_write(&t, 0x23, 0xF0);    /* HMM1 -1 */
    tia_write(&t, 0x24, 0x50);    /* HMBL +5 */
    hmove_and_drain(&t);
    ASSERT_EQ(t.p0_pos, 47);
    ASSERT_EQ(t.p1_pos, 52);
    ASSERT_EQ(t.m0_pos, 49);
    ASSERT_EQ(t.m1_pos, 51);
    ASSERT_EQ(t.bl_pos, 45);
    return 0;
}

/* --- HMCLR --- */

static int test_hmclr_zeroes_all_hm(void)
{
    struct tia t;
    int i;
    std_setup(&t);
    tia_write(&t, 0x20, 0x70);
    tia_write(&t, 0x21, 0x80);
    tia_write(&t, 0x22, 0x30);
    tia_write(&t, 0x23, 0xF0);
    tia_write(&t, 0x24, 0x50);
    tia_write(&t, 0x2B, 0);       /* HMCLR — 2-clk delay */
    for (i = 0; i < 2; i++) tia_tick(&t);
    ASSERT_EQ(t.hmp0, 0);
    ASSERT_EQ(t.hmp1, 0);
    ASSERT_EQ(t.hmm0, 0);
    ASSERT_EQ(t.hmm1, 0);
    ASSERT_EQ(t.hmbl, 0);
    /* HMOVE after HMCLR must do nothing. */
    t.p0_pos = 40;
    hmove_and_drain(&t);
    ASSERT_EQ(t.p0_pos, 40);
    return 0;
}

/* --- HMOVE comb: first 8 visible pixels black when strobed in HBLANK --- */

static int test_hmove_comb_in_hblank(void)
{
    struct tia t;
    int i;
    std_setup(&t);
    /* Put a sentinel sprite so we know normal rendering would differ from black. */
    t.p0_pos = 0;
    tia_write(&t, 0x1B, 0xFF);    /* GRP0 all on */
    /* Tick into HBLANK, strobe HMOVE, then finish the scanline. */
    for (i = 0; i < 10; i++) tia_tick(&t);     /* hpos = 10, still HBLANK */
    tia_write(&t, 0x2A, 0);                    /* HMOVE */
    for (; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(&t);

    /* The first 8 visible pixels should be black (HMOVE comb). */
    if (expect_range(&t, 0, 7,  BLACK(t),    "comb black")) return 1;
    /* After the comb, sprite rendering resumes. hmp0=0 so sprite didn't move;
     * GRP0=0xFF placed at p0_pos=0 means it drew at 0..7 — but those are
     * clobbered by the comb. Sprite pixel 8..? -- actually sprite is only
     * 8 pixels wide, so pixels 0..7 are the full sprite, all comb. Rest is bg. */
    if (expect_range(&t, 8, 159, BG_COLOR(t), "after comb")) return 1;
    return 0;
}

static int test_hmove_comb_mid_visible(void)
{
    /* HMOVE strobe is pipelined 6 colour clocks on real hardware, so a
     * store at visible x=50 actually starts the comb at x=56. Pixels
     * 50..55 show the background, 56..63 show the comb, then background
     * resumes. */
    struct tia t;
    int i;
    std_setup(&t);
    for (i = 0; i < TIA_HBLANK_CLOCKS + 50; i++) tia_tick(&t);  /* hpos=118, visible x=50 */
    tia_write(&t, 0x2A, 0);
    for (; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(&t);
    if (expect_range(&t, 50, 54,  BG_COLOR(t), "pre-delay bg"))    return 1;
    if (expect_range(&t, 55, 62,  BLACK(t),    "mid-visible comb")) return 1;
    if (expect_range(&t, 63, 159, BG_COLOR(t), "after mid comb"))  return 1;
    return 0;
}

/* --- Combined: HMOVE both moves and produces comb on same scanline --- */

static int test_hmove_moves_and_combs(void)
{
    struct tia t;
    int i;
    std_setup(&t);
    t.p0_pos = 100;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x20, 0x30);                /* HMP0 +3 */
    for (i = 0; i < 10; i++) tia_tick(&t);    /* hpos=10 HBLANK */
    tia_write(&t, 0x2A, 0);                    /* HMOVE: p0_pos 100 -> 97, comb */
    for (; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(&t);
    if (expect_range(&t, 0, 7, BLACK(t), "comb")) return 1;
    if (expect_range(&t, 97, 104, P0_COLOR(t), "moved sprite")) return 1;
    return 0;
}

/* --- Serialize round-trip of HMOVE state --- */

static int test_serialize_hmove(void)
{
    struct tia a, b;
    uint8_t buf[256];
    size_t sz = tia_serialize_size();
    ASSERT_TRUE(sz <= sizeof(buf));
    std_setup(&a);
    tia_write(&a, 0x20, 0x70);
    tia_write(&a, 0x24, 0x80);
    a.hmove_blank = 5;
    tia_serialize(&a, buf);
    std_setup(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.hmp0, a.hmp0);
    ASSERT_EQ(b.hmbl, a.hmbl);
    ASSERT_EQ(b.hmove_blank, a.hmove_blank);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_hmove_p0_positive_moves_left);
    RUN_TEST(test_hmove_p0_negative_moves_right);
    RUN_TEST(test_hmove_p0_minus_one);
    RUN_TEST(test_hmove_zero_no_motion);
    RUN_TEST(test_hmove_applies_to_all_objects);
    RUN_TEST(test_hmclr_zeroes_all_hm);
    RUN_TEST(test_hmove_comb_in_hblank);
    RUN_TEST(test_hmove_comb_mid_visible);
    RUN_TEST(test_hmove_moves_and_combs);
    RUN_TEST(test_serialize_hmove);
TEST_MAIN_END
