/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_BUS_H
#define TIA_BUS_H

#include <stdint.h>
#include "cpu.h"
#include "tia.h"
#include "riot.h"
#include "cart.h"

/* The 2600's bus is just a few TTL gates. A12 selects cart vs internal;
 * when A12=0, A7 selects TIA (0) vs RIOT (1). Address lines A13-A15
 * don't physically exist on the 6507 — we mask to 13 bits.
 *
 * Every bus op represents one CPU cycle. Internally it ticks the TIA
 * three color clocks and the RIOT once. Read ops honour RDY (WSYNC)
 * by looping until the TIA releases the stall. */
struct bus {
    struct cpu  *cpu;
    struct tia  *tia;
    struct riot *riot;
    struct cart *cart;
};

void bus_init(struct bus *b, struct cpu *c, struct tia *t,
              struct riot *r, struct cart *cart);

/* cpu_bus-compatible callbacks. Pass &bus as ctx. */
uint8_t bus_read(void *ctx, uint16_t addr);
void    bus_write(void *ctx, uint16_t addr, uint8_t data);

#endif
