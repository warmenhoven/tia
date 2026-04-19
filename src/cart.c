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

    /* FE (Activision): detected by specific 5-byte signatures. These
     * patterns are JSR sequences that trigger the $01FE hotspot during
     * stack operations. Attributed to the MESS project. */
    {
        static const uint8_t fe_a[5] = { 0x20, 0x00, 0xD0, 0xC6, 0xC5 }; /* Decathlon */
        static const uint8_t fe_b[5] = { 0x20, 0xC3, 0xF8, 0xA5, 0x82 }; /* Robot Tank */
        static const uint8_t fe_c[5] = { 0xD0, 0xFB, 0x20, 0x73, 0xFE }; /* Space Shuttle */
        static const uint8_t fe_d[5] = { 0xD0, 0xFB, 0x20, 0x68, 0xFE }; /* Space Shuttle SECAM */
        static const uint8_t fe_e[5] = { 0x20, 0x00, 0xF0, 0x84, 0xD6 }; /* Thwocker */
        int have_fe = find_bytes(rom, size, fe_a, 5)
                   || find_bytes(rom, size, fe_b, 5)
                   || find_bytes(rom, size, fe_c, 5)
                   || find_bytes(rom, size, fe_d, 5)
                   || find_bytes(rom, size, fe_e, 5);
        if (have_fe && !have_f8) return CART_MAPPER_FE;
    }

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
        /* DPC (Pitfall II): 8K program + 2K display = 10K, or 10K + 255
         * bytes of frequency table = 10495. Accept either. */
        if (size == 10240 || size == 10495) {
            memcpy(c->data, rom, size);
            c->size   = (uint32_t)size;
            c->mapper = CART_MAPPER_DPC;
            c->bank   = 1;
            c->dpc_random = 1;
            return true;
        }
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
 *   DPC co-processor helpers
 * ============================================================ */

static void dpc_clock_rng(struct cart *c)
{
    /* 8-bit LFSR: feedback = NOT(XOR of bits 7, 5, 4, 3). */
    static const uint8_t f[16] = {
        1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
    };
    uint8_t bit = f[((c->dpc_random >> 3) & 0x07)
                   | ((c->dpc_random & 0x80) ? 0x08 : 0x00)];
    c->dpc_random = (uint8_t)((c->dpc_random << 1) | bit);
}

static void dpc_update_music_fetchers(struct cart *c)
{
    /* NTSC 6507 clock rate. For PAL/SECAM this would differ slightly. */
    static const double CLOCK_RATE = 1193191.66666667;
    static const double DPC_PITCH  = 20000.0;
    uint32_t elapsed, whole;
    double clocks;
    int x;

    if (!c->cpu_cycles) return;
    elapsed = (uint32_t)(*c->cpu_cycles - c->dpc_audio_cycles);
    c->dpc_audio_cycles = *c->cpu_cycles;

    clocks = ((DPC_PITCH * elapsed) / CLOCK_RATE) + c->dpc_frac_clocks;
    whole  = (uint32_t)clocks;
    c->dpc_frac_clocks = clocks - (double)whole;
    if (whole == 0) return;

    for (x = 5; x <= 7; x++) {
        int32_t top, newLow;
        if (!c->dpc_music_mode[x - 5]) continue;
        top = (int32_t)c->dpc_tops[x] + 1;
        newLow = (int32_t)(c->dpc_counters[x] & 0x00FF);
        if (c->dpc_tops[x] != 0) {
            newLow -= (int32_t)(whole % (uint32_t)top);
            if (newLow < 0) newLow += top;
        } else {
            newLow = 0;
        }
        if (newLow <= (int32_t)c->dpc_bottoms[x])
            c->dpc_flags[x] = 0x00;
        else if (newLow <= (int32_t)c->dpc_tops[x])
            c->dpc_flags[x] = 0xFF;
        c->dpc_counters[x] = (uint16_t)((c->dpc_counters[x] & 0x0700)
                            | (uint16_t)newLow);
    }
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

    case CART_MAPPER_FE:
        /* No in-cart-space hotspots. Bank switching happens via
         * cart_snoop_bus when the CPU accesses $01FE (RIOT space). */
        return c->data[c->bank * 4096u + a];

    case CART_MAPPER_DPC: {
        dpc_clock_rng(c);

        if (a < 0x0040) {
            /* DPC read port. Bits [5:3] = function, [2:0] = fetcher index. */
            uint8_t idx  = (uint8_t)(a & 0x07);
            uint8_t func = (uint8_t)((a >> 3) & 0x07);
            uint8_t result = 0;

            /* Update flag for this fetcher before returning data. */
            if ((c->dpc_counters[idx] & 0xFF) == c->dpc_tops[idx])
                c->dpc_flags[idx] = 0xFF;
            else if ((c->dpc_counters[idx] & 0xFF) == c->dpc_bottoms[idx])
                c->dpc_flags[idx] = 0x00;

            switch (func) {
            case 0: /* random number (DF0-3) or music amplitude (DF4-7) */
                if (idx < 4) {
                    result = c->dpc_random;
                } else {
                    static const uint8_t amp[8] = {
                        0x00, 0x04, 0x05, 0x09, 0x06, 0x0A, 0x0B, 0x0F
                    };
                    uint8_t m = 0;
                    dpc_update_music_fetchers(c);
                    if (c->dpc_music_mode[0] && c->dpc_flags[5]) m |= 0x01;
                    if (c->dpc_music_mode[1] && c->dpc_flags[6]) m |= 0x02;
                    if (c->dpc_music_mode[2] && c->dpc_flags[7]) m |= 0x04;
                    result = amp[m];
                }
                break;
            case 1: /* display data at counter */
                result = c->data[8192 + (2047 - (c->dpc_counters[idx] & 0x7FF))];
                break;
            case 2: /* display data AND'd with flag */
                result = (uint8_t)(c->data[8192 + (2047 - (c->dpc_counters[idx] & 0x7FF))]
                        & c->dpc_flags[idx]);
                break;
            case 7: /* flag register */
                result = c->dpc_flags[idx];
                break;
            default:
                result = 0;
                break;
            }

            /* Decrement counter (unless music-mode fetcher). */
            if (idx < 5 || !c->dpc_music_mode[idx - 5])
                c->dpc_counters[idx] = (uint16_t)((c->dpc_counters[idx] - 1) & 0x07FF);

            return result;
        }
        if (a < 0x0080) {
            /* DPC write port accessed via a READ — unusual but must not
             * fall through to program ROM. Return 0. */
            return 0;
        }
        /* $1080-$1FFF: program ROM with F8-style bank switching.
         * The first 0x80 bytes of each 4K bank are shadowed by the DPC
         * register ports and never reached here. */
        hotspot_f8(c, a);
        return c->data[c->bank * 4096u + a];
    }

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

    case CART_MAPPER_FE:
        /* ROM only; writes ignored. Bank switching is via $01FE snoop. */
        return;

    case CART_MAPPER_DPC: {
        dpc_clock_rng(c);

        if (a >= 0x0040 && a < 0x0080) {
            uint8_t idx  = (uint8_t)(a & 0x07);
            uint8_t func = (uint8_t)((a >> 3) & 0x07);
            switch (func) {
            case 0: /* set top + clear flag */
                c->dpc_tops[idx] = data;
                c->dpc_flags[idx] = 0x00;
                break;
            case 1: /* set bottom */
                c->dpc_bottoms[idx] = data;
                break;
            case 2: /* set counter low */
                if (idx >= 5 && c->dpc_music_mode[idx - 5])
                    c->dpc_counters[idx] = (uint16_t)((c->dpc_counters[idx] & 0x0700)
                                         | c->dpc_tops[idx]);
                else
                    c->dpc_counters[idx] = (uint16_t)((c->dpc_counters[idx] & 0x0700)
                                         | data);
                break;
            case 3: /* set counter high + music mode */
                c->dpc_counters[idx] = (uint16_t)(((uint16_t)(data & 0x07) << 8)
                                     | (c->dpc_counters[idx] & 0x00FF));
                if (idx >= 5)
                    c->dpc_music_mode[idx - 5] = (data & 0x10) != 0;
                break;
            case 6: /* reset random number generator */
                c->dpc_random = 1;
                break;
            default:
                break;
            }
            return;
        }
        /* Program-ROM writes: just handle F8 hotspots. */
        hotspot_f8(c, a);
        return;
    }
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

void cart_snoop_bus(struct cart *c, uint16_t addr, uint8_t data)
{
    if (c->mapper != CART_MAPPER_FE) return;

    /* FE state machine: if the PREVIOUS access hit $01FE, the data byte
     * on THIS access selects the bank. Then clear the flag regardless.
     * D5=0 → bank 1; D5=1 → bank 0 (equivalently: (data>>5)^7, low bit). */
    if (c->fe_pending) {
        c->bank = (uint8_t)(((data >> 5) ^ 7) & 1);
        c->fe_pending = false;
        return;
    }
    c->fe_pending = ((addr & 0x1FFF) == 0x01FE);
}
