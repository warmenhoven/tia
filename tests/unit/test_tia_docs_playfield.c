/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored playfield bit-mapping tests.
 *
 * Source: SPG §PF0 / PF1 / PF2 — "The playfield is 20 bits wide, drawn in
 * 4-color-clock blocks across the 160-clock visible scanline. Bits map:
 *
 *   PF0 bits 4-7 → PF pixels 0-3    (bit 4 first in scan order)
 *   PF1 bits 7-0 → PF pixels 4-11   (bit 7 first — note reversed order)
 *   PF2 bits 0-7 → PF pixels 12-19  (bit 0 first)
 *
 * Each PF pixel covers 4 visible color clocks, so PF pixel N maps to visible
 * x = [4N, 4N+3].
 *
 * The right half of the scanline (pixels 20-39, visible x=80-159) is either
 * a *repeat* of the left (CTRLPF.REF=0) or a *mirror* (REF=1).
 *
 * This file tests each of the 20 PF bits individually to pin down the
 * bit-to-pixel mapping exactly. */

#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void full_scanline(struct tia *t)
{
    int i;
    for (i = 0; i < TIA_SCANLINE_CLOCKS; i++) tia_tick(t);
}

#define PF(t) ((t).palette[((t).colupf >> 1) & 0x7F])
#define BG(t) ((t).palette[((t).colubk >> 1) & 0x7F])

static int expect_range(const struct tia *t, uint16_t lo, uint16_t hi,
                        uint32_t color, const char *label)
{
    uint16_t x;
    for (x = lo; x <= hi; x++) {
        if (t->fb[x] != color) {
            fprintf(stderr, "%s: fb[%u] expected %08x got %08x\n",
                    label, x, color, t->fb[x]);
            return 1;
        }
    }
    return 0;
}

static void setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x08, 0x82);   /* COLUPF */
    tia_write(t, 0x09, 0x44);   /* COLUBK */
}

/* Parameterised: set the specified PF register/bit pair, render, and assert
 * the expected PF pixel lights up (at both the left position and its right
 * repeat, since default CTRLPF has REF=0). */
static int single_bit_test(uint8_t reg_addr, uint8_t bit_mask,
                           int expected_pf_pixel, const char *label)
{
    struct tia t;
    uint16_t left_lo, left_hi, right_lo, right_hi;
    setup(&t);
    tia_write(&t, reg_addr, bit_mask);
    full_scanline(&t);

    left_lo  = (uint16_t)(expected_pf_pixel * 4);
    left_hi  = (uint16_t)(left_lo + 3);
    right_lo = (uint16_t)(left_lo + 80);
    right_hi = (uint16_t)(left_hi + 80);

    if (expect_range(&t, left_lo,  left_hi,  PF(t), label)) return 1;
    if (expect_range(&t, right_lo, right_hi, PF(t), label)) return 1;
    /* The rest of the scanline should be BG. */
    if (left_lo > 0)
        if (expect_range(&t, 0, (uint16_t)(left_lo - 1), BG(t), "before")) return 1;
    if (left_hi < 79)
        if (expect_range(&t, (uint16_t)(left_hi + 1), 79, BG(t), "after_left")) return 1;
    if (right_lo > 80)
        if (expect_range(&t, 80, (uint16_t)(right_lo - 1), BG(t), "before_right")) return 1;
    if (right_hi < 159)
        if (expect_range(&t, (uint16_t)(right_hi + 1), 159, BG(t), "after_right")) return 1;
    return 0;
}

/* PF0 bit 4-7: PF pixels 0-3 */
static int test_pf0_bit4_maps_to_pf_pixel_0(void) { return single_bit_test(0x0D, 0x10, 0, "PF0.4"); }
static int test_pf0_bit5_maps_to_pf_pixel_1(void) { return single_bit_test(0x0D, 0x20, 1, "PF0.5"); }
static int test_pf0_bit6_maps_to_pf_pixel_2(void) { return single_bit_test(0x0D, 0x40, 2, "PF0.6"); }
static int test_pf0_bit7_maps_to_pf_pixel_3(void) { return single_bit_test(0x0D, 0x80, 3, "PF0.7"); }

/* PF1 bit 7-0: PF pixels 4-11 */
static int test_pf1_bit7_maps_to_pf_pixel_4(void)  { return single_bit_test(0x0E, 0x80, 4,  "PF1.7"); }
static int test_pf1_bit6_maps_to_pf_pixel_5(void)  { return single_bit_test(0x0E, 0x40, 5,  "PF1.6"); }
static int test_pf1_bit5_maps_to_pf_pixel_6(void)  { return single_bit_test(0x0E, 0x20, 6,  "PF1.5"); }
static int test_pf1_bit4_maps_to_pf_pixel_7(void)  { return single_bit_test(0x0E, 0x10, 7,  "PF1.4"); }
static int test_pf1_bit3_maps_to_pf_pixel_8(void)  { return single_bit_test(0x0E, 0x08, 8,  "PF1.3"); }
static int test_pf1_bit2_maps_to_pf_pixel_9(void)  { return single_bit_test(0x0E, 0x04, 9,  "PF1.2"); }
static int test_pf1_bit1_maps_to_pf_pixel_10(void) { return single_bit_test(0x0E, 0x02, 10, "PF1.1"); }
static int test_pf1_bit0_maps_to_pf_pixel_11(void) { return single_bit_test(0x0E, 0x01, 11, "PF1.0"); }

/* PF2 bit 0-7: PF pixels 12-19 */
static int test_pf2_bit0_maps_to_pf_pixel_12(void) { return single_bit_test(0x0F, 0x01, 12, "PF2.0"); }
static int test_pf2_bit1_maps_to_pf_pixel_13(void) { return single_bit_test(0x0F, 0x02, 13, "PF2.1"); }
static int test_pf2_bit2_maps_to_pf_pixel_14(void) { return single_bit_test(0x0F, 0x04, 14, "PF2.2"); }
static int test_pf2_bit3_maps_to_pf_pixel_15(void) { return single_bit_test(0x0F, 0x08, 15, "PF2.3"); }
static int test_pf2_bit4_maps_to_pf_pixel_16(void) { return single_bit_test(0x0F, 0x10, 16, "PF2.4"); }
static int test_pf2_bit5_maps_to_pf_pixel_17(void) { return single_bit_test(0x0F, 0x20, 17, "PF2.5"); }
static int test_pf2_bit6_maps_to_pf_pixel_18(void) { return single_bit_test(0x0F, 0x40, 18, "PF2.6"); }
static int test_pf2_bit7_maps_to_pf_pixel_19(void) { return single_bit_test(0x0F, 0x80, 19, "PF2.7"); }

/* PF0 low nibble (bits 0-3) is ignored entirely.
 * SPG: "Only bits 4-7 of PF0 are used." */
static int test_pf0_low_nibble_has_no_effect(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x0D, 0x0F);   /* bits 0-3 only */
    full_scanline(&t);
    if (expect_range(&t, 0, 159, BG(t), "pf0 low nibble inert")) return 1;
    return 0;
}

/* --- Mirror vs Repeat ---
 *
 * SPG §CTRLPF bit 0 "REF" — when set, right half renders PF in reverse
 * order (pixel 19 first, pixel 0 last). When clear, right half repeats
 * the left verbatim. Test with an asymmetric single-bit pattern. */

static int test_repeat_right_copies_left(void)
{
    /* REF=0 (default). Set PF2 bit 7 (PF pixel 19). Left copy at clocks
     * 76..79. Repeat: same pattern in right half, so PF pixel 19 appears
     * again at its position within the right half's copy of the left
     * scanning, i.e. at visible x = 156..159. */
    struct tia t;
    setup(&t);
    tia_write(&t, 0x0F, 0x80);
    tia_write(&t, 0x0A, 0x00);   /* CTRLPF: REF=0 */
    full_scanline(&t);
    if (expect_range(&t, 76, 79, PF(t), "left pf19")) return 1;
    if (expect_range(&t, 156, 159, PF(t), "repeat pf19")) return 1;
    return 0;
}

static int test_mirror_right_reverses_left(void)
{
    /* REF=1. Set PF0 bit 4 (PF pixel 0). Left copy at clocks 0..3. Mirror
     * sends it to PF pixel 39 → visible x = 156..159. */
    struct tia t;
    setup(&t);
    tia_write(&t, 0x0D, 0x10);
    tia_write(&t, 0x0A, 0x01);   /* REF=1 */
    full_scanline(&t);
    if (expect_range(&t, 0, 3, PF(t), "left pf0")) return 1;
    if (expect_range(&t, 156, 159, PF(t), "mirror pf0->pf39")) return 1;
    if (expect_range(&t, 80, 155, BG(t), "mirror gap")) return 1;
    return 0;
}

/* --- SCORE mode ---
 *
 * SPG §CTRLPF bit 1 "SCORE" — when set, left-half PF is drawn in COLUP0,
 * right-half PF in COLUP1. The name is historical: used for 2-digit
 * scores in split colours. */

static int test_score_mode_splits_pf_colors(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x06, 0x20);   /* COLUP0 */
    tia_write(&t, 0x07, 0x40);   /* COLUP1 */
    tia_write(&t, 0x08, 0x82);   /* COLUPF — not used in score */
    tia_write(&t, 0x0D, 0x10);   /* PF0.4 = PF pixel 0 */
    tia_write(&t, 0x0F, 0x80);   /* PF2.7 = PF pixel 19 */
    tia_write(&t, 0x0A, 0x02);   /* SCORE */
    full_scanline(&t);
    {
        uint32_t p0 = t.palette[(0x20 >> 1) & 0x7F];
        uint32_t p1 = t.palette[(0x40 >> 1) & 0x7F];
        if (expect_range(&t,   0,   3, p0, "left pf0 uses COLUP0"))  return 1;
        if (expect_range(&t,  76,  79, p0, "left pf19 uses COLUP0")) return 1;
        if (expect_range(&t,  80,  83, p1, "right pf0 uses COLUP1")) return 1;
        if (expect_range(&t, 156, 159, p1, "right pf19 uses COLUP1")) return 1;
    }
    return 0;
}

/* --- PFP (PF priority) ---
 *
 * SPG §CTRLPF bit 2 "PFP" — when set, the playfield (and ball) take
 * priority over players and missiles. Without PFP, players/missiles win. */

static int test_pfp_pf_beats_player(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 0;
    tia_write(&t, 0x06, 0x20);   /* COLUP0 */
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    tia_write(&t, 0x0A, 0x04);   /* PFP=1 */
    full_scanline(&t);
    /* P0 at x=0..7 overlaps PF at x=0..15. With PFP, PF wins. */
    if (expect_range(&t, 0, 15, PF(t), "pfp pf over p0")) return 1;
    return 0;
}

static int test_no_pfp_player_beats_pf(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 0;
    tia_write(&t, 0x06, 0x20);
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    tia_write(&t, 0x0A, 0x00);   /* PFP=0 */
    full_scanline(&t);
    {
        uint32_t p0c = t.palette[(0x20 >> 1) & 0x7F];
        if (expect_range(&t, 0, 7, p0c, "p0 over pf")) return 1;
        if (expect_range(&t, 8, 15, PF(t), "pf alone")) return 1;
    }
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_pf0_bit4_maps_to_pf_pixel_0);
    RUN_TEST(test_pf0_bit5_maps_to_pf_pixel_1);
    RUN_TEST(test_pf0_bit6_maps_to_pf_pixel_2);
    RUN_TEST(test_pf0_bit7_maps_to_pf_pixel_3);
    RUN_TEST(test_pf1_bit7_maps_to_pf_pixel_4);
    RUN_TEST(test_pf1_bit6_maps_to_pf_pixel_5);
    RUN_TEST(test_pf1_bit5_maps_to_pf_pixel_6);
    RUN_TEST(test_pf1_bit4_maps_to_pf_pixel_7);
    RUN_TEST(test_pf1_bit3_maps_to_pf_pixel_8);
    RUN_TEST(test_pf1_bit2_maps_to_pf_pixel_9);
    RUN_TEST(test_pf1_bit1_maps_to_pf_pixel_10);
    RUN_TEST(test_pf1_bit0_maps_to_pf_pixel_11);
    RUN_TEST(test_pf2_bit0_maps_to_pf_pixel_12);
    RUN_TEST(test_pf2_bit1_maps_to_pf_pixel_13);
    RUN_TEST(test_pf2_bit2_maps_to_pf_pixel_14);
    RUN_TEST(test_pf2_bit3_maps_to_pf_pixel_15);
    RUN_TEST(test_pf2_bit4_maps_to_pf_pixel_16);
    RUN_TEST(test_pf2_bit5_maps_to_pf_pixel_17);
    RUN_TEST(test_pf2_bit6_maps_to_pf_pixel_18);
    RUN_TEST(test_pf2_bit7_maps_to_pf_pixel_19);
    RUN_TEST(test_pf0_low_nibble_has_no_effect);
    RUN_TEST(test_repeat_right_copies_left);
    RUN_TEST(test_mirror_right_reverses_left);
    RUN_TEST(test_score_mode_splits_pf_colors);
    RUN_TEST(test_pfp_pf_beats_player);
    RUN_TEST(test_no_pfp_player_beats_pf);
TEST_MAIN_END
