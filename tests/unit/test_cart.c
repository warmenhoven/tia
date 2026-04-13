/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cart.h"
#include "test_framework.h"

static int test_load_4k(void)
{
    struct cart c;
    uint8_t rom[4096];
    int i;
    for (i = 0; i < 4096; i++) rom[i] = (uint8_t)(i & 0xFF);
    ASSERT_TRUE(cart_load(&c, rom, 4096));
    ASSERT_EQ(c.size, 4096);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);
    ASSERT_EQ(cart_read(&c, 0x0FFF), 0xFF);
    /* Address bits above bit 11 are ignored (handled by bus). */
    ASSERT_EQ(cart_read(&c, 0x1234), 0x34);
    return 0;
}

static int test_load_2k_mirrors(void)
{
    struct cart c;
    uint8_t rom[2048];
    int i;
    for (i = 0; i < 2048; i++) rom[i] = (uint8_t)(i & 0xFF);
    ASSERT_TRUE(cart_load(&c, rom, 2048));
    ASSERT_EQ(c.size, 2048);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);
    ASSERT_EQ(cart_read(&c, 0x07FF), 0xFF);
    /* Upper half mirrors lower half. */
    ASSERT_EQ(cart_read(&c, 0x0800), 0x00);
    ASSERT_EQ(cart_read(&c, 0x0FFF), 0xFF);
    return 0;
}

static int test_load_bad_size_fails(void)
{
    struct cart c;
    uint8_t rom[1024];
    ASSERT_TRUE(!cart_load(&c, rom, 1024));
    ASSERT_TRUE(!cart_load(&c, rom, 3000));
    ASSERT_TRUE(!cart_load(&c, rom, 8192));
    return 0;
}

static int test_write_is_noop(void)
{
    struct cart c;
    uint8_t rom[4096];
    memset(rom, 0xAA, sizeof(rom));
    cart_load(&c, rom, 4096);
    cart_write(&c, 0x0000, 0x55);
    ASSERT_EQ(cart_read(&c, 0x0000), 0xAA);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_load_4k);
    RUN_TEST(test_load_2k_mirrors);
    RUN_TEST(test_load_bad_size_fails);
    RUN_TEST(test_write_is_noop);
TEST_MAIN_END
