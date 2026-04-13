/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cart.h"
#include "test_framework.h"

/* ============================================================
 *   Plain 2K / 4K
 * ============================================================ */

static int test_load_4k(void)
{
    struct cart c;
    uint8_t rom[4096];
    int i;
    for (i = 0; i < 4096; i++) rom[i] = (uint8_t)(i & 0xFF);
    ASSERT_TRUE(cart_load(&c, rom, 4096));
    ASSERT_EQ(c.size, 4096);
    ASSERT_EQ(c.mapper, CART_MAPPER_PLAIN);
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
    ASSERT_EQ(c.mapper, CART_MAPPER_PLAIN);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);
    ASSERT_EQ(cart_read(&c, 0x07FF), 0xFF);
    ASSERT_EQ(cart_read(&c, 0x0800), 0x00);    /* upper mirror */
    ASSERT_EQ(cart_read(&c, 0x0FFF), 0xFF);
    return 0;
}

static int test_load_bad_size_fails(void)
{
    struct cart c;
    uint8_t rom[1024];
    ASSERT_TRUE(!cart_load(&c, rom, 1024));
    ASSERT_TRUE(!cart_load(&c, rom, 3000));
    ASSERT_TRUE(!cart_load(&c, rom, 65536));
    return 0;
}

static int test_plain_write_is_noop(void)
{
    struct cart c;
    uint8_t rom[4096];
    memset(rom, 0xAA, sizeof(rom));
    cart_load(&c, rom, 4096);
    cart_write(&c, 0x0000, 0x55);
    ASSERT_EQ(cart_read(&c, 0x0000), 0xAA);
    return 0;
}

/* ============================================================
 *   F8 (8K, 2 banks) — hotspots $1FF8 (bank 0), $1FF9 (bank 1)
 * ============================================================ */

static void fill_banks(uint8_t *rom, int n_banks, int bank_size)
{
    int b, i;
    for (b = 0; b < n_banks; b++)
        for (i = 0; i < bank_size; i++)
            rom[b * bank_size + i] = (uint8_t)(b * 0x10 + (i & 0x0F));
}

static int test_f8_loads_and_starts_in_bank1(void)
{
    struct cart c;
    uint8_t rom[8192];
    fill_banks(rom, 2, 4096);
    ASSERT_TRUE(cart_load(&c, rom, 8192));
    ASSERT_EQ(c.size, 8192);
    ASSERT_EQ(c.mapper, CART_MAPPER_F8);
    ASSERT_EQ(c.bank, 1);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x10);    /* bank 1 byte 0 */
    return 0;
}

static int test_f8_hotspot_read_switches_bank(void)
{
    struct cart c;
    uint8_t rom[8192];
    fill_banks(rom, 2, 4096);
    cart_load(&c, rom, 8192);

    /* Hotspot read returns the byte from the NEW bank (hotspot fires first) */
    ASSERT_EQ(cart_read(&c, 0x0FF8), 0x08);    /* bank 0 byte $FF8 */
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);    /* bank 0 byte 0 */
    ASSERT_EQ(cart_read(&c, 0x0FF9), 0x19);    /* bank 1 byte $FF9 */
    ASSERT_EQ(cart_read(&c, 0x0000), 0x10);    /* bank 1 byte 0 */
    return 0;
}

static int test_f8_hotspot_write_switches_bank(void)
{
    struct cart c;
    uint8_t rom[8192];
    fill_banks(rom, 2, 4096);
    cart_load(&c, rom, 8192);

    cart_write(&c, 0x0FF8, 0);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);    /* bank 0 */
    cart_write(&c, 0x0FF9, 0);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x10);    /* bank 1 */
    return 0;
}

static int test_f8_sc_ram_roundtrip(void)
{
    struct cart c;
    uint8_t rom[8192];
    memset(rom, 0xAA, sizeof(rom));
    cart_load(&c, rom, 8192);

    /* Before any SC write, reads at $F080-$F0FF return ROM data */
    ASSERT_EQ(cart_read(&c, 0x0080), 0xAA);

    /* Write to $F000-$F07F enables SC mode; that byte lives in sc_ram */
    cart_write(&c, 0x0010, 0x5A);
    ASSERT_EQ(c.sc_enabled, 1);
    ASSERT_EQ(cart_read(&c, 0x0090), 0x5A);    /* read at $F080+0x10 */
    return 0;
}

/* ============================================================
 *   F6 (16K, 4 banks) — hotspots $1FF6..$1FF9
 * ============================================================ */

static int test_f6_loads_and_switches(void)
{
    struct cart c;
    uint8_t rom[16384];
    fill_banks(rom, 4, 4096);
    ASSERT_TRUE(cart_load(&c, rom, 16384));
    ASSERT_EQ(c.mapper, CART_MAPPER_F6);
    ASSERT_EQ(c.bank, 3);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x30);

    cart_write(&c, 0x0FF6, 0);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);    /* bank 0 */
    cart_write(&c, 0x0FF7, 0);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x10);
    cart_write(&c, 0x0FF8, 0);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x20);
    cart_write(&c, 0x0FF9, 0);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x30);
    return 0;
}

/* ============================================================
 *   F4 (32K, 8 banks) — hotspots $1FF4..$1FFB
 * ============================================================ */

static int test_f4_loads_and_switches(void)
{
    struct cart c;
    uint8_t rom[32768];
    int b;
    fill_banks(rom, 8, 4096);
    ASSERT_TRUE(cart_load(&c, rom, 32768));
    ASSERT_EQ(c.mapper, CART_MAPPER_F4);
    ASSERT_EQ(c.bank, 7);
    ASSERT_EQ(cart_read(&c, 0x0000), 0x70);

    for (b = 0; b < 8; b++) {
        cart_write(&c, (uint16_t)(0x0FF4 + b), 0);
        ASSERT_EQ(cart_read(&c, 0x0000), (uint8_t)(b * 0x10));
    }
    return 0;
}

/* ============================================================
 *   E0 (8K Parker Bros) — 3 × 1K slots + fixed 1K
 * ============================================================ */

static int test_e0_detection_and_fixed_slot(void)
{
    struct cart c;
    uint8_t rom[8192];
    int i;
    /* Fill with per-slot-recognisable bytes: bank b, byte i → b<<5 | (i&0x1F) */
    for (i = 0; i < 8192; i++) {
        int bank = i / 1024;
        rom[i] = (uint8_t)((bank << 5) | (i & 0x1F));
    }
    /* Plant an E0 hotspot STA pattern so our detector fires */
    rom[0x100] = 0x8D; rom[0x101] = 0xE0; rom[0x102] = 0x1F;

    ASSERT_TRUE(cart_load(&c, rom, 8192));
    ASSERT_EQ(c.mapper, CART_MAPPER_E0);

    /* Slot 3 ($F C00..$FFFF) is hardwired to bank 7 */
    ASSERT_EQ(cart_read(&c, 0x0C00), (7 << 5));
    return 0;
}

static int test_e0_slot_switching(void)
{
    struct cart c;
    uint8_t rom[8192];
    int i;
    for (i = 0; i < 8192; i++) {
        int bank = i / 1024;
        rom[i] = (uint8_t)((bank << 5) | (i & 0x1F));
    }
    rom[0x100] = 0x8D; rom[0x101] = 0xE0; rom[0x102] = 0x1F;
    cart_load(&c, rom, 8192);

    /* Slot 0 hotspots $1FE0..$1FE7 select bank 0..7 for $F000-$F3FF */
    cart_read(&c, 0x0FE3);                      /* bank 3 -> slot 0 */
    ASSERT_EQ(cart_read(&c, 0x0000), (3 << 5));

    /* Slot 1 hotspots $1FE8..$1FEF → $F400-$F7FF */
    cart_read(&c, 0x0FEA);                      /* bank 2 -> slot 1 */
    ASSERT_EQ(cart_read(&c, 0x0400), (2 << 5));

    /* Slot 2 hotspots $1FF0..$1FF7 → $F800-$FBFF */
    cart_read(&c, 0x0FF1);                      /* bank 1 -> slot 2 */
    ASSERT_EQ(cart_read(&c, 0x0800), (1 << 5));
    return 0;
}

/* ============================================================
 *   3F (Tigervision) — low 2K switchable via write to $00-$3F
 * ============================================================ */

static int test_3f_detection_and_fixed_upper(void)
{
    struct cart c;
    uint8_t rom[8192];
    int i;
    /* Per-2K-bank marker bytes */
    for (i = 0; i < 8192; i++) {
        int bank = i / 2048;
        rom[i] = (uint8_t)((bank << 6) | (i & 0x3F));
    }
    /* Plant a 3F signature (STA $3F) */
    rom[0x200] = 0x85; rom[0x201] = 0x3F;

    ASSERT_TRUE(cart_load(&c, rom, 8192));
    ASSERT_EQ(c.mapper, CART_MAPPER_3F);
    ASSERT_EQ(c.bank, 0);

    /* Lower 2K defaults to bank 0 */
    ASSERT_EQ(cart_read(&c, 0x0000), 0x00);
    /* Upper 2K is always the last 2K (bank 3) */
    ASSERT_EQ(cart_read(&c, 0x0800), (3 << 6));
    return 0;
}

static int test_3f_bank_switch_via_snoop(void)
{
    struct cart c;
    uint8_t rom[8192];
    int i;
    for (i = 0; i < 8192; i++) {
        int bank = i / 2048;
        rom[i] = (uint8_t)((bank << 6) | (i & 0x3F));
    }
    rom[0x200] = 0x85; rom[0x201] = 0x3F;
    cart_load(&c, rom, 8192);

    /* Writing a bank number to $00-$3F (from bus level) switches lower 2K */
    cart_snoop_write(&c, 0x003F, 2);
    ASSERT_EQ(c.bank, 2);
    ASSERT_EQ(cart_read(&c, 0x0000), (2 << 6));

    /* Upper half still fixed */
    ASSERT_EQ(cart_read(&c, 0x0800), (3 << 6));

    /* Value wraps modulo bank count */
    cart_snoop_write(&c, 0x0000, 5);             /* 5 % 4 = 1 */
    ASSERT_EQ(c.bank, 1);
    return 0;
}

TEST_MAIN_BEGIN
    RUN_TEST(test_load_4k);
    RUN_TEST(test_load_2k_mirrors);
    RUN_TEST(test_load_bad_size_fails);
    RUN_TEST(test_plain_write_is_noop);
    RUN_TEST(test_f8_loads_and_starts_in_bank1);
    RUN_TEST(test_f8_hotspot_read_switches_bank);
    RUN_TEST(test_f8_hotspot_write_switches_bank);
    RUN_TEST(test_f8_sc_ram_roundtrip);
    RUN_TEST(test_f6_loads_and_switches);
    RUN_TEST(test_f4_loads_and_switches);
    RUN_TEST(test_e0_detection_and_fixed_slot);
    RUN_TEST(test_e0_slot_switching);
    RUN_TEST(test_3f_detection_and_fixed_upper);
    RUN_TEST(test_3f_bank_switch_via_snoop);
TEST_MAIN_END
