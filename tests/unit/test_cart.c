/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdlib.h>
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
    ASSERT_TRUE(!cart_load(&c, rom, 20480));   /* between 16K (F6/E7) and 32K (F4) */
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

/* Build an 8K ROM where bytes encode their bank (per-1K, per-2K, per-4K
 * depending on mapper) plus offset-within-bank, and drop a minimal reset
 * routine in bank 7 that writes to an E0 hotspot. The probe-based detector
 * runs this reset code and sees the hotspot, so the test exercises the
 * real detection path instead of planting a byte-pattern. */
static void build_8k_rom_with_e0_boot(uint8_t *rom, int hotspot_lo)
{
    int i;
    for (i = 0; i < 8192; i++) {
        int bank = i / 1024;
        rom[i] = (uint8_t)((bank << 5) | (i & 0x1F));
    }
    /* Boot at $FC00 (slot 3 start). SEI; CLD; LDX #$FF; TXS; STA $1FExx;
     * JMP to self. Keeps the CPU running but quickly hits the hotspot. */
    rom[0x1C00] = 0x78;                                 /* SEI */
    rom[0x1C01] = 0xD8;                                 /* CLD */
    rom[0x1C02] = 0xA2; rom[0x1C03] = 0xFF;             /* LDX #$FF */
    rom[0x1C04] = 0x9A;                                 /* TXS */
    rom[0x1C05] = 0x8D; rom[0x1C06] = (uint8_t)hotspot_lo; rom[0x1C07] = 0x1F;
    rom[0x1C08] = 0x4C; rom[0x1C09] = 0x08; rom[0x1C0A] = 0xFC;  /* JMP $FC08 */
    rom[0x1FFC] = 0x00; rom[0x1FFD] = 0xFC;             /* reset vec → $FC00 */
}

static int test_e0_detection_and_fixed_slot(void)
{
    struct cart c;
    uint8_t rom[8192];
    build_8k_rom_with_e0_boot(rom, 0xE0);
    ASSERT_TRUE(cart_load(&c, rom, 8192));
    ASSERT_EQ(c.mapper, CART_MAPPER_E0);
    /* Slot 3 ($FC00-$FFFF) is hardwired to bank 7. Read past the boot code
     * we planted at the start of bank 7 so we see the fill pattern: offset
     * 0x20 in bank 7 → byte (7<<5) | (0x20 & 0x1F) = 0xE0. */
    ASSERT_EQ(cart_read(&c, 0x0C20), (7 << 5));
    return 0;
}

static int test_e0_slot_switching(void)
{
    struct cart c;
    uint8_t rom[8192];
    build_8k_rom_with_e0_boot(rom, 0xE0);
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

/* ============================================================
 *   FA (CBS RAM+): 12K, 3 banks, 256 B RAM
 * ============================================================ */

static int test_fa_loads_and_starts_in_last_bank(void)
{
    struct cart c;
    uint8_t rom[12288];
    memset(rom, 0, sizeof(rom));
    rom[0 * 4096 + 0] = 0xAA;         /* byte 0 of bank 0 */
    rom[1 * 4096 + 0] = 0xBB;
    rom[2 * 4096 + 0] = 0xCC;
    ASSERT_TRUE(cart_load(&c, rom, 12288));
    ASSERT_EQ(c.mapper, CART_MAPPER_FA);
    ASSERT_EQ(c.bank, 2);
    ASSERT_EQ(cart_read(&c, 0x1000), 0xCC);   /* bank 2 at offset 0 */
    return 0;
}

static int test_fa_hotspots_switch_banks(void)
{
    struct cart c;
    uint8_t rom[12288];
    memset(rom, 0, sizeof(rom));
    rom[0 * 4096 + 0x200] = 0xA0;
    rom[1 * 4096 + 0x200] = 0xB0;
    rom[2 * 4096 + 0x200] = 0xC0;
    cart_load(&c, rom, 12288);
    cart_read(&c, 0x1FF8);  ASSERT_EQ(cart_read(&c, 0x1200), 0xA0);
    cart_read(&c, 0x1FF9);  ASSERT_EQ(cart_read(&c, 0x1200), 0xB0);
    cart_read(&c, 0x1FFA);  ASSERT_EQ(cart_read(&c, 0x1200), 0xC0);
    return 0;
}

static int test_fa_ram_roundtrip(void)
{
    /* Write via $1000-$10FF, read back via $1100-$11FF. */
    struct cart c;
    uint8_t rom[12288];
    memset(rom, 0, sizeof(rom));
    cart_load(&c, rom, 12288);
    cart_write(&c, 0x1000, 0x12);
    cart_write(&c, 0x10FF, 0x34);
    ASSERT_EQ(cart_read(&c, 0x1100), 0x12);
    ASSERT_EQ(cart_read(&c, 0x11FF), 0x34);
    return 0;
}

/* ============================================================
 *   E7 (M-Network): 16K, 8 × 2K banks, 1K + 4×256 B RAM
 * ============================================================ */

static int test_e7_detection(void)
{
    /* E7 detected when ROM has ≥3 direct-addressing instructions against
     * addresses that mirror $1FE0..$1FEB. Below that threshold it's F6
     * (single-byte matches occasionally hit from random data bytes in F6
     * graphics tables). */
    struct cart c;
    uint8_t rom[16384];

    /* Clean ROM → F6. */
    memset(rom, 0, sizeof(rom));
    ASSERT_TRUE(cart_load(&c, rom, 16384));
    ASSERT_EQ(c.mapper, CART_MAPPER_F6);

    /* 1 spurious hit stays F6 (under threshold). */
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xAD; rom[1] = 0xE0; rom[2] = 0x1F;
    ASSERT_TRUE(cart_load(&c, rom, 16384));
    ASSERT_EQ(c.mapper, CART_MAPPER_F6);

    /* 3 hits → E7. */
    rom[3] = 0xAD; rom[4] = 0xE1; rom[5] = 0x1F;
    rom[6] = 0xAD; rom[7] = 0xE2; rom[8] = 0x1F;
    ASSERT_TRUE(cart_load(&c, rom, 16384));
    ASSERT_EQ(c.mapper, CART_MAPPER_E7);

    /* Upper-mirror high byte ($FFEx) also counts — BurgerTime-style. */
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xAD; rom[1] = 0xE0; rom[2] = 0xFF;
    rom[3] = 0xAD; rom[4] = 0xE1; rom[5] = 0xFF;
    rom[6] = 0xAD; rom[7] = 0xE2; rom[8] = 0xFF;
    ASSERT_TRUE(cart_load(&c, rom, 16384));
    ASSERT_EQ(c.mapper, CART_MAPPER_E7);
    return 0;
}

static int test_e7_lower_bank_switches(void)
{
    struct cart c;
    uint8_t rom[16384];
    int b;
    memset(rom, 0, sizeof(rom));
    /* Three hotspot refs to trip the E7 detector (threshold ≥3). */
    rom[0] = 0xAD; rom[1] = 0xE0; rom[2] = 0x1F;
    rom[3] = 0xAD; rom[4] = 0xE1; rom[5] = 0x1F;
    rom[6] = 0xAD; rom[7] = 0xE2; rom[8] = 0x1F;
    for (b = 0; b < 7; b++) rom[b * 2048 + 0x100] = (uint8_t)(0xA0 + b);
    cart_load(&c, rom, 16384);
    /* Select each bank 0..6 for the lower 2K and confirm. */
    for (b = 0; b < 7; b++) {
        cart_read(&c, 0x1FE0 + b);
        if (cart_read(&c, 0x1100) != (uint8_t)(0xA0 + b)) return 1;
    }
    return 0;
}

static int test_e7_upper_2k_is_fixed_bank7(void)
{
    /* Bank 7 is 2K. $1800-$19FF is private RAM (shadows the first 512
     * bytes of bank 7), so the ROM-visible window is $1A00-$1FFF,
     * mapping to bank 7 offset 0x200..0x7FF. $1D00 → offset 0x500. */
    struct cart c;
    uint8_t rom[16384];
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xAD; rom[1] = 0xE0; rom[2] = 0x1F;
    rom[7 * 2048 + 0x500] = 0xF7;
    cart_load(&c, rom, 16384);
    ASSERT_EQ(cart_read(&c, 0x1D00), 0xF7);
    /* Switching the lower bank doesn't disturb the upper fixed slot. */
    cart_read(&c, 0x1FE3);
    ASSERT_EQ(cart_read(&c, 0x1D00), 0xF7);
    return 0;
}

static int test_e7_lower_ram_write_read(void)
{
    /* $1FE7 enables 1K RAM in the lower 2K: writes $1000-$13FF,
     * reads $1400-$17FF (same 1K). */
    struct cart c;
    uint8_t rom[16384];
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xAD; rom[1] = 0xE0; rom[2] = 0x1F;
    rom[3] = 0xAD; rom[4] = 0xE1; rom[5] = 0x1F;
    rom[6] = 0xAD; rom[7] = 0xE2; rom[8] = 0x1F;
    cart_load(&c, rom, 16384);
    cart_read(&c, 0x1FE7);                /* enable lower RAM */
    cart_write(&c, 0x1000, 0x55);
    cart_write(&c, 0x13FF, 0xAA);
    ASSERT_EQ(cart_read(&c, 0x1400), 0x55);
    ASSERT_EQ(cart_read(&c, 0x17FF), 0xAA);
    return 0;
}

static int test_e7_upper_ram_banked(void)
{
    /* $1FE8..$1FEB pick one of four 256-byte private-RAM banks for
     * the $1800-$19FF upper slot. Each bank stores independently. */
    struct cart c;
    uint8_t rom[16384];
    int b;
    memset(rom, 0, sizeof(rom));
    rom[0] = 0xAD; rom[1] = 0xE0; rom[2] = 0x1F;
    rom[3] = 0xAD; rom[4] = 0xE1; rom[5] = 0x1F;
    rom[6] = 0xAD; rom[7] = 0xE2; rom[8] = 0x1F;
    cart_load(&c, rom, 16384);
    /* Write a bank-identifying byte into each private bank. */
    for (b = 0; b < 4; b++) {
        cart_read(&c, 0x1FE8 + b);
        cart_write(&c, 0x1850, (uint8_t)(0xB0 + b));
    }
    /* Read each back through the read window ($1900-$19FF). */
    for (b = 0; b < 4; b++) {
        cart_read(&c, 0x1FE8 + b);
        if (cart_read(&c, 0x1950) != (uint8_t)(0xB0 + b)) return 1;
    }
    return 0;
}

/* ============================================================
 *   F0 / Megaboy: 64K, +1-per-$1FF0-access cycle-forward bank
 * ============================================================ */

static int test_f0_loads_and_starts_in_last_bank(void)
{
    struct cart c;
    uint8_t *rom = (uint8_t *)calloc(65536, 1);
    int i;
    for (i = 0; i < 16; i++) rom[i * 4096 + 0x500] = (uint8_t)i;
    ASSERT_TRUE(cart_load(&c, rom, 65536));
    ASSERT_EQ(c.mapper, CART_MAPPER_F0);
    ASSERT_EQ(c.bank, 15);
    ASSERT_EQ(cart_read(&c, 0x1500), 15);
    free(rom);
    return 0;
}

static int test_f0_hotspot_cycles_through_all_banks(void)
{
    struct cart c;
    uint8_t *rom = (uint8_t *)calloc(65536, 1);
    int i;
    for (i = 0; i < 16; i++) rom[i * 4096 + 0x500] = (uint8_t)i;
    cart_load(&c, rom, 65536);
    /* Start bank 15; each $1FF0 access advances by 1 (mod 16). */
    for (i = 0; i < 20; i++) {
        cart_read(&c, 0x1FF0);
        if (cart_read(&c, 0x1500) != (uint8_t)((16 + i) & 0x0F)) {
            free(rom);
            return 1;
        }
    }
    free(rom);
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
    RUN_TEST(test_fa_loads_and_starts_in_last_bank);
    RUN_TEST(test_fa_hotspots_switch_banks);
    RUN_TEST(test_fa_ram_roundtrip);
    RUN_TEST(test_e7_detection);
    RUN_TEST(test_e7_lower_bank_switches);
    RUN_TEST(test_e7_upper_2k_is_fixed_bank7);
    RUN_TEST(test_e7_lower_ram_write_read);
    RUN_TEST(test_e7_upper_ram_banked);
    RUN_TEST(test_f0_loads_and_starts_in_last_bank);
    RUN_TEST(test_f0_hotspot_cycles_through_all_banks);
TEST_MAIN_END
