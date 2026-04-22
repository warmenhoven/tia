/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TIA_TIA_H
#define TIA_TIA_H

#include <stddef.h>

#include "compat.h"

#define TIA_HBLANK_CLOCKS    68
#define TIA_SCANLINE_CLOCKS  228
#define TIA_VISIBLE_WIDTH    160
#define TIA_MAX_SCANLINES    312   /* large enough for PAL */

/* Region determines palette, nominal fps, and audio sample rate. PAL60 is
 * PAL palette with NTSC-rate timing (used by a handful of European games
 * authored for 60 Hz displays). SECAM uses an 8-colour reduced palette. */
enum tia_region {
    TIA_REGION_NTSC  = 0,
    TIA_REGION_PAL   = 1,
    TIA_REGION_PAL60 = 2,
    TIA_REGION_SECAM = 3
};

/* How many past frames the auto-detect considers before locking. Skip
 * frame 0 (RAM-init garbage) and sample frames 1..N; the threshold is a
 * ~50-line gap (NTSC ~262, PAL ~312) so no median needed. */
#define TIA_DETECT_FRAMES 5

/* Palette variant. Both standard and z26 cover NTSC / PAL / SECAM.
 * "standard" is our default colour set; "z26" is an alternate set from
 * the z26 emulator, closer to the classic "TIA colour poster" and
 * preferred by some users. */
enum tia_palette_variant {
    TIA_PALETTE_STANDARD = 0,
    TIA_PALETTE_Z26      = 1
};

struct tia {
    /* Framebuffer (max size fits NTSC or PAL). Libretro reads this. */
    uint32_t fb[TIA_VISIBLE_WIDTH * TIA_MAX_SCANLINES];
    const uint32_t *palette;       /* pointer to active 128-entry palette */
    enum tia_region region;        /* currently selected region */
    enum tia_palette_variant palette_variant;

    /* Beam position */
    uint16_t hpos;                 /* 0..227 */
    uint16_t scanline;             /* 0..TIA_MAX_SCANLINES-1 */

    /* Frame delivery */
    uint32_t frame_number;
    bool     frame_ready;          /* set on VSYNC rising edge */
    /* Stable per-frame visible region, updated on VSYNC rise. libretro reads
     * these. 0xFFFF means "no VBLANK transition observed this frame." */
    uint16_t visible_start;
    uint16_t visible_end;
    /* In-progress tracking for the current frame; promoted to visible_* on
     * VSYNC rise. */
    uint16_t _pending_vstart;
    uint16_t _pending_vend;

    /* Sync and blanking */
    bool     vsync;
    bool     vblank;

    /* CPU stall request (WSYNC). The bus layer polls this. */
    bool     rdy_asserted;

    /* Pre-palette 7-bit colour:luminance (col<<3 | lum) driven to the DAC
     * on the most recent tick, 0 during HBLANK/VBLANK/VSYNC/HMOVE blank.
     * Populated by tia_tick; not serialised (recomputed on the next tick).
     * Used only by the oracle test harness. */
    uint8_t  last_colulum;

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

    /* Delay queue — deferred register writes. Real hardware pipelines TIA
     * register writes through several colour-clock-latch stages before
     * they take effect; games rely on this (e.g. new_year_2024's 6502
     * kernel rewrites PF0/1/2 mid-scanline on the assumption the PF
     * writes land 2 clocks after the store completes). We model it with
     * a small ring buffer: `delay_slots[i][j]` holds entries scheduled
     * to fire i clocks from `delay_head`. 8 slots covers HMOVE's 6-clock
     * max delay; 8 entries per slot covers the worst-case burst without
     * collisions. reg==0xFF marks an empty entry. */
    struct {
        uint8_t reg;
        uint8_t value;
    } delay_slots[8][8];
    uint8_t  delay_head;           /* next slot to fire */

    /* Collision registers — bits 7 and 6 only; 0 bits unused.
     * cx[0]=CXM0P, [1]=CXM1P, [2]=CXP0FB, [3]=CXP1FB,
     * [4]=CXM0FB, [5]=CXM1FB, [6]=CXBLPF, [7]=CXPPMM. */
    uint8_t  cx[8];

    /* Audio — Brenner die-shot-derived model (two channels, each a 5-bit
     * "noise" shift register and a 4-bit "pulse" shift register with
     * shift-and-invert update rule). */
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

    /* Input: 6 TIA input pins. Bit 7 of inpt[i] reflects the pin state.
     *   0..3 = paddle potentiometers (analog capacitor dump)
     *   4..5 = joystick fire buttons (digital)
     * When inpt_ground (VBLANK bit 7) is set:
     *   - INPT4/5 reads force-return 0 (game uses this to ground trigger)
     *   - INPT0-3 paddle capacitors are discharged (held at 0 until release)
     *
     * Paddle model: when VBLANK bit 7 goes false, the capacitor on each
     * paddle starts charging through the pot's resistance. Bit 7 of INPTi
     * flips from 0 to 1 once the cap crosses the comparator threshold;
     * the time to reach threshold is proportional to the paddle's rotary
     * position. paddle_charge_max[i] is the total clocks required (set by
     * the frontend from the analog input), paddle_charge_cnt[i] is the
     * running countdown. */
    uint8_t  inpt[6];
    bool     inpt_ground;
    uint16_t paddle_charge_max[4];  /* target charge time in TIA color clocks */
    uint16_t paddle_charge_cnt[4];  /* running countdown; 0 = cap fully charged */

    /* Region auto-detection. Counts scanlines between VSYNC rising edges
     * for the first few frames after boot; once detect_locked is true, the
     * result is written into detected_region. The libretro layer consults
     * detected_region when the user has set "region = auto". Frame 0 is
     * skipped because many ROMs emit a partial/garbage first frame while
     * RAM initialises. */
    uint16_t detect_samples[TIA_DETECT_FRAMES];
    uint8_t  detect_count;         /* number of usable samples collected */
    bool     detect_locked;
    enum tia_region detected_region;
};

extern const uint32_t tia_ntsc_palette[128];
extern const uint32_t tia_pal_palette[128];
extern const uint32_t tia_secam_palette[128];
extern const uint32_t tia_ntsc_palette_z26[128];
extern const uint32_t tia_pal_palette_z26[128];
extern const uint32_t tia_secam_palette_z26[128];

/* Swap palette to match `r` (uses the currently-selected palette variant).
 * Does not change timing / fps (the libretro layer handles that via
 * retro_get_system_av_info / SET_SYSTEM_AV_INFO). */
void tia_set_region(struct tia *t, enum tia_region r);

/* Change the palette variant and re-select the active palette for the
 * current region. */
void tia_set_palette_variant(struct tia *t, enum tia_palette_variant v);

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
