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
    {
        uint8_t v;
        if (a & 0x1000)      v = cart_read(b->cart, a);
        else if (a & 0x0080) v = riot_read(b->riot, a);
        else                 v = tia_read(b->tia, a);
        cart_snoop_bus(b->cart, a, v);
        return v;
    }
}

void bus_write(void *ctx, uint16_t addr, uint8_t data)
{
    struct bus *b = (struct bus *)ctx;
    uint16_t a;

    /* Write takes effect at the end of the cycle: all 3 TIA color clocks
     * happen first, then RIOT ticks, then the write lands. This produces
     * correct STA WSYNC behaviour at scanline boundaries: if the scanline
     * wraps during the 3 ticks and the write is WSYNC, tia_write sees
     * hpos=0 and skips the RDY assertion (WSYNC "missed the scanline"). */
    tia_tick(b->tia);
    tia_tick(b->tia);
    tia_tick(b->tia);
    /* RIOT ticks for this cycle BEFORE the write lands. A timer write
     * loads a fresh interval value; that value must NOT be ticked by
     * this cycle's prescaler clock (the tick corresponds to the write
     * cycle itself, which only delivers the new value). */
    riot_tick(b->riot);
    /* Writes never stall on RDY (NMOS 6502 hardware behaviour). */
    a = (uint16_t)(addr & 0x1FFF);
    /* Let the cart snoop every CPU write. Mappers like 3F (Tigervision)
     * switch banks on TIA-range writes, so the snoop fires before the
     * actual destination routing below. */
    cart_snoop_write(b->cart, a, data);
    if (a & 0x1000)         cart_write(b->cart, a, data);
    else if (a & 0x0080)    riot_write(b->riot, a, data);
    else                    tia_write(b->tia, a, data);
    cart_snoop_bus(b->cart, a, data);
}
