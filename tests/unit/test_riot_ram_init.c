/* SPDX-License-Identifier: GPL-2.0-or-later */
/* riot_init leaves RAM zeroed (the deterministic base the harness/tests use)
 * with a fixed non-zero power-on timer; riot_randomize_ram fills RAM with a
 * seed-deterministic pseudo-random pattern (the hardware-like cold-boot mode). */
#include "riot.h"
#include "test_framework.h"

static int test_init_zeroes_ram_fixed_timer(void)
{
    struct riot r;
    int i, all_zero = 1;
    riot_init(&r);
    for (i = 0; i < 128; i++) if (r.ram[i] != 0) all_zero = 0;
    ASSERT_EQ(all_zero, 1);                 /* deterministic base = zeroed */
    ASSERT_EQ((int)r.prescaler_div, 1024);  /* fixed power-on prescaler */
    ASSERT_TRUE(r.timer != 0);              /* fixed non-zero power-on timer */
    return 0;
}

static int test_randomize_nonzero_and_seed_deterministic(void)
{
    struct riot a, b, c;
    int i, all_zero = 1, ab_same = 1, ac_diff = 0;

    riot_init(&a);
    riot_randomize_ram(&a, 0x12345678u);
    for (i = 0; i < 128; i++) if (a.ram[i] != 0) all_zero = 0;
    ASSERT_EQ(all_zero, 0);                 /* hardware fill is not all zero */

    riot_init(&b);
    riot_randomize_ram(&b, 0x12345678u);
    for (i = 0; i < 128; i++) if (a.ram[i] != b.ram[i]) ab_same = 0;
    ASSERT_EQ(ab_same, 1);                  /* same seed -> identical fill */

    riot_init(&c);
    riot_randomize_ram(&c, 0x9ABCDEF0u);
    for (i = 0; i < 128; i++) if (a.ram[i] != c.ram[i]) ac_diff = 1;
    ASSERT_EQ(ac_diff, 1);                  /* different seed -> different fill */
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_init_zeroes_ram_fixed_timer);
    RUN_TEST(test_randomize_nonzero_and_seed_deterministic);
TEST_MAIN_END
