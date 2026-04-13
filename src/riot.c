/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "riot.h"
#include <string.h>

/* ============================================================
 *   Address-bit helpers (6532 decoder within its chip-select region)
 * ============================================================ */

#define A9  0x0200u  /* RAM select: 0 = RAM, 1 = I/O + Timer */
#define A4  0x0010u  /* 1 = interval write, 0 = PA7 edge cfg (writes) */
#define A3  0x0008u  /* 1 = IRQ enable on interval write */
#define A2  0x0004u  /* 1 = timer region, 0 = I/O registers */
#define A1  0x0002u
#define A0  0x0001u

/* ============================================================
 *   Lifecycle
 * ============================================================ */

void riot_init(struct riot *r)
{
    memset(r, 0, sizeof(*r));
    r->pa_in = 0xFF;
    r->pb_in = 0xFF;
    /* Power-on convention: prescaler in T1024T mode, timer with a non-zero
     * "random-ish" value. Real hardware's power-on state is undefined; this
     * approximation keeps timer-sensitive games from instantly underflowing. */
    r->prescaler_div = 1024;
    r->timer = 0x77;
    r->pa7_last = r->pa_in & 0x80;
}

void riot_reset(struct riot *r)
{
    /* Per the datasheet, RAM is not reset. Ports/DDR/timer/flags clear. */
    r->pa_out = 0;
    r->pa_ddr = 0;
    r->pb_out = 0;
    r->pb_ddr = 0;
    r->timer = 0;
    r->prescaler_div = 1;
    r->prescaler_cnt = 0;
    r->timer_underflow = false;
    r->timer_irq_enable = false;
    r->pa7_edge_positive = false;
    r->pa7_irq_enable = false;
    r->pa7_edge_flag = false;
    r->pa7_last = r->pa_in & 0x80;
}

/* ============================================================
 *   Tick — one CPU cycle
 * ============================================================ */

void riot_tick(struct riot *r)
{
    /* PA7 edge detect */
    uint8_t pa7 = (uint8_t)(r->pa_in & 0x80);
    if (pa7 != r->pa7_last) {
        int rising = pa7 != 0;
        if ((rising && r->pa7_edge_positive) ||
            (!rising && !r->pa7_edge_positive)) {
            r->pa7_edge_flag = true;
        }
        r->pa7_last = pa7;
    }

    /* Timer prescaler */
    r->prescaler_cnt++;
    if (r->prescaler_cnt >= r->prescaler_div) {
        r->prescaler_cnt = 0;
        if (r->timer == 0) {
            r->timer_underflow = true;
            r->prescaler_div = 1;
        }
        r->timer--;  /* wraps 0x00 -> 0xFF on underflow */
    }
}

/* ============================================================
 *   Reads
 * ============================================================ */

static uint8_t port_read(uint8_t in, uint8_t out, uint8_t ddr)
{
    return (uint8_t)((out & ddr) | (in & (uint8_t)~ddr));
}

uint8_t riot_read(struct riot *r, uint16_t addr)
{
    if (!(addr & A9))
        return r->ram[addr & 0x7Fu];

    if (!(addr & A2)) {
        /* I/O registers — A1:A0 selects */
        switch (addr & (A1 | A0)) {
        case 0: return port_read(r->pa_in, r->pa_out, r->pa_ddr);
        case A0: return r->pa_ddr;
        case A1: return port_read(r->pb_in, r->pb_out, r->pb_ddr);
        default: return r->pb_ddr;
        }
    }

    /* Timer region — A0 selects INTIM vs TIMINT on read. */
    if (addr & A0) {
        uint8_t v = (uint8_t)((r->timer_underflow ? 0x80 : 0) |
                              (r->pa7_edge_flag   ? 0x40 : 0));
        /* Reading TIMINT clears the PA7 edge flag. */
        r->pa7_edge_flag = false;
        return v;
    }
    /* INTIM: reading the timer clears the underflow flag. */
    {
        uint8_t v = r->timer;
        r->timer_underflow = false;
        return v;
    }
}

/* ============================================================
 *   Writes
 * ============================================================ */

void riot_write(struct riot *r, uint16_t addr, uint8_t data)
{
    if (!(addr & A9)) {
        r->ram[addr & 0x7Fu] = data;
        return;
    }

    if (!(addr & A2)) {
        switch (addr & (A1 | A0)) {
        case 0:  r->pa_out = data; break;
        case A0: r->pa_ddr = data; break;
        case A1: r->pb_out = data; break;
        default: r->pb_ddr = data; break;
        }
        return;
    }

    /* Timer region */
    if (addr & A4) {
        /* Interval write: load timer, choose prescaler, latch IRQ-enable. */
        static const uint16_t divs[4] = { 1, 8, 64, 1024 };
        r->prescaler_div   = divs[addr & (A1 | A0)];
        r->prescaler_cnt   = 0;
        r->timer           = data;
        r->timer_underflow = false;
        r->timer_irq_enable = (addr & A3) != 0;
    } else {
        /* PA7 edge-detect configuration. A1 is don't-care. */
        r->pa7_edge_positive = (addr & A0) != 0;
        r->pa7_irq_enable    = (addr & A3) != 0;
    }
}

/* ============================================================
 *   Serialization
 * ============================================================ */

size_t riot_serialize_size(void)
{
    /* 128 RAM + 6 port bytes + 5 timer bytes + 4 edge bytes = 143 */
    return 128 + 6 + 5 + 4;
}

void riot_serialize(const struct riot *r, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    memcpy(p, r->ram, 128);      p += 128;
    *p++ = r->pa_in;
    *p++ = r->pa_out;
    *p++ = r->pa_ddr;
    *p++ = r->pb_in;
    *p++ = r->pb_out;
    *p++ = r->pb_ddr;
    *p++ = r->timer;
    *p++ = (uint8_t)(r->prescaler_div & 0xFF);
    *p++ = (uint8_t)(r->prescaler_div >> 8);
    *p++ = (uint8_t)(r->prescaler_cnt & 0xFF);
    *p++ = (uint8_t)(r->prescaler_cnt >> 8);
    *p++ = (uint8_t)((r->timer_underflow ? 1 : 0) |
                     (r->timer_irq_enable ? 2 : 0));
    *p++ = (uint8_t)((r->pa7_edge_positive ? 1 : 0) |
                     (r->pa7_irq_enable    ? 2 : 0) |
                     (r->pa7_edge_flag     ? 4 : 0));
    *p++ = r->pa7_last;
}

bool riot_deserialize(struct riot *r, const void *buf, size_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t tflags, eflags;
    if (size < riot_serialize_size()) return false;
    memcpy(r->ram, p, 128);      p += 128;
    r->pa_in  = *p++;
    r->pa_out = *p++;
    r->pa_ddr = *p++;
    r->pb_in  = *p++;
    r->pb_out = *p++;
    r->pb_ddr = *p++;
    r->timer = *p++;
    r->prescaler_div  = (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); p += 2;
    r->prescaler_cnt  = (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); p += 2;
    tflags = *p++;
    r->timer_underflow   = (tflags & 1) != 0;
    r->timer_irq_enable  = (tflags & 2) != 0;
    eflags = *p++;
    r->pa7_edge_positive = (eflags & 1) != 0;
    r->pa7_irq_enable    = (eflags & 2) != 0;
    r->pa7_edge_flag     = (eflags & 4) != 0;
    r->pa7_last = *p++;
    (void)p;
    return true;
}
