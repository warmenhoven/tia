/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doc-anchored tests for HM* and HMOVE.
 *
 * Sources:
 *   SPG §HMP0 / HMM0 / HMBL — "The HMxx registers contain a 4-bit signed
 *     value in the upper nibble. Positive moves left, negative moves
 *     right. Effect is applied by strobing HMOVE."
 *   Towers — HMOVE counter values are signed 4-bit (+7 = 0111, -8 = 1000);
 *     motion is applied in 1-clock steps during the HMOVE comb.
 *
 * The full 16-value table, per SPG:
 *   $70 = +7  (max left)
 *   $60 = +6
 *   $50 = +5
 *   $40 = +4
 *   $30 = +3
 *   $20 = +2
 *   $10 = +1
 *   $00 =  0  (no motion)
 *   $F0 = -1
 *   $E0 = -2
 *   $D0 = -3
 *   $C0 = -4
 *   $B0 = -5
 *   $A0 = -6
 *   $90 = -7
 *   $80 = -8  (max right)
 */

#include <stdio.h>
#include <string.h>
#include "tia.h"
#include "test_framework.h"

static void setup(struct tia *t)
{
    tia_init(t);
}

/* Apply HMP0 = value, strobe HMOVE, assert p0_pos moved by -signed_motion.
 * Doc: SPG §HMxx - "Positive moves left." Left = smaller x = subtract. */
static int motion_test(uint8_t hmp0, int expected_delta, const char *label)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 60;
    tia_write(&t, 0x20, hmp0);
    tia_write(&t, 0x2A, 0);        /* HMOVE */
    if (t.p0_pos != (int16_t)(60 - expected_delta)) {
        fprintf(stderr, "%s: hmp0=%02X expected p0_pos=%d got %d\n",
                label, hmp0, 60 - expected_delta, t.p0_pos);
        return 1;
    }
    return 0;
}

#define T(name, hm, delta) \
    static int name(void) { return motion_test((hm), (delta), #name); }

T(test_hmp0_plus_7,  0x70, +7)
T(test_hmp0_plus_6,  0x60, +6)
T(test_hmp0_plus_5,  0x50, +5)
T(test_hmp0_plus_4,  0x40, +4)
T(test_hmp0_plus_3,  0x30, +3)
T(test_hmp0_plus_2,  0x20, +2)
T(test_hmp0_plus_1,  0x10, +1)
T(test_hmp0_zero,    0x00,  0)
T(test_hmp0_minus_1, 0xF0, -1)
T(test_hmp0_minus_2, 0xE0, -2)
T(test_hmp0_minus_3, 0xD0, -3)
T(test_hmp0_minus_4, 0xC0, -4)
T(test_hmp0_minus_5, 0xB0, -5)
T(test_hmp0_minus_6, 0xA0, -6)
T(test_hmp0_minus_7, 0x90, -7)
T(test_hmp0_minus_8, 0x80, -8)

/* --- HM* registers: only the upper nibble is significant ---
 *
 * Documented: SPG §HMxx — "The lower four bits of HMxx are ignored."
 * Test: writing 0xFF vs 0xF0 must produce identical motion (low nibble
 * masked). */

static int test_hmp0_low_nibble_ignored(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 60;
    tia_write(&t, 0x20, 0xFF);       /* high nibble F (=-1), low nibble garbage */
    tia_write(&t, 0x2A, 0);
    ASSERT_EQ(t.p0_pos, 61);         /* -1 → +1 position delta */
    return 0;
}

/* --- HMOVE applies to ALL five HM registers simultaneously ---
 *
 * Documented: SPG §HMOVE — "A strobe to HMOVE transfers all five HM*
 * register values to their respective object position counters." */

static int test_hmove_applies_all_five(void)
{
    struct tia t;
    setup(&t);
    t.p0_pos = 60; t.p1_pos = 60; t.m0_pos = 60; t.m1_pos = 60; t.bl_pos = 60;
    tia_write(&t, 0x20, 0x10);       /* HMP0 +1 */
    tia_write(&t, 0x21, 0x20);       /* HMP1 +2 */
    tia_write(&t, 0x22, 0x30);       /* HMM0 +3 */
    tia_write(&t, 0x23, 0xF0);       /* HMM1 -1 */
    tia_write(&t, 0x24, 0xE0);       /* HMBL -2 */
    tia_write(&t, 0x2A, 0);
    ASSERT_EQ(t.p0_pos, 59);
    ASSERT_EQ(t.p1_pos, 58);
    ASSERT_EQ(t.m0_pos, 57);
    ASSERT_EQ(t.m1_pos, 61);
    ASSERT_EQ(t.bl_pos, 62);
    return 0;
}

/* --- HMCLR zeros all five HM registers ---
 *
 * Documented: SPG §HMCLR — "A strobe to HMCLR clears all five HM* registers
 * to zero. A subsequent HMOVE has no effect until HMxx are reloaded." */

static int test_hmclr_zeros_all_then_hmove_noop(void)
{
    struct tia t;
    setup(&t);
    tia_write(&t, 0x20, 0x70);
    tia_write(&t, 0x21, 0x80);
    tia_write(&t, 0x22, 0x50);
    tia_write(&t, 0x23, 0xA0);
    tia_write(&t, 0x24, 0x30);
    tia_write(&t, 0x2B, 0);          /* HMCLR */
    ASSERT_EQ(t.hmp0, 0);
    ASSERT_EQ(t.hmp1, 0);
    ASSERT_EQ(t.hmm0, 0);
    ASSERT_EQ(t.hmm1, 0);
    ASSERT_EQ(t.hmbl, 0);
    t.p0_pos = 50; t.p1_pos = 50; t.m0_pos = 50; t.m1_pos = 50; t.bl_pos = 50;
    tia_write(&t, 0x2A, 0);          /* HMOVE after HMCLR: no motion */
    ASSERT_EQ(t.p0_pos, 50);
    ASSERT_EQ(t.p1_pos, 50);
    ASSERT_EQ(t.m0_pos, 50);
    ASSERT_EQ(t.m1_pos, 50);
    ASSERT_EQ(t.bl_pos, 50);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_hmp0_plus_7);
    RUN_TEST(test_hmp0_plus_6);
    RUN_TEST(test_hmp0_plus_5);
    RUN_TEST(test_hmp0_plus_4);
    RUN_TEST(test_hmp0_plus_3);
    RUN_TEST(test_hmp0_plus_2);
    RUN_TEST(test_hmp0_plus_1);
    RUN_TEST(test_hmp0_zero);
    RUN_TEST(test_hmp0_minus_1);
    RUN_TEST(test_hmp0_minus_2);
    RUN_TEST(test_hmp0_minus_3);
    RUN_TEST(test_hmp0_minus_4);
    RUN_TEST(test_hmp0_minus_5);
    RUN_TEST(test_hmp0_minus_6);
    RUN_TEST(test_hmp0_minus_7);
    RUN_TEST(test_hmp0_minus_8);
    RUN_TEST(test_hmp0_low_nibble_ignored);
    RUN_TEST(test_hmove_applies_all_five);
    RUN_TEST(test_hmclr_zeros_all_then_hmove_noop);
TEST_MAIN_END
