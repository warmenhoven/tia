/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cpu.h"
#include "test_framework.h"

static uint8_t ram[65536];

static uint8_t bus_read(void *ctx, uint16_t a)  { (void)ctx; return ram[a]; }
static void    bus_write(void *ctx, uint16_t a, uint8_t v) { (void)ctx; ram[a] = v; }

static void init(struct cpu *c)
{
    struct cpu_bus bus;
    bus.read = bus_read; bus.write = bus_write; bus.ctx = NULL;
    memset(ram, 0, sizeof(ram));
    cpu_init(c, bus);
}

/* --- reset sequence --- */

static int test_reset_takes_7_cycles(void)
{
    struct cpu c;
    init(&c);
    ram[0xFFFC] = 0x34;
    ram[0xFFFD] = 0x12;
    cpu_reset(&c);
    ASSERT_EQ(c.pc, 0x1234);
    ASSERT_EQ(c.cycles, 7);
    ASSERT_EQ(c.s, 0xFD);
    ASSERT_TRUE((c.p & CPU_FLAG_I) != 0);
    ASSERT_TRUE((c.p & CPU_FLAG_U) != 0);
    return 0;
}

/* --- JMP ($xxFF) page-boundary bug --- */

static int test_jmp_indirect_page_bug(void)
{
    struct cpu c;
    init(&c);
    ram[0x0000] = 0x6C; ram[0x0001] = 0xFF; ram[0x0002] = 0x02;  /* JMP ($02FF) */
    ram[0x02FF] = 0x78;                                          /* target lo */
    ram[0x0200] = 0x56;                                          /* target hi (bug: NOT $0300) */
    ram[0x0300] = 0xAA;                                          /* should not be read */
    c.pc = 0x0000;
    cpu_step(&c);
    ASSERT_EQ(c.pc, 0x5678);
    ASSERT_EQ(c.cycles, 5);
    return 0;
}

/* --- branch cycle counts --- */

static int test_branch_not_taken_2_cycles(void)
{
    struct cpu c;
    init(&c);
    ram[0x0000] = 0xF0; ram[0x0001] = 0x10;   /* BEQ +$10 */
    c.pc = 0x0000;
    c.p &= (uint8_t)~CPU_FLAG_Z;
    cpu_step(&c);
    ASSERT_EQ(c.pc, 0x0002);
    ASSERT_EQ(c.cycles, 2);
    return 0;
}

static int test_branch_taken_same_page_3_cycles(void)
{
    struct cpu c;
    init(&c);
    ram[0x0000] = 0xF0; ram[0x0001] = 0x10;
    c.pc = 0x0000;
    c.p |= CPU_FLAG_Z;
    cpu_step(&c);
    ASSERT_EQ(c.pc, 0x0012);
    ASSERT_EQ(c.cycles, 3);
    return 0;
}

static int test_branch_taken_page_cross_4_cycles(void)
{
    struct cpu c;
    init(&c);
    ram[0x00F0] = 0xF0; ram[0x00F1] = 0x20;   /* target $0112 — crosses page */
    c.pc = 0x00F0;
    c.p |= CPU_FLAG_Z;
    cpu_step(&c);
    ASSERT_EQ(c.pc, 0x0112);
    ASSERT_EQ(c.cycles, 4);
    return 0;
}

/* --- basic sanity: LDA imm + flags --- */

static int test_lda_imm_sets_nz(void)
{
    struct cpu c;
    init(&c);
    ram[0x0000] = 0xA9; ram[0x0001] = 0x80;   /* LDA #$80 */
    ram[0x0002] = 0xA9; ram[0x0003] = 0x00;   /* LDA #$00 */
    c.pc = 0x0000;

    cpu_step(&c);
    ASSERT_EQ(c.a, 0x80);
    ASSERT_TRUE((c.p & CPU_FLAG_N) != 0);
    ASSERT_TRUE((c.p & CPU_FLAG_Z) == 0);
    ASSERT_EQ(c.cycles, 2);

    cpu_step(&c);
    ASSERT_EQ(c.a, 0x00);
    ASSERT_TRUE((c.p & CPU_FLAG_N) == 0);
    ASSERT_TRUE((c.p & CPU_FLAG_Z) != 0);
    return 0;
}

/* --- JSR/RTS roundtrip + pushed-address semantics --- */

static int test_jsr_rts_roundtrip(void)
{
    struct cpu c;
    init(&c);
    /* $8000: JSR $9000 ; NOP ; NOP */
    ram[0x8000] = 0x20; ram[0x8001] = 0x00; ram[0x8002] = 0x90;
    ram[0x8003] = 0xEA;                                            /* NOP after return */
    /* $9000: RTS */
    ram[0x9000] = 0x60;
    c.pc = 0x8000;
    c.s  = 0xFF;

    cpu_step(&c);                       /* JSR: 6 cycles */
    ASSERT_EQ(c.pc, 0x9000);
    ASSERT_EQ(c.cycles, 6);
    /* Pushed PC should be $8002 (last operand byte of JSR) */
    ASSERT_EQ(ram[0x01FF], 0x80);       /* PCH */
    ASSERT_EQ(ram[0x01FE], 0x02);       /* PCL */
    ASSERT_EQ(c.s, 0xFD);

    cpu_step(&c);                       /* RTS: 6 cycles — returns to $8003 */
    ASSERT_EQ(c.pc, 0x8003);
    ASSERT_EQ(c.cycles, 12);
    ASSERT_EQ(c.s, 0xFF);
    return 0;
}

/* --- STA abs,X always takes dummy-read cycle even without page cross --- */

static int test_sta_absx_always_5_cycles(void)
{
    struct cpu c;
    init(&c);
    ram[0x0000] = 0x9D; ram[0x0001] = 0x00; ram[0x0002] = 0x02;   /* STA $0200,X */
    c.pc = 0x0000;
    c.x  = 0x05;
    c.a  = 0x42;
    cpu_step(&c);
    ASSERT_EQ(ram[0x0205], 0x42);
    ASSERT_EQ(c.cycles, 5);
    return 0;
}

/* --- serialize / deserialize round trip --- */

static int test_serialize_roundtrip(void)
{
    struct cpu a, b;
    uint8_t buf[64];
    size_t sz = cpu_serialize_size();

    ASSERT_TRUE(sz <= sizeof(buf));

    init(&a);
    a.a = 0x12; a.x = 0x34; a.y = 0x56; a.s = 0x78;
    a.p = (uint8_t)(CPU_FLAG_N | CPU_FLAG_V | CPU_FLAG_C | CPU_FLAG_U);
    a.pc = 0xABCD;
    a.halted = true;
    a.cycles = 0x0123456789ABCDEFULL;

    cpu_serialize(&a, buf);

    init(&b);
    ASSERT_TRUE(cpu_deserialize(&b, buf, sz));
    ASSERT_EQ(b.a, a.a);
    ASSERT_EQ(b.x, a.x);
    ASSERT_EQ(b.y, a.y);
    ASSERT_EQ(b.s, a.s);
    ASSERT_EQ(b.p, a.p);
    ASSERT_EQ(b.pc, a.pc);
    ASSERT_EQ(b.halted, a.halted);
    ASSERT_TRUE(b.cycles == a.cycles);
    return 0;
}

static int test_deserialize_rejects_short_buffer(void)
{
    struct cpu c;
    uint8_t buf[64];
    init(&c);
    ASSERT_TRUE(!cpu_deserialize(&c, buf, cpu_serialize_size() - 1));
    return 0;
}

static int test_serialize_survives_mid_execution(void)
{
    struct cpu a, b;
    uint8_t buf[64];

    init(&a);
    a.pc = 0x0200; a.s = 0xFD; a.p = (uint8_t)(CPU_FLAG_I | CPU_FLAG_U);
    /* LDA #$55 ; LDX #$AA ; STA $80 */
    ram[0x0200] = 0xA9; ram[0x0201] = 0x55;
    ram[0x0202] = 0xA2; ram[0x0203] = 0xAA;
    ram[0x0204] = 0x85; ram[0x0205] = 0x80;

    cpu_step(&a); cpu_step(&a);     /* execute LDA + LDX */
    cpu_serialize(&a, buf);

    /* Deserialize into b with a fresh (but matching) RAM state. */
    init(&b);
    ram[0x0200] = 0xA9; ram[0x0201] = 0x55;
    ram[0x0202] = 0xA2; ram[0x0203] = 0xAA;
    ram[0x0204] = 0x85; ram[0x0205] = 0x80;
    ASSERT_TRUE(cpu_deserialize(&b, buf, cpu_serialize_size()));

    cpu_step(&a);
    cpu_step(&b);
    ASSERT_EQ(ram[0x80], 0x55);
    ASSERT_EQ(a.pc, b.pc);
    ASSERT_EQ(a.a, b.a);
    ASSERT_TRUE(a.cycles == b.cycles);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_reset_takes_7_cycles);
    RUN_TEST(test_jmp_indirect_page_bug);
    RUN_TEST(test_branch_not_taken_2_cycles);
    RUN_TEST(test_branch_taken_same_page_3_cycles);
    RUN_TEST(test_branch_taken_page_cross_4_cycles);
    RUN_TEST(test_lda_imm_sets_nz);
    RUN_TEST(test_jsr_rts_roundtrip);
    RUN_TEST(test_sta_absx_always_5_cycles);
    RUN_TEST(test_serialize_roundtrip);
    RUN_TEST(test_deserialize_rejects_short_buffer);
    RUN_TEST(test_serialize_survives_mid_execution);
TEST_MAIN_END
