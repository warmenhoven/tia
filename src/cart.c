/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cart.h"
#include <string.h>

/* ============================================================
 *   Mapper detection for ambiguous sizes (8K could be F8/E0/3F)
 * ============================================================ */

static int find_bytes(const uint8_t *rom, size_t size,
                      const uint8_t *pat, size_t pat_len)
{
    size_t i;
    if (pat_len > size) return 0;
    for (i = 0; i <= size - pat_len; i++) {
        if (memcmp(rom + i, pat, pat_len) == 0) return 1;
    }
    return 0;
}

/* Signature-based 8K mapper detection. Combines three signals:
 *   (a) direct-addressing hotspot accesses in the code (strong positive)
 *   (b) F8 hotspot writes (strong positive for F8)
 *   (c) reset-vector location (weak positive; used only if nothing else fires)
 *
 * Games like Gyruss access E0 slots only through indirect addressing, so
 * (a) alone misses them; the reset-vector check rescues those. F8 games
 * occasionally place their entry point in $FC00-$FFFF too, so we only apply
 * the reset-vector heuristic when no explicit signature was found. */
static uint8_t detect_8k_mapper(const uint8_t *rom, size_t size)
{
    static const uint8_t e0_a[3] = { 0x8D, 0xE0, 0x1F };  /* STA $1FE0 */
    static const uint8_t e0_b[3] = { 0xAD, 0xE0, 0x1F };  /* LDA $1FE0 */
    static const uint8_t e0_c[3] = { 0x8D, 0xE8, 0x1F };  /* STA $1FE8 */
    static const uint8_t e0_d[3] = { 0xAD, 0xF0, 0x1F };  /* LDA $1FF0 */

    static const uint8_t f8_a[3] = { 0x8D, 0xF8, 0x1F };  /* STA $1FF8 */
    static const uint8_t f8_b[3] = { 0x8D, 0xF9, 0x1F };  /* STA $1FF9 */
    static const uint8_t f8_c[3] = { 0xAD, 0xF8, 0x1F };  /* LDA $1FF8 */
    static const uint8_t f8_d[3] = { 0xAD, 0xF9, 0x1F };  /* LDA $1FF9 */

    static const uint8_t f3_a[2] = { 0x85, 0x3F };        /* STA $3F */
    static const uint8_t f3_b[2] = { 0x86, 0x3F };        /* STX $3F */
    static const uint8_t f3_c[2] = { 0x84, 0x3F };        /* STY $3F */

    int have_e0 = find_bytes(rom, size, e0_a, 3)
               || find_bytes(rom, size, e0_b, 3)
               || find_bytes(rom, size, e0_c, 3)
               || find_bytes(rom, size, e0_d, 3);
    int have_f8 = find_bytes(rom, size, f8_a, 3)
               || find_bytes(rom, size, f8_b, 3)
               || find_bytes(rom, size, f8_c, 3)
               || find_bytes(rom, size, f8_d, 3);
    int have_3f = find_bytes(rom, size, f3_a, 2)
               || find_bytes(rom, size, f3_b, 2)
               || find_bytes(rom, size, f3_c, 2);

    if (have_e0 && !have_f8) return CART_MAPPER_E0;
    if (have_f8)             return CART_MAPPER_F8;
    if (have_3f)             return CART_MAPPER_3F;

    /* Fallback: reset vector at $FFFC lives at ROM offset (size-4). On E0
     * carts it must point into the fixed slot 3 ($FC00-$FFFF) because that's
     * the only region guaranteed mapped at boot. F8 carts rarely land there. */
    {
        uint16_t reset = (uint16_t)(rom[size - 4] | (rom[size - 3] << 8));
        if (reset >= 0xFC00) return CART_MAPPER_E0;
    }
    return CART_MAPPER_F8;
}

/* ============================================================
 *   Load
 * ============================================================ */

bool cart_load(struct cart *c, const void *rom, size_t size)
{
    memset(c, 0, sizeof(*c));

    switch (size) {
    case 2048:
        memcpy(c->data, rom, 2048);
        memcpy(c->data + 2048, rom, 2048);   /* mirror into 4K window */
        c->size   = 2048;
        c->mapper = CART_MAPPER_PLAIN;
        return true;

    case 4096:
        memcpy(c->data, rom, 4096);
        c->size   = 4096;
        c->mapper = CART_MAPPER_PLAIN;
        return true;

    case 8192:
        memcpy(c->data, rom, 8192);
        c->size   = 8192;
        c->mapper = detect_8k_mapper(rom, size);
        /* Reset vector sits at $FFFC in the currently-selected bank, so
         * most carts arrange for code to run from the last bank first.
         * F8: start in bank 1. E0: slots default to bank 0; slot 3 is
         * hardwired to bank 7 (last 1K), which holds the reset vectors.
         * 3F: start in bank 0; upper 2K is hardwired to last bank. */
        if (c->mapper == CART_MAPPER_F8) c->bank = 1;
        return true;

    case 16384:
        memcpy(c->data, rom, 16384);
        c->size   = 16384;
        c->mapper = CART_MAPPER_F6;
        c->bank   = 3;                       /* last bank */
        return true;

    case 32768:
        memcpy(c->data, rom, 32768);
        c->size   = 32768;
        c->mapper = CART_MAPPER_F4;
        c->bank   = 7;                       /* last bank */
        return true;

    default:
        return false;
    }
}

/* ============================================================
 *   Hotspot helpers
 * ============================================================ */

static void hotspot_f8(struct cart *c, uint16_t a)
{
    if      (a == 0xFF8) c->bank = 0;
    else if (a == 0xFF9) c->bank = 1;
}

static void hotspot_f6(struct cart *c, uint16_t a)
{
    if      (a == 0xFF6) c->bank = 0;
    else if (a == 0xFF7) c->bank = 1;
    else if (a == 0xFF8) c->bank = 2;
    else if (a == 0xFF9) c->bank = 3;
}

static void hotspot_f4(struct cart *c, uint16_t a)
{
    if (a >= 0xFF4 && a <= 0xFFB) c->bank = (uint8_t)(a - 0xFF4);
}

static void hotspot_e0(struct cart *c, uint16_t a)
{
    /* Slot 0: $1FE0..$1FE7 select bank for $F000..$F3FF */
    /* Slot 1: $1FE8..$1FEF select bank for $F400..$F7FF */
    /* Slot 2: $1FF0..$1FF7 select bank for $F800..$FBFF */
    /* Slot 3: $FC00..$FFFF is always bank 7 (not switchable) */
    if      (a >= 0xFE0 && a <= 0xFE7) c->e0_slots[0] = (uint8_t)(a & 7);
    else if (a >= 0xFE8 && a <= 0xFEF) c->e0_slots[1] = (uint8_t)(a & 7);
    else if (a >= 0xFF0 && a <= 0xFF7) c->e0_slots[2] = (uint8_t)(a & 7);
}

/* ============================================================
 *   Read
 * ============================================================ */

uint8_t cart_read(struct cart *c, uint16_t addr)
{
    uint16_t a = (uint16_t)(addr & 0x0FFF);

    switch (c->mapper) {
    case CART_MAPPER_PLAIN:
        return c->data[a];

    case CART_MAPPER_F8:
        hotspot_f8(c, a);
        if (c->sc_enabled && a >= 0x080 && a < 0x100)
            return c->sc_ram[a - 0x080];
        return c->data[c->bank * 4096u + a];

    case CART_MAPPER_F6:
        hotspot_f6(c, a);
        if (c->sc_enabled && a >= 0x080 && a < 0x100)
            return c->sc_ram[a - 0x080];
        return c->data[c->bank * 4096u + a];

    case CART_MAPPER_F4:
        hotspot_f4(c, a);
        if (c->sc_enabled && a >= 0x080 && a < 0x100)
            return c->sc_ram[a - 0x080];
        return c->data[c->bank * 4096u + a];

    case CART_MAPPER_E0: {
        uint8_t slot   = (uint8_t)(a >> 10);                  /* 0..3 */
        uint16_t off   = (uint16_t)(a & 0x03FF);
        uint8_t bank   = (slot == 3) ? 7 : c->e0_slots[slot];
        hotspot_e0(c, a);
        return c->data[bank * 1024u + off];
    }

    case CART_MAPPER_3F:
        /* Bank-switch trigger is via writes to $00-$3F, handled in
         * cart_snoop_write. Reads here just fetch from the active bank
         * (lower 2K) or the fixed last 2K (upper 2K). */
        if (a < 0x800)
            return c->data[c->bank * 2048u + a];
        return c->data[(c->size - 2048) + (a - 0x800)];

    default:
        return 0;
    }
}

/* ============================================================
 *   Write
 * ============================================================ */

void cart_write(struct cart *c, uint16_t addr, uint8_t data)
{
    uint16_t a = (uint16_t)(addr & 0x0FFF);

    switch (c->mapper) {
    case CART_MAPPER_PLAIN:
        return;                              /* plain ROM ignores writes */

    case CART_MAPPER_F8:
        if (a < 0x080) {                     /* SuperChip RAM write region */
            c->sc_ram[a] = data;
            c->sc_enabled = true;
        }
        hotspot_f8(c, a);                    /* writes trigger hotspots too */
        return;

    case CART_MAPPER_F6:
        if (a < 0x080) {
            c->sc_ram[a] = data;
            c->sc_enabled = true;
        }
        hotspot_f6(c, a);
        return;

    case CART_MAPPER_F4:
        if (a < 0x080) {
            c->sc_ram[a] = data;
            c->sc_enabled = true;
        }
        hotspot_f4(c, a);
        return;

    case CART_MAPPER_E0:
        hotspot_e0(c, a);
        return;

    case CART_MAPPER_3F:
        /* Bank switch is triggered by low-address writes (snoop_write).
         * Writes inside the cart window itself are ignored. */
        return;
    }
}

/* ============================================================
 *   Bus snoop
 * ============================================================ */

void cart_snoop_write(struct cart *c, uint16_t addr, uint8_t data)
{
    if (c->mapper == CART_MAPPER_3F && (addr & 0x1FFF) < 0x0040) {
        uint8_t num_banks = (uint8_t)(c->size / 2048);
        if (num_banks == 0) num_banks = 1;
        c->bank = (uint8_t)(data % num_banks);
    }
}
