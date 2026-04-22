/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_RIOT_H
#define TIA_RIOT_H

#include <stddef.h>

#include "compat.h"

/* MOS 6532 RIOT: 128B RAM + 2 I/O ports + programmable timer.
 *
 * The libretro glue layer writes pa_in / pb_in directly to reflect the
 * current state of joysticks and console switches (active-low). Reads
 * of SWCHA / SWCHB combine those with the CPU-writable output latches
 * and direction registers. */
struct riot {
    uint8_t  ram[128];

    /* Port A — joystick 1 (bits 7-4) + joystick 2 (bits 3-0). */
    uint8_t  pa_in;
    uint8_t  pa_out;
    uint8_t  pa_ddr;

    /* Port B — console switches. */
    uint8_t  pb_in;
    uint8_t  pb_out;
    uint8_t  pb_ddr;

    /* Timer */
    uint8_t  timer;             /* INTIM current value */
    uint16_t prescaler_div;     /* 1, 8, 64, or 1024 */
    uint16_t prescaler_cnt;     /* counts cycles up to prescaler_div */
    bool     timer_underflow;   /* TIMINT bit 7 */
    bool     timer_irq_enable;  /* from A3 on last interval write */

    /* PA7 edge detect */
    bool     pa7_edge_positive; /* true = positive edge, false = negative */
    bool     pa7_irq_enable;
    bool     pa7_edge_flag;     /* TIMINT bit 6 */
    uint8_t  pa7_last;

    /* Callback fired when Port A's output or DDR latches change (i.e. the
     * effective pin state seen by a connected controller may have changed).
     * Used by the keypad controller to rescan the row drivers on each
     * SWCHA / SWACNT write. NULL = no callback. */
    void   (*pa_changed)(void *ctx);
    void    *pa_changed_ctx;
};

void riot_init(struct riot *r);
void riot_reset(struct riot *r);
void riot_tick(struct riot *r);

uint8_t riot_read(struct riot *r, uint16_t addr);
void    riot_write(struct riot *r, uint16_t addr, uint8_t data);

size_t riot_serialize_size(void);
void   riot_serialize(const struct riot *r, void *buf);
bool   riot_deserialize(struct riot *r, const void *buf, size_t size);

#endif
