/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cart.h"
#include <string.h>

bool cart_load(struct cart *c, const void *rom, size_t size)
{
    if (size != 2048 && size != 4096) return false;
    memcpy(c->data, rom, size);
    if (size == 2048) memcpy(c->data + 2048, rom, 2048);
    c->size = (uint16_t)size;
    return true;
}

uint8_t cart_read(struct cart *c, uint16_t addr)
{
    return c->data[addr & 0x0FFF];
}

void cart_write(struct cart *c, uint16_t addr, uint8_t data)
{
    (void)c; (void)addr; (void)data;
    /* Tier-A 4K ROM: writes ignored. Mapper variants will override. */
}
