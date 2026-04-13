/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_CART_H
#define TIA_CART_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CART_4K 4096

struct cart {
    uint8_t data[CART_4K];
    uint16_t size;       /* original ROM size */
};

/* Load 2K or 4K ROM. Smaller ROMs are mirrored to fill the 4K window.
 * Returns false on unsupported size. */
bool cart_load(struct cart *c, const void *rom, size_t size);

uint8_t cart_read(struct cart *c, uint16_t addr);
void    cart_write(struct cart *c, uint16_t addr, uint8_t data);

#endif
