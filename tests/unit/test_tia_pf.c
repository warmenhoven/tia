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

/* Verify that fb[x_lo..x_hi] on current scanline all equal `color`. */
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

#define PF_COLOR(t)  ((t).palette[((t).colupf >> 1) & 0x7F])
#define BG_COLOR(t)  ((t).palette[((t).colubk >> 1) & 0x7F])
#define P0_COLOR(t)  ((t).palette[((t).colup0 >> 1) & 0x7F])
#define P1_COLOR(t)  ((t).palette[((t).colup1 >> 1) & 0x7F])

/* --- Zero playfield is all background --- */

static int test_zero_playfield(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);   /* COLUPF — should not matter */
    tia_write(&t, 0x09, 0x44);   /* COLUBK */
    full_scanline(&t);
    if (expect_range(&t, 0, 159, BG_COLOR(t), "zero pf")) return 1;
    return 0;
}

/* --- PF0 bit 4 → PF pixel 0 → color clocks 0..3 --- */

static int test_pf0_bit4_is_pf_pixel_0(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0D, 0x10);   /* PF0 bit 4 */
    full_scanline(&t);
    if (expect_range(&t, 0, 3, PF_COLOR(t), "pf0 bit 4 left")) return 1;
    if (expect_range(&t, 4, 79, BG_COLOR(t), "rest of left")) return 1;
    /* Repeated right half: PF pixel 0 maps to PF pixel 20 → fb[80..83] */
    if (expect_range(&t, 80, 83, PF_COLOR(t), "pf0 bit 4 repeated")) return 1;
    if (expect_range(&t, 84, 159, BG_COLOR(t), "rest of right")) return 1;
    return 0;
}

/* --- PF0 low bits (0..3) have no effect --- */

static int test_pf0_low_bits_ignored(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0D, 0x0F);   /* PF0 bits 0..3 set, high nibble clear */
    full_scanline(&t);
    if (expect_range(&t, 0, 159, BG_COLOR(t), "pf0 low bits")) return 1;
    return 0;
}

/* --- PF1 bit 7 → PF pixel 4 → clocks 16..19 --- */

static int test_pf1_bit7_is_pf_pixel_4(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0E, 0x80);   /* PF1 bit 7 */
    full_scanline(&t);
    if (expect_range(&t, 16, 19, PF_COLOR(t), "pf1 bit 7 left")) return 1;
    if (expect_range(&t, 96, 99, PF_COLOR(t), "pf1 bit 7 right repeat")) return 1;
    return 0;
}

/* --- PF2 bit 0 → PF pixel 12 → clocks 48..51 --- */

static int test_pf2_bit0_is_pf_pixel_12(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0F, 0x01);   /* PF2 bit 0 */
    full_scanline(&t);
    if (expect_range(&t, 48, 51, PF_COLOR(t), "pf2 bit 0 left")) return 1;
    if (expect_range(&t, 128, 131, PF_COLOR(t), "pf2 bit 0 right repeat")) return 1;
    return 0;
}

/* --- PF2 bit 7 → PF pixel 19 → clocks 76..79 (rightmost PF pixel) --- */

static int test_pf2_bit7_is_pf_pixel_19(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0F, 0x80);   /* PF2 bit 7 */
    full_scanline(&t);
    if (expect_range(&t, 76, 79, PF_COLOR(t), "pf2 bit 7 left")) return 1;
    if (expect_range(&t, 156, 159, PF_COLOR(t), "pf2 bit 7 right repeat")) return 1;
    return 0;
}

/* --- Full playfield turns everything into COLUPF --- */

static int test_full_playfield(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0D, 0xF0);
    tia_write(&t, 0x0E, 0xFF);
    tia_write(&t, 0x0F, 0xFF);
    full_scanline(&t);
    if (expect_range(&t, 0, 159, PF_COLOR(t), "full pf")) return 1;
    return 0;
}

/* --- Repeat (REF=0): right half copies left, not mirrored ---
 *
 * Use an asymmetric pattern (only PF pixel 0 on) so mirror and repeat
 * produce distinguishable output in the right half. */

static int test_repeat_copies_left_to_right(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0D, 0x10);    /* only PF pixel 0 on */
    /* CTRLPF default = 0 (REF off, repeat) */
    full_scanline(&t);
    if (expect_range(&t, 0, 3, PF_COLOR(t), "repeat left")) return 1;
    if (expect_range(&t, 4, 79, BG_COLOR(t), "repeat left gap")) return 1;
    /* Repeat: same pattern at pf_pixel 20 -> clocks 80..83. */
    if (expect_range(&t, 80, 83, PF_COLOR(t), "repeat right copy")) return 1;
    if (expect_range(&t, 84, 159, BG_COLOR(t), "repeat right gap")) return 1;
    return 0;
}

/* --- Mirror (REF=1): right half reverses left ---
 *
 * Asymmetric pattern: only PF pixel 0 on. Mirror sends it to pf_pixel 39
 * (clocks 156..159), NOT pf_pixel 20. Repeat would send it to 20. */

static int test_mirror_reverses_right_half(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0D, 0x10);    /* only PF pixel 0 on */
    tia_write(&t, 0x0A, 0x01);    /* REF */
    full_scanline(&t);
    if (expect_range(&t, 0, 3, PF_COLOR(t), "mirror left")) return 1;
    if (expect_range(&t, 4, 79, BG_COLOR(t), "mirror left gap")) return 1;
    /* Mirror: pixel 0 ends up at pf_pixel 39 -> clocks 156..159. */
    if (expect_range(&t, 80, 155, BG_COLOR(t), "mirror right gap")) return 1;
    if (expect_range(&t, 156, 159, PF_COLOR(t), "mirror right end")) return 1;
    return 0;
}

/* --- Score mode: left half uses COLUP0, right half uses COLUP1 --- */

static int test_score_mode_split_color(void)
{
    struct tia t;
    tia_init(&t);
    tia_write(&t, 0x06, 0x20);    /* COLUP0 */
    tia_write(&t, 0x07, 0x40);    /* COLUP1 */
    tia_write(&t, 0x08, 0x82);    /* COLUPF — shouldn't be used */
    tia_write(&t, 0x09, 0x60);    /* COLUBK */
    tia_write(&t, 0x0D, 0x10);    /* PF0 bit 4 */
    tia_write(&t, 0x0F, 0x80);    /* PF2 bit 7 */
    tia_write(&t, 0x0A, 0x02);    /* CTRLPF.SCORE */
    full_scanline(&t);
    if (expect_range(&t, 0, 3,     P0_COLOR(t), "score left 0")) return 1;
    if (expect_range(&t, 76, 79,   P0_COLOR(t), "score left 19")) return 1;
    if (expect_range(&t, 80, 83,   P1_COLOR(t), "score right 0")) return 1;
    if (expect_range(&t, 156, 159, P1_COLOR(t), "score right 19")) return 1;
    return 0;
}

/* --- Playfield should beat background at overlapping pixels --- */

static int test_playfield_over_background(void)
{
    struct tia t;
    uint32_t bg, pf;
    tia_init(&t);
    tia_write(&t, 0x08, 0x82);
    tia_write(&t, 0x09, 0x44);
    tia_write(&t, 0x0D, 0x10);
    bg = BG_COLOR(t);
    pf = PF_COLOR(t);
    ASSERT_TRUE(bg != pf);
    full_scanline(&t);
    ASSERT_EQ(t.fb[0], pf);
    ASSERT_EQ(t.fb[4], bg);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_zero_playfield);
    RUN_TEST(test_pf0_bit4_is_pf_pixel_0);
    RUN_TEST(test_pf0_low_bits_ignored);
    RUN_TEST(test_pf1_bit7_is_pf_pixel_4);
    RUN_TEST(test_pf2_bit0_is_pf_pixel_12);
    RUN_TEST(test_pf2_bit7_is_pf_pixel_19);
    RUN_TEST(test_full_playfield);
    RUN_TEST(test_repeat_copies_left_to_right);
    RUN_TEST(test_mirror_reverses_right_half);
    RUN_TEST(test_score_mode_split_color);
    RUN_TEST(test_playfield_over_background);
TEST_MAIN_END
