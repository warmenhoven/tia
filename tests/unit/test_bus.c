/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "bus.h"
#include "test_framework.h"

static void setup(struct cpu *c, struct tia *t, struct riot *r,
                  struct cart *ct, struct bus *b)
{
    uint8_t rom[4096];
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xEA;                /* NOP just to have some nonzero byte */
    rom[0xFFC & 0xFFF] = 0x00;    /* reset vec lo */
    rom[0xFFD & 0xFFF] = 0xF0;    /* reset vec hi = $F000 */
    cart_load(ct, rom, 4096);
    cpu_init(c, (struct cpu_bus){ bus_read, bus_write, b });
    tia_init(t);
    riot_init(r);
    bus_init(b, c, t, r, ct);
}

/* --- Address decoding --- */

static int test_decode_tia_region(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    setup(&c, &t, &r, &ct, &b);
    bus_write(&b, 0x0009, 0x44);   /* COLUBK at TIA region */
    tia_tick(&t); tia_tick(&t);    /* drain COLUBK's 2-tick DAC pipeline delay */
    ASSERT_EQ(t.colubk, 0x44);
    return 0;
}

static int test_decode_riot_region(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    setup(&c, &t, &r, &ct, &b);
    bus_write(&b, 0x0080, 0x55);   /* RIOT RAM base */
    ASSERT_EQ(r.ram[0], 0x55);
    return 0;
}

static int test_decode_cart_region(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    uint8_t v;
    setup(&c, &t, &r, &ct, &b);
    ct.data[0x100] = 0xAB;
    v = bus_read(&b, 0x1100);
    ASSERT_EQ(v, 0xAB);
    return 0;
}

static int test_upper_address_bits_ignored(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    setup(&c, &t, &r, &ct, &b);
    /* $3009 has A13=1 (ignored). Low 13 bits = $1009 → A12=1 → cart.
     * Actually $3009 & 0x1FFF = $1009 → cart. So test that. */
    ct.data[0x009] = 0x77;
    {
        uint8_t v = bus_read(&b, 0x3009);
        ASSERT_EQ(v, 0x77);
    }
    /* Address $4009: $4009 & 0x1FFF = $0009 → TIA. Write should land. */
    bus_write(&b, 0x4009, 0x66);
    tia_tick(&t); tia_tick(&t);
    ASSERT_EQ(t.colubk, 0x66);
    return 0;
}

/* --- Clock propagation --- */

static int test_bus_op_advances_tia_three_clocks(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    setup(&c, &t, &r, &ct, &b);
    ASSERT_EQ(t.hpos, 0);
    bus_read(&b, 0x1000);
    ASSERT_EQ(t.hpos, 3);
    return 0;
}

static int test_bus_op_advances_riot_one_cycle(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    setup(&c, &t, &r, &ct, &b);
    /* Set up a TIM1T countdown; one bus op advances the RIOT tick once. */
    bus_write(&b, 0x0294, 10);      /* RIOT TIM1T = 10 */
    {
        uint8_t before = r.timer;
        bus_read(&b, 0x1000);        /* one bus op */
        ASSERT_EQ(r.timer, before - 1);
    }
    return 0;
}

/* --- WSYNC stall on reads --- */

static int test_wsync_stall_extends_read(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    uint16_t hpos_before, hpos_after;
    setup(&c, &t, &r, &ct, &b);
    /* Advance the beam into HBLANK at a known hpos. */
    bus_write(&b, 0x0009, 0x00);    /* 1 cycle -> hpos=3 */
    bus_write(&b, 0x0002, 0x00);    /* WSYNC strobe, 1 more cycle -> hpos=6; sets rdy */
    hpos_before = t.hpos;
    ASSERT_TRUE(t.rdy_asserted);
    /* Next read should loop until hpos wraps to 0 (new scanline). */
    bus_read(&b, 0x1000);
    hpos_after = t.hpos;
    /* After stall + 3-clock read tick, hpos should be 3 on the new scanline. */
    ASSERT_EQ(hpos_after, 3);
    ASSERT_TRUE(!t.rdy_asserted);
    ASSERT_TRUE(hpos_before != hpos_after);
    return 0;
}

/* --- Writes don't stall on RDY --- */

static int test_writes_dont_stall(void)
{
    struct cpu c; struct tia t; struct riot r; struct cart ct; struct bus b;
    setup(&c, &t, &r, &ct, &b);
    bus_write(&b, 0x0002, 0x00);    /* WSYNC */
    ASSERT_TRUE(t.rdy_asserted);
    /* A write should complete without stalling — rdy still asserted after. */
    bus_write(&b, 0x0009, 0x77);
    ASSERT_TRUE(t.rdy_asserted);
    tia_tick(&t); tia_tick(&t);     /* drain COLUBK delay */
    ASSERT_EQ(t.colubk, 0x77);
    /* hpos should only have advanced 3 clocks, not stalled to next line. */
    ASSERT_TRUE(t.hpos < 228);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_decode_tia_region);
    RUN_TEST(test_decode_riot_region);
    RUN_TEST(test_decode_cart_region);
    RUN_TEST(test_upper_address_bits_ignored);
    RUN_TEST(test_bus_op_advances_tia_three_clocks);
    RUN_TEST(test_bus_op_advances_riot_one_cycle);
    RUN_TEST(test_wsync_stall_extends_read);
    RUN_TEST(test_writes_dont_stall);
TEST_MAIN_END
