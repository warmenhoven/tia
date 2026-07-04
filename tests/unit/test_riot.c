/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "riot.h"
#include "test_framework.h"

static void tick_n(struct riot *r, int n) { while (n-- > 0) riot_tick(r); }

/* --- RAM --- */

static int test_ram_basic(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0080, 0xAB);
    riot_write(&r, 0x00FF, 0x42);
    ASSERT_EQ(riot_read(&r, 0x0080), 0xAB);
    ASSERT_EQ(riot_read(&r, 0x00FF), 0x42);
    return 0;
}

static int test_ram_mirror_respects_a9_only(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0080, 0x11);
    /* $0180 has A8=1, A9=0 — still RAM index $00, so aliases to $0080. */
    ASSERT_EQ(riot_read(&r, 0x0180), 0x11);
    return 0;
}

/* --- Timer: each prescaler --- */

static int test_tim1t_decrements_every_cycle(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0294, 10);          /* TIM1T, timer=10 */
    ASSERT_EQ(riot_read(&r, 0x0284), 10);
    tick_n(&r, 1);  ASSERT_EQ(riot_read(&r, 0x0284), 9);
    tick_n(&r, 1);  ASSERT_EQ(riot_read(&r, 0x0284), 8);
    tick_n(&r, 8);  ASSERT_EQ(riot_read(&r, 0x0284), 0);
    return 0;
}

/* After a timer write, the prescaler is staged so the *first* tick wraps
 * and decrements the timer. Decrement events fire at cycles 1, 1+D,
 * 1+2D, ... where D is the prescaler divisor. */
static int test_tim8t_decrements_every_8_cycles(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0295, 5);           /* TIM8T, timer=5 */
    tick_n(&r, 1); ASSERT_EQ(r.timer, 4);
    tick_n(&r, 7); ASSERT_EQ(r.timer, 4);
    tick_n(&r, 1); ASSERT_EQ(r.timer, 3);
    tick_n(&r, 8); ASSERT_EQ(r.timer, 2);
    return 0;
}

static int test_tim64t_decrements_every_64(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0296, 2);
    tick_n(&r, 1);  ASSERT_EQ(r.timer, 1);
    tick_n(&r, 63); ASSERT_EQ(r.timer, 1);
    tick_n(&r, 1);  ASSERT_EQ(r.timer, 0);
    return 0;
}

static int test_t1024t_decrements_every_1024(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0297, 1);
    tick_n(&r, 1);    ASSERT_EQ(r.timer, 0);
    tick_n(&r, 1023); ASSERT_EQ(r.timer, 0);
    tick_n(&r, 1);    ASSERT_EQ(r.timer, 0xFF);  /* underflow */
    return 0;
}

/* --- Underflow transition --- */

static int test_underflow_sets_flag_and_switches_to_div1(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0295, 1);           /* TIM8T, timer=1 */
    tick_n(&r, 1);                        /* timer -> 0 */
    ASSERT_EQ(r.timer, 0);
    ASSERT_TRUE(!r.timer_underflow);
    tick_n(&r, 8);                        /* next decrement -> underflow */
    ASSERT_EQ(r.timer, 0xFF);
    ASSERT_TRUE(r.timer_underflow);
    /* Now prescaler is ÷1: one tick drops us to 0xFE. */
    tick_n(&r, 1); ASSERT_EQ(r.timer, 0xFE);
    tick_n(&r, 1); ASSERT_EQ(r.timer, 0xFD);
    return 0;
}

/* --- TIMINT clear rules --- */

static int test_intim_read_clears_underflow_flag(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0294, 0);
    tick_n(&r, 1);                        /* underflow (this is the wrap cycle) */
    ASSERT_TRUE(r.timer_underflow);
    tick_n(&r, 1);                        /* advance off the wrap cycle */
    ASSERT_TRUE(r.timer_underflow);
    /* Reading TIMINT does NOT clear bit 7. */
    (void)riot_read(&r, 0x0285);
    ASSERT_TRUE(r.timer_underflow);
    /* Reading INTIM (not on the wrap cycle) clears bit 7. */
    (void)riot_read(&r, 0x0284);
    ASSERT_TRUE(!r.timer_underflow);
    return 0;
}

static int test_timxt_write_clears_underflow_flag(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0294, 0);
    tick_n(&r, 1);
    ASSERT_TRUE(r.timer_underflow);
    riot_write(&r, 0x0295, 50);           /* any TIMxT write */
    ASSERT_TRUE(!r.timer_underflow);
    return 0;
}

/* --- Port I/O with DDR --- */

static int test_swcha_ddr_masked_read(void)
{
    struct riot r;
    riot_init(&r);
    r.pa_in = 0xF0;
    riot_write(&r, 0x0281, 0x0F);         /* low nibble = outputs */
    riot_write(&r, 0x0280, 0xAA);         /* write output latch */
    /* expect: (0xAA & 0x0F) | (0xF0 & 0xF0) = 0x0A | 0xF0 = 0xFA */
    ASSERT_EQ(riot_read(&r, 0x0280), 0xFA);
    return 0;
}

static int test_swchb_ddr_all_input(void)
{
    struct riot r;
    riot_init(&r);
    r.pb_in = 0x3F;
    riot_write(&r, 0x0282, 0xFF);         /* write ignored because DDR=0 externally */
    ASSERT_EQ(riot_read(&r, 0x0282), 0x3F);
    return 0;
}

/* --- PA7 edge detect --- */

static int test_pa7_negative_edge_sets_flag(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0284, 0);            /* A4=0, A0=0 -> negative edge */
    r.pa_in = 0x80; riot_tick(&r);        /* sync pa7_last high */
    r.pa_in = 0x00; riot_tick(&r);        /* negative edge */
    ASSERT_TRUE(r.pa7_edge_flag);
    return 0;
}

static int test_pa7_positive_edge_sets_flag(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0285, 0);            /* A4=0, A0=1 -> positive edge */
    r.pa_in = 0x00; r.pa7_last = 0x00;
    riot_tick(&r);
    r.pa_in = 0x80; riot_tick(&r);        /* positive edge */
    ASSERT_TRUE(r.pa7_edge_flag);
    return 0;
}

static int test_timint_read_clears_pa7_edge(void)
{
    struct riot r;
    uint8_t v;
    riot_init(&r);
    riot_write(&r, 0x0284, 0);
    r.pa_in = 0x80; riot_tick(&r);
    r.pa_in = 0x00; riot_tick(&r);
    ASSERT_TRUE(r.pa7_edge_flag);
    v = riot_read(&r, 0x0285);            /* TIMINT read */
    ASSERT_TRUE((v & 0x40) != 0);
    ASSERT_TRUE(!r.pa7_edge_flag);
    /* But INTIM read does NOT clear the edge flag — set it again and confirm. */
    riot_write(&r, 0x0284, 0);
    r.pa_in = 0x80; riot_tick(&r);
    r.pa_in = 0x00; riot_tick(&r);
    ASSERT_TRUE(r.pa7_edge_flag);
    (void)riot_read(&r, 0x0284);
    ASSERT_TRUE(r.pa7_edge_flag);
    return 0;
}

static int test_edge_config_a3_selects_irq_enable(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0284, 0);            /* A3=0 */
    ASSERT_TRUE(!r.pa7_irq_enable);
    riot_write(&r, 0x028C, 0);            /* A3=1 */
    ASSERT_TRUE(r.pa7_irq_enable);
    return 0;
}

/* --- Serialization --- */

static int test_serialize_roundtrip(void)
{
    struct riot a, b;
    uint8_t buf[256];
    size_t sz = riot_serialize_size();

    ASSERT_TRUE(sz <= sizeof(buf));

    riot_init(&a);
    riot_write(&a, 0x0080, 0x42);
    riot_write(&a, 0x0294, 200);         /* TIM1T */
    tick_n(&a, 137);                      /* mid-countdown */
    a.pa_in = 0x5A; a.pb_in = 0x3C;
    riot_write(&a, 0x0281, 0x0F);
    riot_write(&a, 0x0280, 0xAA);

    riot_serialize(&a, buf);
    memset(&b, 0xCD, sizeof(b));
    ASSERT_TRUE(riot_deserialize(&b, buf, sz));

    ASSERT_EQ(memcmp(a.ram, b.ram, 128), 0);
    ASSERT_EQ(b.pa_in, a.pa_in);
    ASSERT_EQ(b.pa_out, a.pa_out);
    ASSERT_EQ(b.pa_ddr, a.pa_ddr);
    ASSERT_EQ(b.pb_in, a.pb_in);
    ASSERT_EQ(b.timer, a.timer);
    ASSERT_EQ(b.prescaler_div, a.prescaler_div);
    ASSERT_EQ(b.prescaler_cnt, a.prescaler_cnt);

    /* Continue ticking both; should stay in lockstep. */
    tick_n(&a, 100);
    tick_n(&b, 100);
    ASSERT_EQ(a.timer, b.timer);
    return 0;
}

static int test_deserialize_rejects_short_buffer(void)
{
    struct riot r;
    uint8_t buf[256];
    riot_init(&r);
    ASSERT_TRUE(!riot_deserialize(&r, buf, riot_serialize_size() - 1));
    return 0;
}

/* --- Datasheet-grounded timer semantics (MOS 6532 datasheet; nocash 2k6specs) --- */

/* After underflow the timer decrements once per clock (div-1). Reading INTIM
 * (on a later cycle, not the underflow cycle itself) re-activates the
 * programmed interval: "the interval is automatically re-actived when reading
 * from the INTIM register" (nocash 2k6specs). So once INTIM has been read,
 * ticking fewer cycles than one interval must NOT decrement the timer. */
static int test_intim_read_reverts_to_programmed_divider(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0296, 3);            /* TIM64T, timer=3 */
    /* Decrements at ticks 1,65,129,193: 3->2->1->0->0xFF (underflow). */
    tick_n(&r, 193);
    ASSERT_EQ(r.timer, 0xFF);
    ASSERT_TRUE(r.timer_underflow);
    tick_n(&r, 1);                        /* off the wrap cycle: div-1 -> 0xFE */
    ASSERT_EQ(r.timer, 0xFE);
    /* Read INTIM now: clears the flag and re-activates the div-64 interval. */
    (void)riot_read(&r, 0x0284);
    ASSERT_TRUE(!r.timer_underflow);
    /* Fewer than 64 ticks must not decrement now that div-64 is restored. */
    tick_n(&r, 10);
    ASSERT_EQ(r.timer, 0xFE);
    return 0;
}

/* Wrapped-this-cycle quirk: an INTIM read landing on the exact underflow cycle
 * does NOT clear the flag, so div-1 continues (6532 datasheet; nocash
 * 2k6specs). The bus ticks the timer before the read, so this models a read
 * on the same cycle the timer wraps. */
static int test_intim_read_on_wrap_cycle_holds_flag(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0296, 1);            /* TIM64T, timer=1 */
    tick_n(&r, 65);                       /* t1: 1->0, t65: 0->0xFF underflow */
    ASSERT_EQ(r.timer, 0xFF);
    ASSERT_TRUE(r.timer_underflow);
    /* Read on the underflow cycle: flag held. */
    (void)riot_read(&r, 0x0284);
    ASSERT_TRUE(r.timer_underflow);
    /* div-1 still active: next tick decrements 0xFF->0xFE, not div-64. */
    tick_n(&r, 1);
    ASSERT_EQ(r.timer, 0xFE);
    return 0;
}

/* Companion to the above: WITHOUT an INTIM read the flag stays set, so div-1
 * continues (one decrement per clock) through subsequent cycles. */
static int test_div1_persists_until_intim_read(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0296, 3);            /* TIM64T, timer=3 */
    tick_n(&r, 193);                      /* underflow -> 0xFF, div-1 */
    ASSERT_EQ(r.timer, 0xFF);
    ASSERT_TRUE(r.timer_underflow);
    tick_n(&r, 10);                       /* no read: div-1 keeps going */
    ASSERT_EQ(r.timer, 0xF5);
    return 0;
}

/* Edge detect fires only for the configured direction; the opposite edge is a
 * no-op that must not set the flag (6532 datasheet, PA7 edge detect). */
static int test_pa7_wrong_direction_edge_is_noop(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0284, 0);            /* A4=0,A0=0 -> negative-edge cfg */
    r.pa_in = 0x00; r.pa7_last = 0x00;    /* start low, no spurious edge */
    r.pa_in = 0x80; riot_tick(&r);        /* positive edge: wrong direction */
    ASSERT_TRUE(!r.pa7_edge_flag);
    return 0;
}

/* Reading the direction registers returns the DDR latch value. */
static int test_swacnt_swbcnt_readback(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0281, 0x0F);         /* SWACNT */
    riot_write(&r, 0x0283, 0xF0);         /* SWBCNT */
    ASSERT_EQ(riot_read(&r, 0x0281), 0x0F);
    ASSERT_EQ(riot_read(&r, 0x0283), 0xF0);
    return 0;
}

/* Timer-read decode: A1 and A3 are don't-care, only A0 selects INTIM vs
 * TIMINT. $0284/$0286/$028C all read INTIM. */
static int test_timer_read_decode_mirroring(void)
{
    struct riot r;
    uint8_t a, b, c;
    riot_init(&r);
    riot_write(&r, 0x0294, 100);          /* TIM1T, timer=100 */
    tick_n(&r, 5);                        /* timer = 95 */
    a = riot_read(&r, 0x0284);
    b = riot_read(&r, 0x0286);            /* A1 set: still INTIM */
    c = riot_read(&r, 0x028C);            /* A3 set: still INTIM */
    ASSERT_EQ(a, 95);
    ASSERT_EQ(b, 95);
    ASSERT_EQ(c, 95);
    return 0;
}

/* riot_reset clears ports/DDR/timer/flags but preserves RAM (datasheet: RAM
 * is not affected by RES). */
static int test_riot_reset_preserves_ram_clears_state(void)
{
    struct riot r;
    riot_init(&r);
    riot_write(&r, 0x0080, 0x42);         /* RAM */
    riot_write(&r, 0x0296, 50);           /* arm timer */
    riot_write(&r, 0x0281, 0x0F);         /* pa_ddr */
    riot_write(&r, 0x0280, 0xAA);         /* pa_out */
    riot_reset(&r);
    ASSERT_EQ(riot_read(&r, 0x0080), 0x42);   /* RAM preserved */
    ASSERT_EQ(r.timer, 0);
    ASSERT_EQ(r.prescaler_div, 1);
    ASSERT_EQ(r.pa_ddr, 0);
    ASSERT_EQ(r.pa_out, 0);
    ASSERT_TRUE(!r.timer_underflow);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_ram_basic);
    RUN_TEST(test_ram_mirror_respects_a9_only);
    RUN_TEST(test_tim1t_decrements_every_cycle);
    RUN_TEST(test_tim8t_decrements_every_8_cycles);
    RUN_TEST(test_tim64t_decrements_every_64);
    RUN_TEST(test_t1024t_decrements_every_1024);
    RUN_TEST(test_underflow_sets_flag_and_switches_to_div1);
    RUN_TEST(test_intim_read_clears_underflow_flag);
    RUN_TEST(test_timxt_write_clears_underflow_flag);
    RUN_TEST(test_swcha_ddr_masked_read);
    RUN_TEST(test_swchb_ddr_all_input);
    RUN_TEST(test_pa7_negative_edge_sets_flag);
    RUN_TEST(test_pa7_positive_edge_sets_flag);
    RUN_TEST(test_timint_read_clears_pa7_edge);
    RUN_TEST(test_edge_config_a3_selects_irq_enable);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_rejects_short_buffer);
    RUN_TEST(test_intim_read_reverts_to_programmed_divider);
    RUN_TEST(test_intim_read_on_wrap_cycle_holds_flag);
    RUN_TEST(test_div1_persists_until_intim_read);
    RUN_TEST(test_pa7_wrong_direction_edge_is_noop);
    RUN_TEST(test_swacnt_swbcnt_readback);
    RUN_TEST(test_timer_read_decode_mirroring);
    RUN_TEST(test_riot_reset_preserves_ram_clears_state);
TEST_MAIN_END
