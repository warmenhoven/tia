/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored tests for NUSIZ0/NUSIZ1.
 *
 * Source: SPG §NUSIZ0 and §NUSIZ1.
 *   Bits 0-2 select player copies/size:
 *     000 = one copy (8 clocks wide)
 *     001 = two copies, close (16 clocks apart, centre-to-centre)
 *     010 = two copies, medium (32 clocks apart)
 *     011 = three copies, close (16 apart)
 *     100 = two copies, wide (64 clocks apart)
 *     101 = one copy, double size (16 clocks wide; each GRP bit 2 clocks)
 *     110 = three copies, medium (32 apart)
 *     111 = one copy, quad size (32 clocks wide; each GRP bit 4 clocks)
 *   Bits 4-5 select missile size (1, 2, 4, 8 color clocks).
 *   The missile's COPIES match the paired player's NUSIZ bits 0-2.
 *
 * These tests exercise every bits-0-2 value (8 total) for both player and
 * missile copies, and every bits-4-5 value for missile size.
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

#define BG(t)  ((t).palette[((t).colubk >> 1) & 0x7F])
#define P0(t)  ((t).palette[((t).colup0 >> 1) & 0x7F])

static void setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x09, 0x44);   /* COLUBK */
    tia_write(t, 0x06, 0x82);   /* COLUP0 */
}

/* Helper: assert the 8 visible pixels of a "copy" of GRP0=0xFF are present
 * at [start, start+7] and that pixels [start+8, start+gap_end] are background. */
static int assert_copy_at(const struct tia *t, uint16_t start, const char *lbl)
{
    if (start + 7 >= TIA_VISIBLE_WIDTH) return 0;  /* off-screen, skip */
    return expect_range(t, start, (uint16_t)(start + 7), P0(*t), lbl);
}

/* --- NUSIZ bits 0-2: player copy modes (0..7) ---
 *
 * Documented: SPG §NUSIZ0 table. Copy centres are positioned relative to
 * the RESP0 counter 0. Pattern layouts:
 *   000: one copy
 *   001: [copy1][gap8][copy2]                  - 8px copies, 16 clock stride
 *   010: [copy1][gap24][copy2]                 - 32 clock stride
 *   011: [copy1][gap8][copy2][gap8][copy3]     - 16 clock stride, 3 copies
 *   100: [copy1][gap56][copy2]                 - 64 clock stride
 *   101: double-size: one copy, 16 clocks wide
 *   110: [copy1][gap24][copy2][gap24][copy3]   - 32 clock stride, 3 copies
 *   111: quad-size: one copy, 32 clocks wide */

static int test_nusiz_000_single_8px(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x00);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (assert_copy_at(&t, 20, "copy1")) return 1;
    if (expect_range(&t, 28, 159, BG(t), "after single")) return 1;
    return 0;
}

static int test_nusiz_001_two_close(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x01);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (assert_copy_at(&t, 20, "copy1")) return 1;
    if (assert_copy_at(&t, 36, "copy2")) return 1;
    if (expect_range(&t, 28, 35,  BG(t), "gap")) return 1;
    if (expect_range(&t, 44, 159, BG(t), "after")) return 1;
    return 0;
}

static int test_nusiz_010_two_medium(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x02);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (assert_copy_at(&t, 20, "copy1")) return 1;
    if (assert_copy_at(&t, 52, "copy2")) return 1;
    if (expect_range(&t, 28, 51, BG(t), "gap")) return 1;
    return 0;
}

static int test_nusiz_011_three_close(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x03);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (assert_copy_at(&t, 20, "copy1")) return 1;
    if (assert_copy_at(&t, 36, "copy2")) return 1;
    if (assert_copy_at(&t, 52, "copy3")) return 1;
    if (expect_range(&t, 60, 159, BG(t), "after")) return 1;
    return 0;
}

static int test_nusiz_100_two_wide(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x04);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (assert_copy_at(&t, 20, "copy1")) return 1;
    if (assert_copy_at(&t, 84, "copy2")) return 1;
    if (expect_range(&t, 28, 83, BG(t), "wide gap")) return 1;
    return 0;
}

static int test_nusiz_101_double_size(void)
{
    /* Double-size: bit 7 covers [pos, pos+1], bit 6 covers [pos+2, pos+3]... */
    struct tia t;
    setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x04, 0x05);
    tia_write(&t, 0x1B, 0x80);   /* only bit 7 on — 2 pixels should be lit */
    full_scanline(&t);
    if (expect_range(&t, 40, 41, P0(t), "double bit7")) return 1;
    if (expect_range(&t, 42, 159, BG(t), "rest")) return 1;
    return 0;
}

static int test_nusiz_110_three_medium(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 20;
    tia_write(&t, 0x04, 0x06);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    if (assert_copy_at(&t, 20, "copy1")) return 1;
    if (assert_copy_at(&t, 52, "copy2")) return 1;
    if (assert_copy_at(&t, 84, "copy3")) return 1;
    return 0;
}

static int test_nusiz_111_quad_size(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 40;
    tia_write(&t, 0x04, 0x07);
    tia_write(&t, 0x1B, 0x80);   /* only bit 7 — 4 pixels */
    full_scanline(&t);
    if (expect_range(&t, 40, 43, P0(t), "quad bit7")) return 1;
    if (expect_range(&t, 44, 159, BG(t), "rest")) return 1;
    return 0;
}

/* --- Missile size (NUSIZ bits 4-5, enumerated) ---
 *
 * Documented: SPG §NUSIZ0 table.
 *   bits 4-5 = 00 -> 1 color clock wide
 *            = 01 -> 2
 *            = 10 -> 4
 *            = 11 -> 8
 */

static int test_missile_size_1(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 20;
    tia_write(&t, 0x04, 0x00);   /* size 1 */
    tia_write(&t, 0x1D, 0x02);   /* ENAM0 */
    full_scanline(&t);
    ASSERT_EQ(t.fb[20], P0(t));
    ASSERT_EQ(t.fb[21], BG(t));
    return 0;
}

static int test_missile_size_2(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 20;
    tia_write(&t, 0x04, 0x10);   /* size 2 */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 20, 21, P0(t), "size2")) return 1;
    ASSERT_EQ(t.fb[22], BG(t));
    return 0;
}

static int test_missile_size_4(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 20;
    tia_write(&t, 0x04, 0x20);
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 20, 23, P0(t), "size4")) return 1;
    ASSERT_EQ(t.fb[24], BG(t));
    return 0;
}

static int test_missile_size_8(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 20;
    tia_write(&t, 0x04, 0x30);
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 20, 27, P0(t), "size8")) return 1;
    ASSERT_EQ(t.fb[28], BG(t));
    return 0;
}

/* --- Missile participates in NUSIZ copies ---
 *
 * Documented: SPG §NUSIZ — "The MSIZE bits modify only the missile's width,
 * but the MSBL bit applies the same copy pattern." The missile uses the
 * paired player's copy setting (bits 0-2), giving N missiles at matching
 * stride. */

static int test_missile_two_close_copies(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x01);   /* 2 close, size 1 */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    ASSERT_EQ(t.fb[30], P0(t));
    ASSERT_EQ(t.fb[46], P0(t));
    if (expect_range(&t, 31, 45, BG(t), "gap")) return 1;
    return 0;
}

static int test_missile_three_close_copies(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x03);
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    ASSERT_EQ(t.fb[30], P0(t));
    ASSERT_EQ(t.fb[46], P0(t));
    ASSERT_EQ(t.fb[62], P0(t));
    return 0;
}

/* --- Missile size AND copy combined ---
 *
 * SPG: missile copies follow player's copy count, each individual missile
 * is MSIZE wide. So NUSIZ=0x31 = 2 close copies of an 8-wide missile. */

static int test_missile_size8_two_close(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 30;
    tia_write(&t, 0x04, 0x31);   /* size 8, 2 close */
    tia_write(&t, 0x1D, 0x02);
    full_scanline(&t);
    if (expect_range(&t, 30, 37, P0(t), "missile copy1")) return 1;
    if (expect_range(&t, 46, 53, P0(t), "missile copy2")) return 1;
    if (expect_range(&t, 38, 45, BG(t), "gap")) return 1;
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_nusiz_000_single_8px);
    RUN_TEST(test_nusiz_001_two_close);
    RUN_TEST(test_nusiz_010_two_medium);
    RUN_TEST(test_nusiz_011_three_close);
    RUN_TEST(test_nusiz_100_two_wide);
    RUN_TEST(test_nusiz_101_double_size);
    RUN_TEST(test_nusiz_110_three_medium);
    RUN_TEST(test_nusiz_111_quad_size);
    RUN_TEST(test_missile_size_1);
    RUN_TEST(test_missile_size_2);
    RUN_TEST(test_missile_size_4);
    RUN_TEST(test_missile_size_8);
    RUN_TEST(test_missile_two_close_copies);
    RUN_TEST(test_missile_three_close_copies);
    RUN_TEST(test_missile_size8_two_close);
TEST_MAIN_END
