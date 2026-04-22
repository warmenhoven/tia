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

/* --- Individual collision pairs --- */

static int test_p0_p1_collision(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    /* CXPPMM bit 7 = P0-P1 */
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x40) == 0);  /* M0-M1 not set */
    return 0;
}

static int test_m0_m1_collision(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 40; t.m1_pos = 40;
    tia_write(&t, 0x04, 0x30);           /* NUSIZ0 missile 8px */
    tia_write(&t, 0x05, 0x30);           /* NUSIZ1 missile 8px */
    tia_write(&t, 0x1D, 0x02);           /* ENAM0 */
    tia_write(&t, 0x1E, 0x02);           /* ENAM1 */
    full_scanline(&t);
    /* CXPPMM bit 6 = M0-M1 */
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x40) != 0);
    return 0;
}

static int test_p0_pf_collision(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 0;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);            /* PF0 bits 4-7: PF pixels 0..3 */
    full_scanline(&t);
    /* CXP0FB bit 7 = P0-PF */
    ASSERT_TRUE((tia_read(&t, 0x02) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x02) & 0x40) == 0);  /* P0-BL not set */
    return 0;
}

static int test_bl_pf_collision(void)
{
    struct tia t;
    std_setup(&t);
    t.bl_pos = 2;
    tia_write(&t, 0x0A, 0x10);            /* CTRLPF: ball size 2px */
    tia_write(&t, 0x1F, 0x02);            /* ENABL */
    tia_write(&t, 0x0D, 0x10);            /* PF0 bit 4: PF pixel 0 (clocks 0..3) */
    full_scanline(&t);
    /* CXBLPF bit 7 */
    ASSERT_TRUE((tia_read(&t, 0x06) & 0x80) != 0);
    return 0;
}

static int test_m0_p0_collision(void)
{
    struct tia t;
    std_setup(&t);
    t.m0_pos = 40; t.p0_pos = 40;
    tia_write(&t, 0x04, 0x30);           /* 8px missile */
    tia_write(&t, 0x1D, 0x02);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    /* CXM0P bit 6 = M0-P0 */
    ASSERT_TRUE((tia_read(&t, 0x00) & 0x40) != 0);
    return 0;
}

/* --- No false positives --- */

static int test_no_collision_without_overlap(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 10; t.p1_pos = 100;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    ASSERT_EQ(tia_read(&t, 0x07) & 0x80, 0);  /* P0-P1 not set */
    return 0;
}

/* --- Collisions are sticky across scanlines --- */

static int test_collisions_persist_across_scanlines(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    /* Move sprites apart and render another scanline — bit still set. */
    t.p0_pos = 0; t.p1_pos = 100;
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    return 0;
}

/* --- CXCLR strobe --- */

static int test_cxclr_clears_all(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    tia_write(&t, 0x2C, 0);              /* CXCLR */
    {
        int i;
        /* After CXCLR, bits 7-6 (collision flags) are zero; bits 5-0 are
         * the floating bus = low 6 bits of the read address (which equals
         * `i` here since i < 8). Expect exact value (0 in bits 7-6) | i. */
        for (i = 0; i < 8; i++)
            ASSERT_EQ(tia_read(&t, (uint16_t)i), (uint16_t)i);
    }
    return 0;
}

/* --- PFP priority swap: playfield in front of players --- */

static int test_pfp_playfield_in_front(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 0;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    tia_write(&t, 0x0A, 0x04);            /* CTRLPF.PFP */
    full_scanline(&t);
    /* Under default priority, P0 would win 0..7. Under PFP, PF wins. */
    {
        int x;
        for (x = 0; x < 16; x++) {
            if (t.fb[x] != PF_COLOR(t)) {
                fprintf(stderr, "PFP fb[%d] expected PF got %08x\n", x, t.fb[x]);
                return 1;
            }
        }
    }
    return 0;
}

/* --- PFP + SCORE: PF takes priority with split color --- */

static int test_pfp_plus_score(void)
{
    struct tia t;
    std_setup(&t);
    t.p0_pos = 0;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    tia_write(&t, 0x0A, 0x06);            /* PFP + SCORE */
    full_scanline(&t);
    ASSERT_EQ(t.fb[0], P0_COLOR(t));       /* left half PF with SCORE = COLUP0 */
    return 0;
}

/* --- Serialize round-trip of collision bits --- */

static int test_serialize_collisions(void)
{
    struct tia a, b;
    uint8_t buf[256];
    size_t sz = tia_serialize_size();
    ASSERT_TRUE(sz <= sizeof(buf));
    std_setup(&a);
    a.cx[0] = 0xC0; a.cx[2] = 0x80; a.cx[7] = 0xC0;
    tia_serialize(&a, buf);
    std_setup(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.cx[0], 0xC0);
    ASSERT_EQ(b.cx[2], 0x80);
    ASSERT_EQ(b.cx[7], 0xC0);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_p0_p1_collision);
    RUN_TEST(test_m0_m1_collision);
    RUN_TEST(test_p0_pf_collision);
    RUN_TEST(test_bl_pf_collision);
    RUN_TEST(test_m0_p0_collision);
    RUN_TEST(test_no_collision_without_overlap);
    RUN_TEST(test_collisions_persist_across_scanlines);
    RUN_TEST(test_cxclr_clears_all);
    RUN_TEST(test_pfp_playfield_in_front);
    RUN_TEST(test_pfp_plus_score);
    RUN_TEST(test_serialize_collisions);
TEST_MAIN_END
