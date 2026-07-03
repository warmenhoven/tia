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

/* Count non-overlapping occurrences of `pat`, stopping once `minhits` is
 * reached.  Returns the number seen (>= minhits means "found enough"). */
static int count_bytes(const uint8_t *rom, size_t size,
                       const uint8_t *pat, size_t pat_len, int minhits)
{
    int hits = 0;
    size_t i = 0;
    if (pat_len > size) return 0;
    while (i + pat_len <= size) {
        if (memcmp(rom + i, pat, pat_len) == 0) {
            if (++hits >= minhits) return hits;
            i += pat_len;                 /* skip past this match window */
        } else {
            i++;
        }
    }
    return hits;
}

/* 8K mapper detection via a curated set of known instruction signatures. 8K can
 * be F8 / E0 (Parker Bros) / 3F (Tigervision) / FE (Activision) — plus
 * UA/SC/3E, not handled here. Address-range guessing produces false positives
 * (a data read near the ROM top looks like an E0 slot-select; one stray $xFF8
 * byte looks like F8), so we match the exact instruction signatures real games
 * use — operand high byte included, since the hotspots are mirrored and games
 * usually write the high $FFxx form — and default to F8. */
static uint8_t detect_8k_mapper(const uint8_t *rom, size_t size)
{
    /* "Looks like F8": STA $1FF9 (or its $FFF9 mirror) at least twice — a
     * real F8 game switches to bank 1 more than once.  Guards the FE check
     * so an F8 game with an FE-ish byte run isn't mis-called FE. */
    static const uint8_t f8_lo[3] = { 0x8D, 0xF9, 0x1F };  /* STA $1FF9 */
    static const uint8_t f8_hi[3] = { 0x8D, 0xF9, 0xFF };  /* STA $FFF9 */
    int looks_f8 = count_bytes(rom, size, f8_lo, 3, 2) >= 2
                || count_bytes(rom, size, f8_hi, 3, 2) >= 2;

    /* E0 (Parker Bros): curated signatures, including $FFxx/$5Fxx/$BFxx
     * mirror forms and a NOP-absolute slot poke. */
    {
        static const uint8_t e0[][3] = {
            { 0x8D, 0xE0, 0x1F },  /* STA $1FE0 */
            { 0x8D, 0xE0, 0x5F },  /* STA $5FE0 */
            { 0x8D, 0xE9, 0xFF },  /* STA $FFE9 */
            { 0x0C, 0xE0, 0x1F },  /* NOP $1FE0 */
            { 0xAD, 0xE0, 0x1F },  /* LDA $1FE0 */
            { 0xAD, 0xE9, 0xFF },  /* LDA $FFE9 */
            { 0xAD, 0xED, 0xFF },  /* LDA $FFED */
            { 0xAD, 0xF3, 0xBF }   /* LDA $BFF3 */
        };
        size_t i;
        for (i = 0; i < sizeof e0 / sizeof e0[0]; i++)
            if (find_bytes(rom, size, e0[i], 3)) return CART_MAPPER_E0;
    }

    /* 3F (Tigervision): STA $3F, required at least twice (a single $85 $3F
     * store to a zero-page temp is a common coincidence). */
    {
        static const uint8_t f3[2] = { 0x85, 0x3F };  /* STA $3F */
        if (count_bytes(rom, size, f3, 2, 2) >= 2) return CART_MAPPER_3F;
    }

    /* UA (UA Ltd): bank-select by accessing $0220/$0240 (and the $02xx
     * mirrors that decode to them). */
    {
        static const uint8_t ua[][3] = {
            { 0x8D, 0x40, 0x02 },  /* STA $0240   */
            { 0xAD, 0x40, 0x02 },  /* LDA $0240   */
            { 0xBD, 0x1F, 0x02 },  /* LDA $021F,X */
            { 0x2C, 0xC0, 0x02 },  /* BIT $02C0   */
            { 0x8D, 0xC0, 0x02 },  /* STA $02C0   */
            { 0xAD, 0xC0, 0x02 }   /* LDA $02C0   */
        };
        size_t i;
        for (i = 0; i < sizeof ua / sizeof ua[0]; i++)
            if (find_bytes(rom, size, ua[i], 3)) return CART_MAPPER_UA;
    }

    /* FE (Activision): JSR-through-stack signatures — only when the ROM
     * doesn't already look like F8. */
    if (!looks_f8) {
        static const uint8_t fe[][5] = {
            { 0x20, 0x00, 0xD0, 0xC6, 0xC5 }, /* Decathlon           */
            { 0x20, 0xC3, 0xF8, 0xA5, 0x82 }, /* Robot Tank          */
            { 0xD0, 0xFB, 0x20, 0x73, 0xFE }, /* Space Shuttle       */
            { 0xD0, 0xFB, 0x20, 0x68, 0xFE }, /* Space Shuttle SECAM */
            { 0x20, 0x00, 0xF0, 0x84, 0xD6 }  /* Thwocker            */
        };
        size_t i;
        for (i = 0; i < sizeof fe / sizeof fe[0]; i++)
            if (find_bytes(rom, size, fe[i], 5)) return CART_MAPPER_FE;
    }

    /* Default F8.  No reset-vector guessing: the curated E0 signatures above
     * already catch the indirect-access E0 games (Gyruss etc.) the old
     * heuristic rescued, without the false positives it caused. */
    return CART_MAPPER_F8;
}

/* E7 (M-Network) vs F6: both are 16K. E7 uses the hotspot range $1FE0-$1FEB
 * (bank selects + RAM enable), which F6 doesn't touch. Signature: any direct
 * access to an address that mirrors to that range. Cart space ($1000-$1FFF)
 * is mirrored at $3xxx, $5xxx, $7xxx, …, $Fxxx; games commonly reference the
 * upper mirrors (e.g. BurgerTime uses $FFE0-$FFEB), so we accept any high
 * byte whose low 5 bits are 0x1F. */
static uint8_t detect_16k_mapper(const uint8_t *rom, size_t size)
{
    /* Count LDA/LDX/LDY/STA/STX/STY absolute to $xxE0..$xxEB where
     * (xxHi & 0x1F) == 0x1F. Requires at least 3 matches to call it E7:
     * real M-Network games hit these hotspots dozens of times, while F6
     * ROMs occasionally score a single spurious match from a random data
     * byte that happens to align with an opcode + address pattern. */
    static const uint8_t ops[6] = { 0xAD, 0xAE, 0xAC, 0x8D, 0x8E, 0x8C };
    size_t i;
    int j, hits = 0;
    for (i = 0; i + 2 < size; i++) {
        for (j = 0; j < 6; j++) {
            if (rom[i] != ops[j]) continue;
            if ((rom[i + 2] & 0x1F) != 0x1F) continue;
            if (rom[i + 1] >= 0xE0 && rom[i + 1] <= 0xEB) {
                hits++;
                if (hits >= 3) return CART_MAPPER_E7;
            }
        }
    }
    return CART_MAPPER_F6;
}

/* CommaVid (CV): a 2K/4K ROM with 1K on-cart RAM. Detected by the RAM-access
 * instruction signatures the two known games use (RAM read port at $F3FF, write
 * port at $F400) — plain 2K/4K ROMs never contain these stores. */
static int detect_cv(const uint8_t *rom, size_t size)
{
    static const uint8_t sig_magicard[3]  = { 0x9D, 0xFF, 0xF3 };  /* STA $F3FF,X */
    static const uint8_t sig_videolife[3] = { 0x99, 0x00, 0xF4 };  /* STA $F400,Y */
    return find_bytes(rom, size, sig_magicard, 3)
        || find_bytes(rom, size, sig_videolife, 3);
}

/* ============================================================
 *   AR (Starpath/Arcadia Supercharger)
 * ============================================================ */

/* Synthetic Supercharger BIOS: drives the load bars and jumps to the loaded
 * game. The runtime copy is patched at offsets 109 and 281. */
static const uint8_t ar_dummy_rom[294] = {
    0xa5, 0xfa, 0x85, 0x80, 0x4c, 0x18, 0xf8, 0xff,
    0xff, 0xff, 0x78, 0xd8, 0xa0, 0x00, 0xa2, 0x00,
    0x94, 0x00, 0xe8, 0xd0, 0xfb, 0x4c, 0x50, 0xf8,
    0xa2, 0x00, 0xbd, 0x06, 0xf0, 0xad, 0xf8, 0xff,
    0xa2, 0x00, 0xad, 0x00, 0xf0, 0xea, 0xbd, 0x00,
    0xf7, 0xca, 0xd0, 0xf6, 0x4c, 0x50, 0xf8, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xa2, 0x03, 0xbc, 0x22, 0xf9, 0x94, 0xfa, 0xca,
    0x10, 0xf8, 0xa0, 0x00, 0xa2, 0x28, 0x94, 0x04,
    0xca, 0x10, 0xfb, 0xa2, 0x1c, 0x94, 0x81, 0xca,
    0x10, 0xfb, 0xa9, 0xff, 0xc9, 0x00, 0xd0, 0x03,
    0x4c, 0x13, 0xf9, 0xa9, 0x00, 0x85, 0x1b, 0x85,
    0x1c, 0x85, 0x1d, 0x85, 0x1e, 0x85, 0x1f, 0x85,
    0x19, 0x85, 0x1a, 0x85, 0x08, 0x85, 0x01, 0xa9,
    0x10, 0x85, 0x21, 0x85, 0x02, 0xa2, 0x07, 0xca,
    0xca, 0xd0, 0xfd, 0xa9, 0x00, 0x85, 0x20, 0x85,
    0x10, 0x85, 0x11, 0x85, 0x02, 0x85, 0x2a, 0xa9,
    0x05, 0x85, 0x0a, 0xa9, 0xff, 0x85, 0x0d, 0x85,
    0x0e, 0x85, 0x0f, 0x85, 0x84, 0x85, 0x85, 0xa9,
    0xf0, 0x85, 0x83, 0xa9, 0x74, 0x85, 0x09, 0xa9,
    0x0c, 0x85, 0x15, 0xa9, 0x1f, 0x85, 0x17, 0x85,
    0x82, 0xa9, 0x07, 0x85, 0x19, 0xa2, 0x08, 0xa0,
    0x00, 0x85, 0x02, 0x88, 0xd0, 0xfb, 0x85, 0x02,
    0x85, 0x02, 0xa9, 0x02, 0x85, 0x02, 0x85, 0x00,
    0x85, 0x02, 0x85, 0x02, 0x85, 0x02, 0xa9, 0x00,
    0x85, 0x00, 0xca, 0x10, 0xe4, 0x06, 0x83, 0x66,
    0x84, 0x26, 0x85, 0xa5, 0x83, 0x85, 0x0d, 0xa5,
    0x84, 0x85, 0x0e, 0xa5, 0x85, 0x85, 0x0f, 0xa6,
    0x82, 0xca, 0x86, 0x82, 0x86, 0x17, 0xe0, 0x0a,
    0xd0, 0xc3, 0xa9, 0x02, 0x85, 0x01, 0xa2, 0x1c,
    0xa0, 0x00, 0x84, 0x19, 0x84, 0x09, 0x94, 0x81,
    0xca, 0x10, 0xfb, 0xa6, 0x80, 0xdd, 0x00, 0xf0,
    0xa9, 0x9a, 0xa2, 0xff, 0xa0, 0x00, 0x9a, 0x4c,
    0xfa, 0x00, 0xcd, 0xf8, 0xff, 0x4c
};

/* Default 256-byte tape-load header (from z26), used only when a raw load
 * lacks its own (not expected for real Supercharger images). */
static const uint8_t ar_default_header[256] = {
    0xac, 0xfa, 0x0f, 0x18, 0x62, 0x00, 0x24, 0x02,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c,
    0x01, 0x05, 0x09, 0x0d, 0x11, 0x15, 0x19, 0x1d,
    0x02, 0x06, 0x0a, 0x0e, 0x12, 0x16, 0x1a, 0x1e,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

/* Apply a bank/RAM/power configuration byte: D4-D0 select the RAM/ROM window
 * layout, D1 = write-enable, D0 = ROM power. */
static void ar_bank_config(struct cart *c, uint8_t config)
{
    static const uint32_t OFFSET_0[8] = {
        2 * CART_AR_BANK_SIZE, 0 * CART_AR_BANK_SIZE,
        2 * CART_AR_BANK_SIZE, 0 * CART_AR_BANK_SIZE,
        2 * CART_AR_BANK_SIZE, 1 * CART_AR_BANK_SIZE,
        2 * CART_AR_BANK_SIZE, 1 * CART_AR_BANK_SIZE
    };
    static const uint32_t OFFSET_1[8] = {
        3 * CART_AR_BANK_SIZE, 3 * CART_AR_BANK_SIZE,
        0 * CART_AR_BANK_SIZE, 2 * CART_AR_BANK_SIZE,
        3 * CART_AR_BANK_SIZE, 3 * CART_AR_BANK_SIZE,
        1 * CART_AR_BANK_SIZE, 2 * CART_AR_BANK_SIZE
    };
    int bank_config = (config >> 2) & 7;

    c->ar_bank          = (uint8_t)(config & 0x1F);
    c->ar_power         = !(config & 1);
    c->ar_write_enabled = (config & 2) ? 1 : 0;
    c->ar_offset[0]     = OFFSET_0[bank_config];
    c->ar_offset[1]     = OFFSET_1[bank_config];
}

/* Set up the synthetic BIOS in the 2K ROM bank of the live image. */
static void ar_init_rom(struct cart *c)
{
    uint8_t code[294];
    memcpy(code, ar_dummy_rom, sizeof code);
    code[109] = 0xFF;   /* 0xFF -> skip the SC BIOS load bars (fast load, the
                         * de-facto reference default); 0x00 shows the bars,
                         * whose ~3 s animation delays the load past frame 190. */
    code[281] = 0x00;   /* value left in A on BIOS exit; fixed 0 is fine */

    /* Fill the ROM bank with an illegal 6502 opcode (0x02 jams a real CPU),
     * then drop the BIOS code over the start of it. */
    memset(c->ar_image + CART_AR_RAM_SIZE, 0x02, CART_AR_BANK_SIZE);
    memcpy(c->ar_image + CART_AR_RAM_SIZE, code, sizeof code);

    /* 6502 vectors point at the initial load code at $F80A. */
    c->ar_image[CART_AR_RAM_SIZE + CART_AR_BANK_SIZE - 4] = 0x0A;
    c->ar_image[CART_AR_RAM_SIZE + CART_AR_BANK_SIZE - 3] = 0xF8;
    c->ar_image[CART_AR_RAM_SIZE + CART_AR_BANK_SIZE - 2] = 0x0A;
    c->ar_image[CART_AR_RAM_SIZE + CART_AR_BANK_SIZE - 1] = 0xF8;
}

/* Copy the requested tape load's pages into the live RAM image and hand the
 * BIOS its bank-switch/start info through the 2600's RIOT RAM. Page
 * checksums are not verified. */
static void ar_load_into_ram(struct cart *c, uint8_t load)
{
    uint16_t image;

    for (image = 0; image < c->ar_num_loads; image++) {
        uint32_t off = (uint32_t)image * CART_AR_LOAD_SIZE;
        const uint8_t *hdr;
        unsigned j;

        if (c->data[off + CART_AR_IMAGE_SIZE + 5] != load)
            continue;

        hdr = &c->data[off + CART_AR_IMAGE_SIZE];   /* 256-byte load header */

        for (j = 0; j < hdr[3]; j++) {
            unsigned bank = hdr[16 + j] & 0x03;
            unsigned page = (hdr[16 + j] >> 2) & 0x07;
            if (bank < 3)
                memcpy(&c->ar_image[bank * CART_AR_BANK_SIZE + page * 256u],
                       &c->data[off + j * 256u], 256);
        }

        /* Pass the bank-switch byte and start address to the BIOS via the
         * 2600's RIOT RAM: 6507 $FE/$FF/$80 -> ar_ram[0x7E]/[0x7F]/[0x00]. */
        c->ar_ram[0x7E] = hdr[0];
        c->ar_ram[0x7F] = hdr[1];
        c->ar_ram[0x00] = hdr[2];
        return;
    }
}

/* ============================================================
 *   Load
 * ============================================================ */

bool cart_load(struct cart *c, const void *rom, size_t size)
{
    memset(c, 0, sizeof(*c));

    /* AR (Starpath/Arcadia Supercharger): the on-tape format is a whole
     * number of 8448-byte loads (8448/16896/25344/33792 bytes), none of
     * which collide with a standard ROM size. The raw loads stay in
     * c->data[]; the live 8K image (RAM + BIOS) is built in c->ar_image. */
    if (size > 0 && size % CART_AR_LOAD_SIZE == 0) {
        if (size > CART_MAX_SIZE) return false;
        memcpy(c->data, rom, size);
        c->size         = (uint32_t)size;
        c->ar_num_loads = (uint8_t)(size / CART_AR_LOAD_SIZE);
        c->mapper       = CART_MAPPER_AR;

        /* If a raw load lacks its own header, append the default one after
         * the 8K image area (not expected for real images). */
        if (size < CART_AR_LOAD_SIZE)
            memcpy(c->data + CART_AR_IMAGE_SIZE, ar_default_header,
                   sizeof ar_default_header);

        /* RAM (ar_image[0..RAM_SIZE)) is already zero from the memset. */
        ar_init_rom(c);

        c->ar_write_enabled  = 0;
        c->ar_power          = 1;
        c->ar_data_hold      = 0;
        c->ar_distinct_at_set = 0;
        c->ar_write_pending  = 0;
        ar_bank_config(c, 0);
        return true;
    }

    switch (size) {
    case 2048:
        if (detect_cv(rom, 2048)) {
            /* 2K CV: the ROM sits at $F800-$FFFF; the 1K RAM powers up clear. */
            memcpy(c->data, rom, 2048);
            c->size   = 2048;
            c->mapper = CART_MAPPER_CV;
            return true;
        }
        memcpy(c->data, rom, 2048);
        memcpy(c->data + 2048, rom, 2048);   /* mirror into 4K window */
        c->size   = 2048;
        c->mapper = CART_MAPPER_PLAIN;
        return true;

    case 4096:
        if (detect_cv(rom, 4096)) {
            /* 4K CV (e.g. a MagiCard image with a saved listing): the first 2K
             * is the initial RAM contents, the second 2K is the ROM. */
            memcpy(c->data, rom + 2048, 2048);
            memcpy(c->cv_ram, rom, CART_CV_RAM_SIZE);
            c->size   = 2048;
            c->mapper = CART_MAPPER_CV;
            return true;
        }
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

    case 12288:
        /* FA / CBS RAM+: 3 × 4K banks, 256 bytes cart RAM.
         * Reset vector lives in bank 2 (top), so boot there. */
        memcpy(c->data, rom, 12288);
        c->size   = 12288;
        c->mapper = CART_MAPPER_FA;
        c->bank   = 2;
        return true;

    case 16384:
        memcpy(c->data, rom, 16384);
        c->size   = 16384;
        c->mapper = detect_16k_mapper(rom, size);
        if (c->mapper == CART_MAPPER_F6) {
            c->bank = 3;                     /* F6: last bank */
        } else {
            /* E7: bank 7 is hardwired to the fixed upper 2K, so the
             * switchable lower 2K starts in bank 0 by convention. The
             * reset vector is in the fixed upper region regardless. */
            c->bank = 0;
        }
        return true;

    case 32768:
        memcpy(c->data, rom, 32768);
        c->size   = 32768;
        c->mapper = CART_MAPPER_F4;
        c->bank   = 7;                       /* last bank */
        return true;

    case 65536:
        /* F0 / Megaboy: 16 × 4K banks, advance on $1FF0 access.
         * Boot bank is 15 (last, holds the reset vector). */
        memcpy(c->data, rom, 65536);
        c->size   = 65536;
        c->mapper = CART_MAPPER_F0;
        c->bank   = 15;
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

    case CART_MAPPER_UA:
        /* No in-cart-space hotspots. Bank switching happens via
         * cart_snoop_bus when the CPU accesses $0220/$0240 (RIOT space). */
        return c->data[c->bank * 4096u + a];

    case CART_MAPPER_CV:
        /* $F000-$F3FF reads RAM; $F400-$F7FF is the write port (a read there
         * returns the same 1K cell); $F800-$FFFF is the 2K ROM. */
        if (a < 0x400) return c->cv_ram[a];
        if (a < 0x800) return c->cv_ram[a - 0x400];
        return c->data[a - 0x800];

    case CART_MAPPER_AR: {
        /* Decode the full 13-bit address (hotspots at $1850 and $1FF8). */
        uint16_t aa = (uint16_t)(addr & 0x1FFF);

        /* "Dummy" SC BIOS hotspot for reading a load: only live while the
         * BIOS ROM is mapped into the upper window. */
        if (aa == 0x1850 && c->ar_offset[1] == CART_AR_RAM_SIZE) {
            uint8_t load = c->ar_ram[0x00];   /* BIOS leaves load # at $0080 */
            ar_load_into_ram(c, load);
            return c->ar_image[(aa & 0x7FF) + c->ar_offset[1]];
        }

        /* Cancel a pending write if more than 5 distinct accesses elapsed. */
        if (c->ar_write_pending &&
            *c->ar_access > c->ar_distinct_at_set + 5)
            c->ar_write_pending = 0;

        if (!(aa & 0x0F00) && (!c->ar_write_enabled || !c->ar_write_pending)) {
            /* Latch the data-hold register (low address byte). */
            c->ar_data_hold       = (uint8_t)aa;
            c->ar_distinct_at_set = *c->ar_access;
            c->ar_write_pending   = 1;
        } else if (aa == 0x1FF8) {
            /* Bank-configuration hotspot. */
            c->ar_write_pending = 0;
            ar_bank_config(c, c->ar_data_hold);
        } else if (c->ar_write_enabled && c->ar_write_pending &&
                   *c->ar_access == c->ar_distinct_at_set + 5) {
            /* Delayed write lands exactly 5 accesses after the latch. */
            if ((aa & 0x800) == 0)
                c->ar_image[(aa & 0x7FF) + c->ar_offset[0]] = c->ar_data_hold;
            else if (c->ar_offset[1] != CART_AR_RAM_SIZE)   /* can't poke ROM */
                c->ar_image[(aa & 0x7FF) + c->ar_offset[1]] = c->ar_data_hold;
            c->ar_write_pending = 0;
        }

        return c->ar_image[(aa & 0x7FF) + c->ar_offset[(aa & 0x800) ? 1 : 0]];
    }

    case CART_MAPPER_FA:
        /* Hotspots $1FF8/9/A select banks 0/1/2. Cart RAM: writes on
         * $1000-$10FF, reads on $1100-$11FF (same 256 bytes, windowed). */
        if      (a == 0xFF8) c->bank = 0;
        else if (a == 0xFF9) c->bank = 1;
        else if (a == 0xFFA) c->bank = 2;
        if (a >= 0x100 && a < 0x200)
            return c->fa_ram[a - 0x100];
        return c->data[c->bank * 4096u + a];

    case CART_MAPPER_E7: {
        /* Memory map:
         *   $1000-$17FF lower 2K: switchable ROM bank 0..6, or the 1K RAM
         *       block when e7_lower_ram is set (writes $1000-$13FF, reads
         *       $1400-$17FF — same 1K).
         *   $1800-$19FF upper 2K private RAM (256-byte bank × 4): writes
         *       $1800-$18FF, reads $1900-$19FF.
         *   $1A00-$1FFF: hardwired to bank 7 (last 2K of ROM).
         *
         * Hotspots: $1FE0..$1FE6 select lower ROM bank 0..6; $1FE7 enables
         * lower RAM; $1FE8..$1FEB select upper RAM bank 0..3. */
        uint8_t ret;
        if      (a >= 0xFE0 && a <= 0xFE6) { c->bank = (uint8_t)(a & 7); c->e7_lower_ram = false; }
        else if (a == 0xFE7)               { c->e7_lower_ram = true; }
        else if (a >= 0xFE8 && a <= 0xFEB) { c->e7_upper_bank = (uint8_t)(a & 3); }

        if (a < 0x400) {
            /* Lower-slot write window when RAM enabled; otherwise ROM. */
            if (c->e7_lower_ram) return 0;  /* write-only window */
            return c->data[c->bank * 2048u + a];
        }
        if (a < 0x800) {
            /* Lower-slot read window. RAM (1K block) when enabled,
             * otherwise continuation of the same ROM bank. */
            if (c->e7_lower_ram) return c->e7_ram[a - 0x400];
            return c->data[c->bank * 2048u + a];
        }
        if (a < 0x900) {
            /* Upper private-RAM write window (write-only; read returns 0). */
            return 0;
        }
        if (a < 0xA00) {
            /* Upper private-RAM read window. */
            ret = c->e7_ram[1024u + (uint16_t)c->e7_upper_bank * 256u + (a - 0x900)];
            return ret;
        }
        /* $1A00-$1FFF: fixed upper 2K (bank 7). Offset within that bank
         * is (a - 0x800) since bank 7 covers the 2K starting at offset
         * 0x800 of our view. */
        return c->data[7u * 2048u + (a - 0x800)];
    }

    case CART_MAPPER_F0:
        /* Any access to $1FF0 advances bank by 1, wrapping at 16. */
        if (a == 0xFF0) c->bank = (uint8_t)((c->bank + 1) & 0x0F);
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

    case CART_MAPPER_UA:
        /* ROM only; bank switching is via the $0220/$0240 snoop. */
        return;

    case CART_MAPPER_CV:
        /* Writes land only through the $F400-$F7FF write port. */
        if (a >= 0x400 && a < 0x800)
            c->cv_ram[a - 0x400] = data;
        return;

    case CART_MAPPER_AR: {
        /* Same state machine as the read path, minus the $1850 load hotspot
         * and the return value. The data byte on the bus is ignored. */
        uint16_t aa = (uint16_t)(addr & 0x1FFF);

        if (c->ar_write_pending &&
            *c->ar_access > c->ar_distinct_at_set + 5)
            c->ar_write_pending = 0;

        if (!(aa & 0x0F00) && (!c->ar_write_enabled || !c->ar_write_pending)) {
            c->ar_data_hold       = (uint8_t)aa;
            c->ar_distinct_at_set = *c->ar_access;
            c->ar_write_pending   = 1;
        } else if (aa == 0x1FF8) {
            c->ar_write_pending = 0;
            ar_bank_config(c, c->ar_data_hold);
        } else if (c->ar_write_enabled && c->ar_write_pending &&
                   *c->ar_access == c->ar_distinct_at_set + 5) {
            if ((aa & 0x800) == 0)
                c->ar_image[(aa & 0x7FF) + c->ar_offset[0]] = c->ar_data_hold;
            else if (c->ar_offset[1] != CART_AR_RAM_SIZE)
                c->ar_image[(aa & 0x7FF) + c->ar_offset[1]] = c->ar_data_hold;
            c->ar_write_pending = 0;
        }
        return;
    }

    case CART_MAPPER_FA:
        if (a == 0xFF8)      { c->bank = 0; return; }
        else if (a == 0xFF9) { c->bank = 1; return; }
        else if (a == 0xFFA) { c->bank = 2; return; }
        if (a < 0x100)
            c->fa_ram[a] = data;
        return;

    case CART_MAPPER_E7:
        if      (a >= 0xFE0 && a <= 0xFE6) { c->bank = (uint8_t)(a & 7); c->e7_lower_ram = false; return; }
        else if (a == 0xFE7)               { c->e7_lower_ram = true;  return; }
        else if (a >= 0xFE8 && a <= 0xFEB) { c->e7_upper_bank = (uint8_t)(a & 3); return; }
        if (a < 0x400 && c->e7_lower_ram) {
            c->e7_ram[a] = data;
            return;
        }
        if (a >= 0x800 && a < 0x900) {
            c->e7_ram[1024u + (uint16_t)c->e7_upper_bank * 256u + (a - 0x800)] = data;
            return;
        }
        return;

    case CART_MAPPER_F0:
        if (a == 0xFF0) c->bank = (uint8_t)((c->bank + 1) & 0x0F);
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
    if (c->mapper == CART_MAPPER_UA) {
        /* Any access (read or write) to $0220/$0240 selects the low/high 4K
         * bank.  The $02xx mirrors that decode here are folded in via the
         * 0x1260 mask (e.g. $02C0 -> $0240). */
        switch (addr & 0x1260) {
        case 0x0220: c->bank = 0; break;
        case 0x0240: c->bank = 1; break;
        default:                  break;
        }
        return;
    }

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
