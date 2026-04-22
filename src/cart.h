/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_CART_H
#define TIA_CART_H

#include <stddef.h>

#include "compat.h"

#define CART_MAX_SIZE      (64 * 1024)   /* 64K covers F0 / Megaboy */
#define CART_SC_RAM_SIZE   128
#define CART_DPC_DISP_SIZE 2048          /* DPC display data ROM (2K) */
#define CART_FA_RAM_SIZE   256           /* CBS RAM+: 256 bytes */
#define CART_E7_RAM_SIZE   2048          /* M-Network: 1K lower + 4×256 upper */

enum cart_mapper {
    CART_MAPPER_PLAIN = 0,  /* 2K or 4K ROM, no bank switching */
    CART_MAPPER_F8,         /* 8K,  2 banks × 4K, hotspots $1FF8/$1FF9 */
    CART_MAPPER_F6,         /* 16K, 4 banks × 4K, hotspots $1FF6-$1FF9 */
    CART_MAPPER_F4,         /* 32K, 8 banks × 4K, hotspots $1FF4-$1FFB */
    CART_MAPPER_E0,         /* 8K Parker Bros: 3 × 1K slots + fixed 1K */
    CART_MAPPER_3F,         /* Tigervision: banks × 2K, write to $00-$3F */
    CART_MAPPER_FE,         /* Activision: 8K, bank switch on $01FE access */
    CART_MAPPER_DPC,        /* Pitfall II: 8K prog + 2K display + co-processor */
    CART_MAPPER_FA,         /* CBS RAM+: 12K, 3 banks × 4K, 256B cart RAM */
    CART_MAPPER_E7,         /* M-Network: 16K, 8 banks × 2K, 2K cart RAM */
    CART_MAPPER_F0          /* Megaboy: 64K, 16 banks × 4K, +1 on $1FF0 access */
};

struct cart {
    uint8_t  data[CART_MAX_SIZE];
    uint32_t size;             /* original ROM size in bytes */
    uint8_t  mapper;           /* enum cart_mapper */
    uint8_t  bank;             /* current bank (F8/F6/F4/3F) */
    uint8_t  e0_slots[4];      /* E0: bank 0..7 per 1K slot; slot 3 ignored */
    uint8_t  sc_ram[CART_SC_RAM_SIZE];
    bool     sc_enabled;       /* SuperChip RAM active (auto-detect on write) */
    bool     fe_pending;       /* FE: previous bus access was at $01FE */

    /* DPC (Pitfall II co-processor) state. Active only when mapper == DPC.
     * The DPC has 8 data fetchers (DF0..DF7), a random number generator,
     * and a music oscillator for DF5/6/7. Display data ROM is at
     * data[8192..10239] (2K after the 8K program banks). */
    uint8_t  dpc_tops[8];
    uint8_t  dpc_bottoms[8];
    uint16_t dpc_counters[8];  /* 11-bit counters (bits 10:0) */
    uint8_t  dpc_flags[8];     /* 0x00 or 0xFF */
    bool     dpc_music_mode[3]; /* DF5, DF6, DF7 */
    uint8_t  dpc_random;       /* 8-bit LFSR, must be non-zero */
    uint64_t dpc_audio_cycles; /* CPU cycles at last music update */
    double   dpc_frac_clocks;  /* fractional DPC osc clocks remainder */
    const uint64_t *cpu_cycles; /* pointer to CPU cycle counter (for music timing) */

    /* FA (CBS RAM+): 256 bytes of cart RAM. Writes on $1000-$10FF, reads
     * on $1100-$11FF (same memory, different address windows). */
    uint8_t  fa_ram[CART_FA_RAM_SIZE];

    /* E7 (M-Network): 2K of cart RAM — one 1K block for the lower 2K slot
     * (when enabled via $1FE7) plus four 256-byte banks for the upper 2K
     * private RAM area ($1800-$19FF, bank selected via $1FE8-$1FEB).
     * `e7_lower_ram` is true when the lower 2K window shows RAM instead
     * of ROM. `e7_upper_bank` is the 0..3 selector for the 256-byte
     * private-RAM bank. */
    uint8_t  e7_ram[CART_E7_RAM_SIZE];
    bool     e7_lower_ram;
    uint8_t  e7_upper_bank;
};

/* Load ROM. Returns false if size is unsupported. Initialises mapper state
 * (bank, slots, SC flag) based on a size+signature heuristic. */
bool cart_load(struct cart *c, const void *rom, size_t size);

uint8_t cart_read(struct cart *c, uint16_t addr);
void    cart_write(struct cart *c, uint16_t addr, uint8_t data);

/* Called by the bus for EVERY CPU write, regardless of destination. Lets
 * mappers (currently only 3F) snoop writes to TIA-range addresses for
 * bank-switch triggers. No-op for mappers that don't need it. */
void    cart_snoop_write(struct cart *c, uint16_t addr, uint8_t data);

/* Called by the bus for EVERY bus access (read or write) with the 13-bit
 * address and the data on the bus. Only the FE mapper uses this — it
 * triggers a bank switch on the access AFTER $01FE, based on the data
 * byte at that subsequent access. No-op for other mappers. */
void    cart_snoop_bus(struct cart *c, uint16_t addr, uint8_t data);

#endif
