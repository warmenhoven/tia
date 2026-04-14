/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tia.h"
#include "test_framework.h"

/* --- INPT4/5 basic read --- */

/* INPT register reads drive only bit 7; bits 6-0 reflect the floating bus,
 * which for zero-page reads is the low 7 bits of the address. The full
 * expected return is therefore (pin_state_bit7) | (addr & 0x7F). */

static int test_inpt_default_high(void)
{
    struct tia t;
    tia_init(&t);
    ASSERT_EQ(tia_read(&t, 0x0C), 0x80 | 0x0C);   /* INPT4 P0 fire, not pressed */
    ASSERT_EQ(tia_read(&t, 0x0D), 0x80 | 0x0D);   /* INPT5 P1 fire, not pressed */
    return 0;
}

static int test_inpt_reflects_pin_state(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x00;                              /* P0 fire pressed */
    ASSERT_EQ(tia_read(&t, 0x0C), 0x00 | 0x0C);
    t.inpt[5] = 0x00;
    ASSERT_EQ(tia_read(&t, 0x0D), 0x00 | 0x0D);
    t.inpt[4] = 0x80;
    ASSERT_EQ(tia_read(&t, 0x0C), 0x80 | 0x0C);
    return 0;
}

/* --- VBLANK bit 7 grounds paddles only, NOT INPT4/5 --- */

static int test_vblank_bit7_grounds_paddles_only(void)
{
    /* VBLANK bit 7 is the paddle DUMP line: it shorts INPT0-3 to ground
     * so paddle caps discharge. INPT4/5 (digital triggers) are NOT
     * affected — their latching is controlled by VBLANK bit 6. */
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x80;                              /* button not pressed */
    t.inpt[5] = 0x80;
    tia_write(&t, 0x01, 0x80);                     /* VBLANK bit 7 = DUMP */
    tia_tick(&t);                                  /* propagate dump to inpt[] */
    /* INPT4/5 unaffected by bit 7. */
    ASSERT_EQ(tia_read(&t, 0x0C), 0x80 | 0x0C);
    ASSERT_EQ(tia_read(&t, 0x0D), 0x80 | 0x0D);
    /* Paddle INPT0 reads 0 (capacitor grounded). */
    ASSERT_EQ(tia_read(&t, 0x08), 0x00 | 0x08);
    return 0;
}

/* --- Paddle INPTs (M11 will flesh out; default stub returns 0x80) --- */

static int test_paddle_inpt_default(void)
{
    struct tia t;
    tia_init(&t);
    ASSERT_EQ(tia_read(&t, 0x08), 0x80 | 0x08);
    ASSERT_EQ(tia_read(&t, 0x09), 0x80 | 0x09);
    ASSERT_EQ(tia_read(&t, 0x0A), 0x80 | 0x0A);
    ASSERT_EQ(tia_read(&t, 0x0B), 0x80 | 0x0B);
    return 0;
}

/* --- Paddle capacitor dynamics ---
 * INPT0-3 bit 7 starts low after the VBLANK-bit-7 ground is released, and
 * flips high once the pot's RC has charged the cap to the comparator's
 * threshold. We model charge time in TIA color clocks; while the ground
 * is held the counter stays at the target. */

static int test_paddle_cap_charges_to_high(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    t.paddle_charge_max[0] = 5;             /* 5 TIA clocks */
    t.inpt_ground = true;                   /* cap grounded */
    for (i = 0; i < 10; i++) tia_tick(&t);
    /* While grounded: bit 7 stays low, cnt stays at 5. */
    ASSERT_EQ(t.inpt[0] & 0x80, 0x00);
    ASSERT_EQ(t.paddle_charge_cnt[0], 5);

    t.inpt_ground = false;                  /* release: start charging */
    for (i = 0; i < 4; i++) tia_tick(&t);   /* 4 clocks of the 5-clock RC */
    ASSERT_EQ(t.inpt[0] & 0x80, 0x00);      /* not yet past threshold */
    ASSERT_EQ(t.paddle_charge_cnt[0], 1);
    tia_tick(&t);                            /* 5th clock: cnt→0, flip bit 7 */
    ASSERT_EQ(t.inpt[0] & 0x80, 0x80);
    ASSERT_EQ(t.paddle_charge_cnt[0], 0);
    return 0;
}

static int test_paddle_reground_resets_cap(void)
{
    struct tia t;
    int i;
    tia_init(&t);
    t.paddle_charge_max[1] = 3;
    t.inpt_ground = false;
    for (i = 0; i < 5; i++) tia_tick(&t);    /* charges past threshold */
    ASSERT_EQ(t.inpt[1] & 0x80, 0x80);

    t.inpt_ground = true;                    /* re-ground the cap */
    tia_tick(&t);
    ASSERT_EQ(t.inpt[1] & 0x80, 0x00);       /* bit 7 clears immediately */
    ASSERT_EQ(t.paddle_charge_cnt[1], 3);    /* cnt reset to target */
    return 0;
}

static int test_paddle_zero_charge_is_immediate(void)
{
    struct tia t;
    tia_init(&t);
    t.paddle_charge_max[2] = 0;              /* full-left paddle = 0 resistance */
    t.inpt_ground = false;
    /* Cap is "already charged" when max == 0. We only flip bit 7 on a
     * cnt→0 transition inside tia_tick, so the pre-existing 0x80 from
     * tia_init is what games see immediately after release. */
    ASSERT_EQ(t.inpt[2] & 0x80, 0x80);
    return 0;
}

/* --- Serialize round-trip includes input state --- */

static int test_serialize_input(void)
{
    struct tia a, b;
    uint8_t buf[128];
    size_t sz = tia_serialize_size();
    ASSERT_TRUE(sz <= sizeof(buf));
    tia_init(&a);
    a.inpt[0] = 0x10;
    a.inpt[4] = 0x00;
    a.inpt[5] = 0x80;
    a.inpt_ground = true;
    a.paddle_charge_max[0] = 1234;
    a.paddle_charge_cnt[0] = 42;
    a.paddle_charge_max[3] = 56789;
    tia_serialize(&a, buf);
    tia_init(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.inpt[0], 0x10);
    ASSERT_EQ(b.inpt[4], 0x00);
    ASSERT_EQ(b.inpt[5], 0x80);
    ASSERT_TRUE(b.inpt_ground);
    ASSERT_EQ(b.paddle_charge_max[0], 1234);
    ASSERT_EQ(b.paddle_charge_cnt[0], 42);
    ASSERT_EQ(b.paddle_charge_max[3], 56789);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_inpt_default_high);
    RUN_TEST(test_inpt_reflects_pin_state);
    RUN_TEST(test_vblank_bit7_grounds_paddles_only);
    RUN_TEST(test_paddle_inpt_default);
    RUN_TEST(test_paddle_cap_charges_to_high);
    RUN_TEST(test_paddle_reground_resets_cap);
    RUN_TEST(test_paddle_zero_charge_is_immediate);
    RUN_TEST(test_serialize_input);
TEST_MAIN_END
