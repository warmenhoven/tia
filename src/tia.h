/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_TIA_H
#define TIA_TIA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TIA_HBLANK_CLOCKS    68
#define TIA_SCANLINE_CLOCKS  228
#define TIA_VISIBLE_WIDTH    160
#define TIA_MAX_SCANLINES    312   /* large enough for PAL */

struct tia {
    /* Framebuffer (max size fits NTSC or PAL). Libretro reads this. */
    uint32_t fb[TIA_VISIBLE_WIDTH * TIA_MAX_SCANLINES];
    const uint32_t *palette;       /* pointer to active 128-entry palette */

    /* Beam position */
    uint16_t hpos;                 /* 0..227 */
    uint16_t scanline;             /* 0..TIA_MAX_SCANLINES-1 */

    /* Frame delivery */
    uint32_t frame_number;
    bool     frame_ready;          /* set on VSYNC rising edge */

    /* Sync and blanking */
    bool     vsync;
    bool     vblank;

    /* CPU stall request (WSYNC). The bus layer polls this. */
    bool     rdy_asserted;

    /* Color registers */
    uint8_t  colubk;
    uint8_t  colupf;
    uint8_t  colup0;
    uint8_t  colup1;

    /* Playfield */
    uint8_t  pf0;      /* only bits 7..4 are used, mapped to PF pixels 0..3 */
    uint8_t  pf1;      /* bits 7..0 -> PF pixels 4..11 */
    uint8_t  pf2;      /* bits 0..7 -> PF pixels 12..19 */
    uint8_t  ctrlpf;   /* bit 0 = REF (mirror), bit 1 = SCORE, bit 2 = PFP */

    /* Players */
    uint8_t  grp0;
    uint8_t  grp1;
    uint8_t  grp0_latch;   /* shadow updated on GRP1 write */
    uint8_t  grp1_latch;   /* shadow updated on GRP0 write */
    uint8_t  nusiz0;       /* bits 0..2: copies/size; bits 4..5: missile size */
    uint8_t  nusiz1;
    bool     refp0;
    bool     refp1;
    bool     vdelp0;
    bool     vdelp1;
    int16_t  p0_pos;       /* visible-x where P0 sprite starts drawing */
    int16_t  p1_pos;
    uint8_t  hmp0;         /* horizontal motion (applied by HMOVE in M3e) */
    uint8_t  hmp1;

    /* Missiles — 1/2/4/8-clock blocks; size from NUSIZx bits 4-5;
     * copies from NUSIZx bits 0-2 (same as paired player). */
    bool     enam0;
    bool     enam1;
    int16_t  m0_pos;
    int16_t  m1_pos;
    uint8_t  hmm0;
    uint8_t  hmm1;
    bool     resmp0;       /* lock M0 to P0 center and hide */
    bool     resmp1;

    /* Ball — single copy; size from CTRLPF bits 4-5; color = COLUPF. */
    bool     enabl;
    bool     enabl_latch;  /* shadow updated on GRP1 write, used if VDELBL */
    bool     vdelbl;
    int16_t  bl_pos;
    uint8_t  hmbl;

    /* HMOVE: countdown of "extended HBLANK" clocks remaining. Each of
     * these visible color clocks shows black, creating the left-edge
     * "HMOVE comb" when HMOVE is strobed during HBLANK. */
    uint8_t  hmove_blank;

    /* Collision registers — bits 7 and 6 only; 0 bits unused.
     * cx[0]=CXM0P, [1]=CXM1P, [2]=CXP0FB, [3]=CXP1FB,
     * [4]=CXM0FB, [5]=CXM1FB, [6]=CXBLPF, [7]=CXPPMM. */
    uint8_t  cx[8];

    /* Audio — Brenner/die-shot model (ported from Stella AudioChannel).
     * Two channels, each a 5-bit "noise" shift register and a 4-bit
     * "pulse" shift register with shift-and-invert update rule. */
    struct {
        uint8_t audc;              /* 4 bits */
        uint8_t audf;              /* 5 bits */
        uint8_t audv;              /* 4 bits */
        uint8_t div_counter;
        uint8_t pulse_counter;
        uint8_t noise_counter;
        bool    clock_enable;
        bool    noise_feedback;
        bool    noise_counter_bit4;
        bool    pulse_counter_hold;
    } aud[2];
    uint32_t audio_sum[2];         /* running sum since last sample */
    uint32_t audio_sum_ct;
    int16_t  audio_mix[31];        /* precomputed DAC compression (0..30) */
    int16_t  audio_buf[2048];      /* queued samples; drained by libretro */
    uint16_t audio_buf_len;
};

extern const uint32_t tia_ntsc_palette[128];

void tia_init(struct tia *t);
void tia_reset(struct tia *t);
void tia_tick(struct tia *t);

uint8_t tia_read(struct tia *t, uint16_t addr);
void    tia_write(struct tia *t, uint16_t addr, uint8_t data);

size_t tia_serialize_size(void);
void   tia_serialize(const struct tia *t, void *buf);
bool   tia_deserialize(struct tia *t, const void *buf, size_t size);

/* Drain up to `max` queued audio samples into `out`, shifting any remainder
 * to the front of the internal buffer. Returns the number of samples copied. */
size_t tia_drain_audio(struct tia *t, int16_t *out, size_t max);

#endif
