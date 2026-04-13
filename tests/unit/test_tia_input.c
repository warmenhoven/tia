/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "tia.h"
#include "test_framework.h"

/* --- INPT4/5 basic read --- */

static int test_inpt_default_high(void)
{
    struct tia t;
    tia_init(&t);
    ASSERT_EQ(tia_read(&t, 0x0C), 0x80);      /* INPT4 P0 fire, not pressed */
    ASSERT_EQ(tia_read(&t, 0x0D), 0x80);      /* INPT5 P1 fire, not pressed */
    return 0;
}

static int test_inpt_reflects_pin_state(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x00;                         /* P0 fire pressed */
    ASSERT_EQ(tia_read(&t, 0x0C), 0x00);
    t.inpt[5] = 0x00;
    ASSERT_EQ(tia_read(&t, 0x0D), 0x00);
    t.inpt[4] = 0x80;
    ASSERT_EQ(tia_read(&t, 0x0C), 0x80);
    return 0;
}

/* --- VBLANK bit 7 grounds INPT4/5 --- */

static int test_vblank_bit7_grounds_inpt45(void)
{
    struct tia t;
    tia_init(&t);
    t.inpt[4] = 0x80;                         /* would normally read high */
    t.inpt[5] = 0x80;
    tia_write(&t, 0x01, 0x80);                /* VBLANK bit 7 = ground */
    ASSERT_EQ(tia_read(&t, 0x0C), 0x00);
    ASSERT_EQ(tia_read(&t, 0x0D), 0x00);
    /* Paddle INPTs (0-3) are NOT grounded by this bit. */
    ASSERT_EQ(tia_read(&t, 0x08), 0x80);
    /* Clear ground; reads return to normal. */
    tia_write(&t, 0x01, 0x00);
    ASSERT_EQ(tia_read(&t, 0x0C), 0x80);
    return 0;
}

/* --- Paddle INPTs (M11 will flesh out; default stub returns 0x80) --- */

static int test_paddle_inpt_default(void)
{
    struct tia t;
    tia_init(&t);
    ASSERT_EQ(tia_read(&t, 0x08), 0x80);
    ASSERT_EQ(tia_read(&t, 0x09), 0x80);
    ASSERT_EQ(tia_read(&t, 0x0A), 0x80);
    ASSERT_EQ(tia_read(&t, 0x0B), 0x80);
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
    tia_serialize(&a, buf);
    tia_init(&b);
    ASSERT_TRUE(tia_deserialize(&b, buf, sz));
    ASSERT_EQ(b.inpt[0], 0x10);
    ASSERT_EQ(b.inpt[4], 0x00);
    ASSERT_EQ(b.inpt[5], 0x80);
    ASSERT_TRUE(b.inpt_ground);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_inpt_default_high);
    RUN_TEST(test_inpt_reflects_pin_state);
    RUN_TEST(test_vblank_bit7_grounds_inpt45);
    RUN_TEST(test_paddle_inpt_default);
    RUN_TEST(test_serialize_input);
TEST_MAIN_END
