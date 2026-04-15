/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored exhaustive collision tests.
 *
 * Source: SPG §CXMnP / CXPnFB / CXMnFB / CXBLPF / CXPPMM.
 *   Eight collision read-only registers. Each holds two collision flags
 *   in bits 7 and 6 (bits 5-0 are floating).
 *
 *   Reg      Addr  Bit 7        Bit 6
 *   CXM0P    0x00  M0 vs P1     M0 vs P0
 *   CXM1P    0x01  M1 vs P0     M1 vs P1
 *   CXP0FB   0x02  P0 vs PF     P0 vs BL
 *   CXP1FB   0x03  P1 vs PF     P1 vs BL
 *   CXM0FB   0x04  M0 vs PF     M0 vs BL
 *   CXM1FB   0x05  M1 vs PF     M1 vs BL
 *   CXBLPF   0x06  BL vs PF     (unused; always 0)
 *   CXPPMM   0x07  P0 vs P1     M0 vs M1
 *
 * All collision bits are LATCHED until CXCLR is strobed.
 *
 * For each register, test both bit 7 and bit 6 in isolation:
 * - Set up only the two objects that trigger that specific pair
 * - Render a scanline
 * - Assert only the expected bit is set in the register
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

static void setup(struct tia *t)
{
    tia_init(t);
    tia_write(t, 0x09, 0x40);   /* COLUBK */
    tia_write(t, 0x06, 0x80);   /* COLUP0 */
    tia_write(t, 0x07, 0x20);   /* COLUP1 */
    tia_write(t, 0x08, 0x60);   /* COLUPF */
}

/* --- CXM0P (0x00): D7 = M0∧P1, D6 = M0∧P0 --- */

static int test_cxm0p_bit7_m0_vs_p1(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x04, 0x30);           /* NUSIZ0: 8-clk missile */
    tia_write(&t, 0x1D, 0x02);           /* ENAM0 */
    tia_write(&t, 0x1C, 0xFF);           /* GRP1 all on */
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x00) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x00) & 0x40) == 0);
    return 0;
}

static int test_cxm0p_bit6_m0_vs_p0(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 40; t.p0_pos = 40;
    tia_write(&t, 0x04, 0x30);
    tia_write(&t, 0x1D, 0x02);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x00) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x00) & 0x80) == 0);
    return 0;
}

/* --- CXM1P (0x01): D7 = M1∧P0, D6 = M1∧P1 --- */

static int test_cxm1p_bit7_m1_vs_p0(void)
{
    struct tia t;
    setup(&t);
    t.m1_pos = 40; t.p0_pos = 40;
    tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x1E, 0x02);
    tia_write(&t, 0x1B, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x01) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x01) & 0x40) == 0);
    return 0;
}

static int test_cxm1p_bit6_m1_vs_p1(void)
{
    struct tia t;
    setup(&t);
    t.m1_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x1E, 0x02);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x01) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x01) & 0x80) == 0);
    return 0;
}

/* --- CXP0FB (0x02): D7 = P0∧PF, D6 = P0∧BL --- */

static int test_cxp0fb_bit7_p0_vs_pf(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 0;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0D, 0xF0);           /* PF0: PF pixels 0..3 */
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x02) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x02) & 0x40) == 0);
    return 0;
}

static int test_cxp0fb_bit6_p0_vs_bl(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 40; t.bl_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x0A, 0x30);           /* CTRLPF: ball size 8 */
    tia_write(&t, 0x1F, 0x02);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x02) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x02) & 0x80) == 0);
    return 0;
}

/* --- CXP1FB (0x03): D7 = P1∧PF, D6 = P1∧BL --- */

static int test_cxp1fb_bit7_p1_vs_pf(void)
{
    struct tia t;
    setup(&t);
    t.p1_pos = 0;
    tia_write(&t, 0x1C, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x03) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x03) & 0x40) == 0);
    return 0;
}

static int test_cxp1fb_bit6_p1_vs_bl(void)
{
    struct tia t;
    setup(&t);
    t.p1_pos = 40; t.bl_pos = 40;
    tia_write(&t, 0x1C, 0xFF);
    tia_write(&t, 0x0A, 0x30);
    tia_write(&t, 0x1F, 0x02);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x03) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x03) & 0x80) == 0);
    return 0;
}

/* --- CXM0FB (0x04): D7 = M0∧PF, D6 = M0∧BL --- */

static int test_cxm0fb_bit7_m0_vs_pf(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 0;
    tia_write(&t, 0x04, 0x30);
    tia_write(&t, 0x1D, 0x02);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x04) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x04) & 0x40) == 0);
    return 0;
}

static int test_cxm0fb_bit6_m0_vs_bl(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 40; t.bl_pos = 40;
    tia_write(&t, 0x04, 0x30);
    tia_write(&t, 0x1D, 0x02);
    tia_write(&t, 0x0A, 0x30);
    tia_write(&t, 0x1F, 0x02);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x04) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x04) & 0x80) == 0);
    return 0;
}

/* --- CXM1FB (0x05): D7 = M1∧PF, D6 = M1∧BL --- */

static int test_cxm1fb_bit7_m1_vs_pf(void)
{
    struct tia t;
    setup(&t);
    t.m1_pos = 0;
    tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x1E, 0x02);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x05) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x05) & 0x40) == 0);
    return 0;
}

static int test_cxm1fb_bit6_m1_vs_bl(void)
{
    struct tia t;
    setup(&t);
    t.m1_pos = 40; t.bl_pos = 40;
    tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x1E, 0x02);
    tia_write(&t, 0x0A, 0x30);
    tia_write(&t, 0x1F, 0x02);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x05) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x05) & 0x80) == 0);
    return 0;
}

/* --- CXBLPF (0x06): D7 = BL∧PF, D6 = unused ---
 *
 * SPG: "D6 of CXBLPF is not connected; reads as 0 (or floating bus low bits)." */

static int test_cxblpf_bit7_bl_vs_pf(void)
{
    struct tia t;
    setup(&t);
    t.bl_pos = 0;
    tia_write(&t, 0x0A, 0x30);
    tia_write(&t, 0x1F, 0x02);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x06) & 0x80) != 0);
    return 0;
}

static int test_cxblpf_bit6_never_set(void)
{
    struct tia t;
    setup(&t);
    /* No matter what we do, bit 6 of CXBLPF should never get set. */
    t.bl_pos = 40; t.p0_pos = 40; t.p1_pos = 40;
    t.m0_pos = 40; t.m1_pos = 40;
    tia_write(&t, 0x04, 0x30); tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x0A, 0x30);
    tia_write(&t, 0x1D, 0x02); tia_write(&t, 0x1E, 0x02);
    tia_write(&t, 0x1F, 0x02);
    tia_write(&t, 0x1B, 0xFF); tia_write(&t, 0x1C, 0xFF);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x06) & 0x40) == 0);
    return 0;
}

/* --- CXPPMM (0x07): D7 = P0∧P1, D6 = M0∧M1 --- */

static int test_cxppmm_bit7_p0_vs_p1(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x40) == 0);
    return 0;
}

static int test_cxppmm_bit6_m0_vs_m1(void)
{
    struct tia t;
    setup(&t);
    t.m0_pos = 40; t.m1_pos = 40;
    tia_write(&t, 0x04, 0x30); tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x1D, 0x02); tia_write(&t, 0x1E, 0x02);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x40) != 0);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) == 0);
    return 0;
}

/* --- CXCLR clears ALL collision latches ---
 *
 * Documented: SPG §CXCLR — "A strobe to CXCLR resets all collision latches
 * to zero." */

static int test_cxclr_clears_every_register(void)
{
    struct tia t;
    int i;
    setup(&t);
    t.p0_pos = 40; t.p1_pos = 40; t.m0_pos = 40; t.m1_pos = 40; t.bl_pos = 40;
    tia_write(&t, 0x04, 0x30); tia_write(&t, 0x05, 0x30);
    tia_write(&t, 0x1B, 0xFF); tia_write(&t, 0x1C, 0xFF);
    tia_write(&t, 0x1D, 0x02); tia_write(&t, 0x1E, 0x02);
    tia_write(&t, 0x1F, 0x02); tia_write(&t, 0x0A, 0x30);
    tia_write(&t, 0x0D, 0xF0);
    full_scanline(&t);
    tia_write(&t, 0x2C, 0);   /* CXCLR */
    /* All collision latches (upper 2 bits) must be 0 after CXCLR.
     * Lower 6 bits are floating bus; our impl returns (addr & 0x3F). */
    for (i = 0; i < 8; i++) {
        uint8_t v = tia_read(&t, (uint16_t)i);
        if ((v & 0xC0) != 0) {
            fprintf(stderr, "CXCLR: reg %d still has collision bits: %02X\n", i, v);
            return 1;
        }
    }
    return 0;
}

/* --- Collision bits are STICKY across scanlines until CXCLR ---
 *
 * Documented: SPG §"Collision Latches" — "Latches are set for any overlap
 * during the visible field and remain set until cleared by CXCLR." */

static int test_collisions_latched_across_scanlines(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 40; t.p1_pos = 40;
    tia_write(&t, 0x1B, 0xFF);
    tia_write(&t, 0x1C, 0xFF);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    /* Move apart, render more scanlines — bit stays latched. */
    t.p0_pos = 0; t.p1_pos = 100;
    full_scanline(&t);
    full_scanline(&t);
    ASSERT_TRUE((tia_read(&t, 0x07) & 0x80) != 0);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_cxm0p_bit7_m0_vs_p1);
    RUN_TEST(test_cxm0p_bit6_m0_vs_p0);
    RUN_TEST(test_cxm1p_bit7_m1_vs_p0);
    RUN_TEST(test_cxm1p_bit6_m1_vs_p1);
    RUN_TEST(test_cxp0fb_bit7_p0_vs_pf);
    RUN_TEST(test_cxp0fb_bit6_p0_vs_bl);
    RUN_TEST(test_cxp1fb_bit7_p1_vs_pf);
    RUN_TEST(test_cxp1fb_bit6_p1_vs_bl);
    RUN_TEST(test_cxm0fb_bit7_m0_vs_pf);
    RUN_TEST(test_cxm0fb_bit6_m0_vs_bl);
    RUN_TEST(test_cxm1fb_bit7_m1_vs_pf);
    RUN_TEST(test_cxm1fb_bit6_m1_vs_bl);
    RUN_TEST(test_cxblpf_bit7_bl_vs_pf);
    RUN_TEST(test_cxblpf_bit6_never_set);
    RUN_TEST(test_cxppmm_bit7_p0_vs_p1);
    RUN_TEST(test_cxppmm_bit6_m0_vs_m1);
    RUN_TEST(test_cxclr_clears_every_register);
    RUN_TEST(test_collisions_latched_across_scanlines);
TEST_MAIN_END
