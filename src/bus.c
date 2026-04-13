/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "bus.h"

/* One CPU cycle = 3 color clocks. TIA is the master clock; RIOT runs at
 * CPU rate. */
static void bus_tick_one_cycle(struct bus *b)
{
    tia_tick(b->tia);
    tia_tick(b->tia);
    tia_tick(b->tia);
    riot_tick(b->riot);
}

void bus_init(struct bus *b, struct cpu *c, struct tia *t,
              struct riot *r, struct cart *cart)
{
    b->cpu  = c;
    b->tia  = t;
    b->riot = r;
    b->cart = cart;
}

uint8_t bus_read(void *ctx, uint16_t addr)
{
    struct bus *b = (struct bus *)ctx;
    uint16_t a;
    /* RDY stall: reads are suspended while TIA holds RDY. Each stall cycle
     * ticks the TIA/RIOT but no memory access occurs. Scanline length (228
     * color clocks) is divisible by 3, so cycle-aligned stalls resume exactly
     * at the next HBLANK start. Stall BEFORE the read-cycle tick so the read's
     * 3 color clocks land at the start of the new scanline (matching hw). */
    while (b->tia->rdy_asserted)
        bus_tick_one_cycle(b);
    bus_tick_one_cycle(b);

    a = (uint16_t)(addr & 0x1FFF);          /* 13-bit address bus */
    if (a & 0x1000) return cart_read(b->cart, a);
    if (a & 0x0080) return riot_read(b->riot, a);
    return tia_read(b->tia, a);
}

void bus_write(void *ctx, uint16_t addr, uint8_t data)
{
    struct bus *b = (struct bus *)ctx;
    uint16_t a;

    /* On real hardware, writes are latched on φ2 — within the CPU cycle,
     * not after it. Model this by performing the write at clock 2 of 3:
     * tick one TIA clock, apply the write, then tick the remaining two.
     *
     * Getting this right matters for STA WSYNC near the end of a scanline.
     * If we ticked all 3 clocks before applying the write, a write on the
     * last cycle of a scanline would cross the scanline wrap and re-assert
     * RDY on the *next* scanline, burning an entire extra scanline of
     * stall — visible as sprite stretching in games with tight flicker
     * kernels (Adventure's dragon). */
    tia_tick(b->tia);
    tia_tick(b->tia);
    /* Writes never stall on RDY (NMOS 6502 hardware behaviour). */
    a = (uint16_t)(addr & 0x1FFF);
    /* Let the cart snoop every CPU write. Mappers like 3F (Tigervision)
     * switch banks on TIA-range writes, so the snoop fires before the
     * actual destination routing below. */
    cart_snoop_write(b->cart, a, data);
    if (a & 0x1000)         cart_write(b->cart, a, data);
    else if (a & 0x0080)    riot_write(b->riot, a, data);
    else                    tia_write(b->tia, a, data);

    tia_tick(b->tia);
    riot_tick(b->riot);
}
