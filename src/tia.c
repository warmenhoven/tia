/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "tia.h"
#include <string.h>
#include <math.h>

#include "palette.h"


void tia_set_region(struct tia *t, enum tia_region r)
{
    int z26 = (t->palette_variant == TIA_PALETTE_Z26);
    t->region = r;
    switch (r) {
    case TIA_REGION_PAL:
    case TIA_REGION_PAL60:
        t->palette = z26 ? tia_pal_palette_z26 : tia_pal_palette;
        break;
    case TIA_REGION_SECAM:
        t->palette = z26 ? tia_secam_palette_z26 : tia_secam_palette;
        break;
    case TIA_REGION_NTSC:
    default:
        t->palette = z26 ? tia_ntsc_palette_z26 : tia_ntsc_palette;
        break;
    }
}

void tia_set_palette_variant(struct tia *t, enum tia_palette_variant v)
{
    t->palette_variant = v;
    tia_set_region(t, t->region);   /* re-select the active table */
}

/* ============================================================
 *   Lifecycle
 * ============================================================ */

void tia_init(struct tia *t)
{
    int i, j;
    memset(t, 0, sizeof(*t));
    tia_set_region(t, TIA_REGION_NTSC);
    t->detected_region = TIA_REGION_NTSC;
    /* Mark every delay-queue slot empty (reg==0xFF). memset-0 would be
     * "slot contains a queued write of value 0 to register 0" — VSYNC —
     * which would fire erroneously on the first frame. */
    for (i = 0; i < 8; i++)
        for (j = 0; j < 8; j++)
            t->delay_slots[i][j].reg = 0xFF;
    /* 0xFFFF is the "not yet observed" sentinel. memset-0 would make the
     * VBLANK handler's "only set if still 0xFFFF" guard reject the first
     * latch, so the libretro layer would never see a real visible_start. */
    t->visible_start = 0xFFFF;
    t->visible_end   = 0xFFFF;
    t->_pending_vstart = 0xFFFF;
    t->_pending_vend   = 0xFFFF;
    /* DAC compression curve: v/30 through a saturating divider that models
     * the R-2R ladder loading with two channels summed. R_MAX = 30, R = 1. */
    for (i = 0; i <= 30; i++) {
        double v = (double)i;
        double s = 32767.0 * (v / 30.0) * (30.0 + 30.0) / (30.0 + v);
        t->audio_mix[i] = (int16_t)floor(s);
    }
    /* Input pins float high when nothing is connected / no button pressed. */
    for (i = 0; i < 6; i++) t->inpt[i] = 0x80;
}

void tia_reset(struct tia *t)
{
    memset(t->fb, 0, sizeof(t->fb));
    t->hpos = 0;
    t->scanline = 0;
    t->frame_number = 0;
    t->frame_ready = false;
    t->visible_start = 0xFFFF;
    t->visible_end   = 0xFFFF;
    t->_pending_vstart = 0xFFFF;
    t->_pending_vend   = 0xFFFF;
    t->vsync = false;
    t->vblank = false;
    t->rdy_asserted = false;
    t->colubk = 0;
    t->colupf = 0;
    t->colup0 = 0;
    t->colup1 = 0;
    t->pf0 = 0;
    t->pf1 = 0;
    t->pf2 = 0;
    t->ctrlpf = 0;
    t->grp0 = 0; t->grp1 = 0;
    t->grp0_latch = 0; t->grp1_latch = 0;
    t->nusiz0 = 0; t->nusiz1 = 0;
    t->refp0 = false; t->refp1 = false;
    t->vdelp0 = false; t->vdelp1 = false;
    t->p0_pos = 0; t->p1_pos = 0;
    t->hmp0 = 0; t->hmp1 = 0;
    t->enam0 = false; t->enam1 = false;
    t->m0_pos = 0; t->m1_pos = 0;
    t->hmm0 = 0; t->hmm1 = 0;
    t->resmp0 = false; t->resmp1 = false;
    t->enabl = false; t->enabl_latch = false;
    t->vdelbl = false;
    t->bl_pos = 0; t->hmbl = 0;
    t->hmove_blank = 0;
    t->hm_remaining[0] = t->hm_remaining[1] = t->hm_remaining[2] =
        t->hm_remaining[3] = t->hm_remaining[4] = 0;
    t->hmove_active = false;
    t->hmove_ticks  = 0;
    {
        int i, j;
        for (i = 0; i < 8; i++)
            for (j = 0; j < 8; j++)
                t->delay_slots[i][j].reg = 0xFF;
        t->delay_head = 0;
    }
    memset(t->cx, 0, sizeof(t->cx));
    memset(&t->aud[0], 0, sizeof(t->aud[0]));
    memset(&t->aud[1], 0, sizeof(t->aud[1]));
    t->audio_sum[0] = t->audio_sum[1] = 0;
    t->audio_sum_ct = 0;
    t->audio_buf_len = 0;
    {
        int i;
        for (i = 0; i < 6; i++) t->inpt[i] = 0x80;
        for (i = 0; i < 4; i++) {
            t->paddle_charge_max[i] = 0;
            t->paddle_charge_cnt[i] = 0;
        }
    }
    t->inpt_ground = false;
    /* Re-arm auto-detect after a user-initiated reset: if they reset to
     * switch ROMs (via load-content), the timing pattern may differ from
     * the previous game, so the lock should refresh. */
    memset(t->detect_samples, 0, sizeof(t->detect_samples));
    t->detect_count = 0;
    t->detect_locked = false;
}

/* ============================================================
 *   Tick — one color clock
 * ============================================================ */

/* Is the playfield emitting at PF pixel index 0..19? */
static int pf_bit_on(const struct tia *t, uint16_t bit)
{
    if (bit < 4)  return (t->pf0 >> (4 + bit))        & 1;
    if (bit < 12) return (t->pf1 >> (11 - bit))       & 1;
    return          (t->pf2 >> (bit - 12))       & 1;
}

/* NUSIZ (bits 0-2) → sprite configuration. */
static int nusiz_copies(uint8_t n)
{
    static const int c[8] = { 1, 2, 2, 3, 2, 1, 3, 1 };
    return c[n & 0x07];
}
static int nusiz_pixel_width(uint8_t n)
{
    static const int w[8] = { 1, 1, 1, 1, 1, 2, 1, 4 };
    return w[n & 0x07];
}
static int nusiz_copy_spacing(uint8_t n)
{
    static const int s[8] = { 0, 16, 32, 16, 64, 0, 32, 0 };
    return s[n & 0x07];
}

/* ============================================================
 *   Audio — two-channel Brenner/silicon model
 * ============================================================ */

#define AUD_C(ch) (t->aud[ch])

/* Per-channel current 4-bit DAC contribution (poly4's output bit gates AUDV). */
static uint8_t audio_actual_volume(const struct tia *t, int ch)
{
    return (uint8_t)((t->aud[ch].pulse_counter & 0x01) * t->aud[ch].audv);
}

static void audio_phase0(struct tia *t, int ch)
{
    uint8_t audc = t->aud[ch].audc;
    if (t->aud[ch].clock_enable) {
        t->aud[ch].noise_counter_bit4 =
            (t->aud[ch].noise_counter & 0x01) != 0;

        /* pulse_counter_hold is driven by AUDC[1:0]. */
        switch (audc & 0x03) {
        case 0x00:
        case 0x01:
            t->aud[ch].pulse_counter_hold = false;
            break;
        case 0x02:
            t->aud[ch].pulse_counter_hold =
                (t->aud[ch].noise_counter & 0x1E) != 0x02;
            break;
        case 0x03:
            t->aud[ch].pulse_counter_hold = !t->aud[ch].noise_counter_bit4;
            break;
        default: break;
        }

        /* Noise (poly5) feedback; AUDC[1:0]==00 chains C8 from pulse_counter. */
        if ((audc & 0x03) == 0) {
            int xor_bit = ((t->aud[ch].pulse_counter ^
                            t->aud[ch].noise_counter) & 0x01) != 0;
            int jump_out = !(t->aud[ch].noise_counter ||
                             (t->aud[ch].pulse_counter != 0x0A));
            int flood = (audc & 0x0C) == 0;
            t->aud[ch].noise_feedback = xor_bit || jump_out || flood;
        } else {
            int tap = (((t->aud[ch].noise_counter & 0x04) ? 1 : 0) ^
                       (t->aud[ch].noise_counter & 0x01)) != 0;
            int jump_out = (t->aud[ch].noise_counter == 0);
            t->aud[ch].noise_feedback = tap || jump_out;
        }
    }

    /* Update divider; latch clockEnable for next phase1. */
    t->aud[ch].clock_enable = (t->aud[ch].div_counter == t->aud[ch].audf);
    if (t->aud[ch].div_counter == t->aud[ch].audf ||
        t->aud[ch].div_counter == 0x1F) {
        t->aud[ch].div_counter = 0;
    } else {
        t->aud[ch].div_counter++;
    }
}

static void audio_phase1(struct tia *t, int ch)
{
    if (!t->aud[ch].clock_enable) return;

    {
        uint8_t audc = t->aud[ch].audc;
        int pulse_fb = 0;
        switch ((audc >> 2) & 0x03) {
        case 0x00:
            pulse_fb =
                ((((t->aud[ch].pulse_counter & 0x02) ? 1 : 0) ^
                   (t->aud[ch].pulse_counter & 0x01)) &&
                 (t->aud[ch].pulse_counter != 0x0A) &&
                 (audc & 0x03));
            break;
        case 0x01:
            pulse_fb = !(t->aud[ch].pulse_counter & 0x08);
            break;
        case 0x02:
            pulse_fb = !t->aud[ch].noise_counter_bit4;
            break;
        case 0x03:
            pulse_fb = !((t->aud[ch].pulse_counter & 0x02) ||
                         !(t->aud[ch].pulse_counter & 0x0E));
            break;
        default: break;
        }

        /* Shift noise right; inject feedback at bit 4. */
        t->aud[ch].noise_counter >>= 1;
        if (t->aud[ch].noise_feedback) t->aud[ch].noise_counter |= 0x10;

        /* Shift-and-invert pulse counter; inject feedback at bit 3. */
        if (!t->aud[ch].pulse_counter_hold) {
            t->aud[ch].pulse_counter =
                (uint8_t)(~(t->aud[ch].pulse_counter >> 1) & 0x07);
            if (pulse_fb) t->aud[ch].pulse_counter |= 0x08;
        }
    }
}

static void audio_emit_sample(struct tia *t)
{
    uint8_t s0, s1;
    uint32_t ct = t->audio_sum_ct;
    if (ct == 0) ct = 1;
    s0 = (uint8_t)(t->audio_sum[0] / ct);
    s1 = (uint8_t)(t->audio_sum[1] / ct);
    t->audio_sum[0] = t->audio_sum[1] = 0;
    t->audio_sum_ct = 0;
    if (t->audio_buf_len < sizeof(t->audio_buf) / sizeof(t->audio_buf[0]))
        t->audio_buf[t->audio_buf_len++] = t->audio_mix[s0 + s1];
}

/* Decode HM register's 4-bit signed motion value in the high nibble.
 * Positive values move the object LEFT (subtract from position);
 * negative values move it RIGHT. */
static int hm_decode(uint8_t hm)
{
    int v = (hm >> 4) & 0x0F;
    if (v & 0x08) v -= 16;
    return v;
}

/* Missile size (color clocks) from NUSIZ bits 4-5. */
static int missile_size(uint8_t nusiz)
{
    return 1 << ((nusiz >> 4) & 0x03);
}

/* Ball size (color clocks) from CTRLPF bits 4-5. */
static int ball_size(uint8_t ctrlpf)
{
    return 1 << ((ctrlpf >> 4) & 0x03);
}

/* Does a missile (any NUSIZ copy) cover visible-x `x`? */
static int missile_pixel_on(int x, int start, uint8_t nusiz, bool enam)
{
    int copies = nusiz_copies(nusiz);
    int spacing = nusiz_copy_spacing(nusiz);
    int size = missile_size(nusiz);
    int c;
    if (!enam) return 0;
    for (c = 0; c < copies; c++) {
        int cs = start + c * spacing;
        if (x >= cs && x < cs + size) return 1;
    }
    return 0;
}

/* Does the ball cover visible-x `x`? Single copy. */
static int ball_pixel_on(int x, int start, uint8_t ctrlpf, bool enabl)
{
    int size = ball_size(ctrlpf);
    if (!enabl) return 0;
    if (x >= start && x < start + size) return 1;
    return 0;
}

/* Does this player draw a set pixel at visible-x `x`? */
static int player_pixel_on(uint8_t gfx, int x, int start,
                           uint8_t nusiz, bool refp)
{
    int copies = nusiz_copies(nusiz);
    int width  = nusiz_pixel_width(nusiz);
    int spacing = nusiz_copy_spacing(nusiz);
    int c;
    int sprite_px = 8 * width;
    if (gfx == 0) return 0;
    for (c = 0; c < copies; c++) {
        int copy_start = start + c * spacing;
        if (x >= copy_start && x < copy_start + sprite_px) {
            int bit_idx = (x - copy_start) / width;
            if (refp) bit_idx = 7 - bit_idx;
            if ((gfx >> (7 - bit_idx)) & 1) return 1;
        }
    }
    return 0;
}

/* Update the 15 collision bits from the set of ON flags for this pixel. */
static void update_collisions(struct tia *t, int p0, int p1, int m0, int m1,
                              int bl, int pf)
{
    if (m0 && p1) t->cx[0] |= 0x80;   /* CXM0P:  M0-P1 */
    if (m0 && p0) t->cx[0] |= 0x40;   /* CXM0P:  M0-P0 */
    if (m1 && p0) t->cx[1] |= 0x80;   /* CXM1P:  M1-P0 */
    if (m1 && p1) t->cx[1] |= 0x40;   /* CXM1P:  M1-P1 */
    if (p0 && pf) t->cx[2] |= 0x80;   /* CXP0FB: P0-PF */
    if (p0 && bl) t->cx[2] |= 0x40;   /* CXP0FB: P0-BL */
    if (p1 && pf) t->cx[3] |= 0x80;   /* CXP1FB: P1-PF */
    if (p1 && bl) t->cx[3] |= 0x40;   /* CXP1FB: P1-BL */
    if (m0 && pf) t->cx[4] |= 0x80;   /* CXM0FB: M0-PF */
    if (m0 && bl) t->cx[4] |= 0x40;   /* CXM0FB: M0-BL */
    if (m1 && pf) t->cx[5] |= 0x80;   /* CXM1FB: M1-PF */
    if (m1 && bl) t->cx[5] |= 0x40;   /* CXM1FB: M1-BL */
    if (bl && pf) t->cx[6] |= 0x80;   /* CXBLPF: BL-PF */
    if (p0 && p1) t->cx[7] |= 0x80;   /* CXPPMM: P0-P1 */
    if (m0 && m1) t->cx[7] |= 0x40;   /* CXPPMM: M0-M1 */
}

/* Resolve the pixel color for visible-x `x`, and update collision bits.
 * Priority: P0/M0 > P1/M1 > PF/BL > BG by default;
 * CTRLPF.PFP (bit 2) swaps: PF/BL become highest priority. */
static uint8_t pixel_color(struct tia *t, uint16_t x)
{
    uint16_t pf_pixel = (uint16_t)(x >> 2);
    int left_half = pf_pixel < 20;
    uint16_t bit;
    int pf_on;
    uint8_t g0, g1;
    int p0_on, p1_on, m0_on, m1_on, bl_on;
    bool eff_enabl;

    if (left_half)                   bit = pf_pixel;
    else if (t->ctrlpf & 0x01)       bit = (uint16_t)(39 - pf_pixel);
    else                             bit = (uint16_t)(pf_pixel - 20);
    pf_on = pf_bit_on(t, bit);

    g0 = t->vdelp0 ? t->grp0_latch : t->grp0;
    g1 = t->vdelp1 ? t->grp1_latch : t->grp1;
    p0_on = player_pixel_on(g0, (int)x, (int)t->p0_pos, t->nusiz0, t->refp0);
    p1_on = player_pixel_on(g1, (int)x, (int)t->p1_pos, t->nusiz1, t->refp1);

    m0_on = !t->resmp0 &&
            missile_pixel_on((int)x, (int)t->m0_pos, t->nusiz0, t->enam0);
    m1_on = !t->resmp1 &&
            missile_pixel_on((int)x, (int)t->m1_pos, t->nusiz1, t->enam1);

    eff_enabl = t->vdelbl ? t->enabl_latch : t->enabl;
    bl_on = ball_pixel_on((int)x, (int)t->bl_pos, t->ctrlpf, eff_enabl);

    update_collisions(t, p0_on, p1_on, m0_on, m1_on, bl_on, pf_on);

    if (t->ctrlpf & 0x04) {
        /* PFP: PF/BL become highest priority. */
        if (pf_on || bl_on) {
            if ((t->ctrlpf & 0x02) && pf_on && !bl_on)
                return left_half ? t->colup0 : t->colup1;
            return t->colupf;
        }
        if (p0_on || m0_on) return t->colup0;
        if (p1_on || m1_on) return t->colup1;
        return t->colubk;
    }
    if (p0_on || m0_on) return t->colup0;
    if (p1_on || m1_on) return t->colup1;
    if (pf_on || bl_on) {
        if ((t->ctrlpf & 0x02) && pf_on && !bl_on)
            return left_half ? t->colup0 : t->colup1;
        return t->colupf;
    }
    return t->colubk;
}

/* Forward decl — defined in the write section below. */
static void delay_tick(struct tia *t);

void tia_tick(struct tia *t)
{
    /* Fire any register writes whose pipeline delay is now up. Happens
     * at the top of the tick so the new values are visible to this
     * tick's pixel-render code. */
    delay_tick(t);

    /* Cycle-accurate HMOVE state machine.  On each clock within the
     * motion window, emit at most one "extra" pixel-counter pulse per
     * object whose pending count is non-zero, stepping it toward 0.
     * A mid-window HMx write (handled in delay_apply) replaces the
     * pending count with the new decoded motion — Cosmic Ark strobes
     * HMOVE, then rewrites HMM0 partway through the window to inject
     * fresh motion pulses on the missile's pixel counter, which
     * produces the starfield.  Window is 24 CPU cycles = 72 colour
     * clocks, matching Andrew Towers' TIA Hardware Notes. */
    if (t->hmove_active) {
        if      (t->hm_remaining[0] > 0) { t->p0_pos--; t->hm_remaining[0]--; }
        else if (t->hm_remaining[0] < 0) { t->p0_pos++; t->hm_remaining[0]++; }
        if      (t->hm_remaining[1] > 0) { t->p1_pos--; t->hm_remaining[1]--; }
        else if (t->hm_remaining[1] < 0) { t->p1_pos++; t->hm_remaining[1]++; }
        if      (t->hm_remaining[2] > 0) { t->m0_pos--; t->hm_remaining[2]--; }
        else if (t->hm_remaining[2] < 0) { t->m0_pos++; t->hm_remaining[2]++; }
        if      (t->hm_remaining[3] > 0) { t->m1_pos--; t->hm_remaining[3]--; }
        else if (t->hm_remaining[3] < 0) { t->m1_pos++; t->hm_remaining[3]++; }
        if      (t->hm_remaining[4] > 0) { t->bl_pos--; t->hm_remaining[4]--; }
        else if (t->hm_remaining[4] < 0) { t->bl_pos++; t->hm_remaining[4]++; }
        if (++t->hmove_ticks >= 24) t->hmove_active = false;
    }
    /* Audio runs every color clock, agnostic of HBLANK/VBLANK/VSYNC. */
    t->audio_sum[0] += audio_actual_volume(t, 0);
    t->audio_sum[1] += audio_actual_volume(t, 1);
    t->audio_sum_ct++;
    switch (t->hpos) {
    case 9: case 81:
        audio_phase0(t, 0);
        audio_phase0(t, 1);
        break;
    case 37: case 149:
        audio_phase1(t, 0);
        audio_phase1(t, 1);
        audio_emit_sample(t);
        break;
    default: break;
    }

    /* RESMP: continuously lock missile position to the paired player's
     * center (player_pos + 4). Hides the missile via the pixel_color path. */
    if (t->resmp0) t->m0_pos = (int16_t)((int)t->p0_pos + 4);
    if (t->resmp1) t->m1_pos = (int16_t)((int)t->p1_pos + 4);

    /* Paddle capacitor charge. While VBLANK bit 7 is set (inpt_ground),
     * the caps are shorted to ground, so bit 7 of INPT0-3 stays low and
     * the countdown resets to the target charge time. Once VBLANK bit 7
     * clears, each cap charges through its paddle's pot resistance; when
     * the count hits zero, the comparator flips the corresponding INPT's
     * bit 7 high. */
    {
        int i;
        for (i = 0; i < 4; i++) {
            if (t->inpt_ground) {
                t->inpt[i] = (uint8_t)(t->inpt[i] & 0x7F);
                t->paddle_charge_cnt[i] = t->paddle_charge_max[i];
            } else if (t->paddle_charge_cnt[i] > 0) {
                t->paddle_charge_cnt[i]--;
                if (t->paddle_charge_cnt[i] == 0)
                    t->inpt[i] = (uint8_t)(t->inpt[i] | 0x80);
            }
        }
    }

    /* HMOVE comb: 8 visible color clocks of extended blanking after an
     * HMOVE strobe. Hold the counter through HBLANK so an HMOVE strobed
     * mid-HBLANK still blanks the first 8 visible pixels; drain it on
     * every post-HBLANK clock (visible OR VBLANK) so an HMOVE strobed
     * during VBLANK expires off-screen instead of bleeding into the first
     * visible scanline (Adventure's yellow castle top row exhibited this
     * as an 8-pixel black stub). */
    t->last_colulum = 0;
    if (t->hpos >= TIA_HBLANK_CLOCKS &&
        !t->vsync &&
        t->scanline < TIA_MAX_SCANLINES) {
        uint16_t x = (uint16_t)(t->hpos - TIA_HBLANK_CLOCKS);
        uint16_t y = t->scanline;
        uint32_t out;
        if (t->vblank) {
            /* VBLANK active — beam is visible to the CRT (video signal is
             * produced) but the picture is forced black. Write black to fb
             * so the libretro layer's 228-row output has clean black
             * padding at top/bottom instead of stale content from the
             * previous frame leaking through. */
            out = t->palette[0];
        } else if (t->hmove_blank > 0) {
            out = t->palette[0];          /* HMOVE comb: black */
        } else {
            uint8_t ci = (uint8_t)((pixel_color(t, x) >> 1) & 0x7F);
            out = t->palette[ci];
            t->last_colulum = ci;
        }
        t->fb[y * TIA_VISIBLE_WIDTH + x] = out;
    }
    if (t->hpos >= TIA_HBLANK_CLOCKS && t->hmove_blank > 0)
        t->hmove_blank--;

    /* Advance beam. HSYNC at wrap. */
    t->hpos++;
    if (t->hpos >= TIA_SCANLINE_CLOCKS) {
        t->hpos = 0;
        if (!t->vsync && t->scanline + 1 < TIA_MAX_SCANLINES)
            t->scanline++;
        /* WSYNC releases at the start of a new scanline. */
        t->rdy_asserted = false;
    }
}

/* ============================================================
 *   Register access
 * ============================================================ */

uint8_t tia_read(struct tia *t, uint16_t addr)
{
    /* A3:A0 selects the read register. $00-$07: collisions (bits 7-6 only);
     * $08-$0D: INPT0-5 (bit 7 only); $0E-$3F: undefined.
     *
     * For the "undefined" bits, real hardware leaves the data bus floating,
     * so the CPU sees whatever was last driven on it — for zero-page reads
     * that's the low byte of the address. Games like Haunted House do
     * `SBC $0F` (a TIA write-only register) and rely on this floating bus
     * behaviour to make the SBC actually decrement the accumulator. Returning
     * 0 deadlocks them in their boot loop. */
    uint8_t sel  = (uint8_t)(addr & 0x0F);
    uint8_t lobs = (uint8_t)(addr & 0x3F);   /* address low used as bus filler */
    if (sel < 8)
        return (uint8_t)((t->cx[sel] & 0xC0) | lobs);
    if (sel < 0x0E) {
        /* inpt_ground (VBLANK bit 7, the paddle DUMP line) only grounds
         * the paddle capacitors (INPT0-3). INPT4/5 are the digital trigger
         * pins and aren't affected by bit 7 — their latching is controlled
         * by VBLANK bit 6, handled in tia_tick. */
        uint8_t v = t->inpt[sel - 8];
        return (uint8_t)((v & 0x80) | (lobs & 0x7F));
    }
    return (uint8_t)(addr & 0xFF);
}

/* ============================================================
 *   Delay queue — deferred register writes
 *
 * Real TIA latches every register write through a colour-clock pipeline
 * before the new value reaches the renderer. We model this with an
 * 8-slot ring buffer: a write with a delay of N is placed in the slot
 * that will be drained N ticks from now. `delay_tick` runs at the top
 * of every tia_tick, drains the current slot, then advances the head.
 * ============================================================ */

#define DELAY_SLOTS 8

static void delay_push(struct tia *t, uint8_t reg, uint8_t val, int delay)
{
    /* `delay_head` points to the slot that will be drained on the NEXT
     * tia_tick (delay_tick runs at tick start and then advances head),
     * so an entry already sits one tick in the future by virtue of being
     * in the queue. delay-1 gives the effective colour-clock count:
     *   delay=1 → fires on the next tick
     *   delay=2 → fires two ticks later, etc. */
    int slot = (t->delay_head + delay - 1) & (DELAY_SLOTS - 1);
    int i;
    for (i = 0; i < 8; i++) {
        if (t->delay_slots[slot][i].reg == reg) {
            /* Same-slot overwrite: game stored the register again before
             * the previous write fired. Real HW collapses these. */
            t->delay_slots[slot][i].value = val;
            return;
        }
        if (t->delay_slots[slot][i].reg == 0xFF) {
            t->delay_slots[slot][i].reg = reg;
            t->delay_slots[slot][i].value = val;
            return;
        }
    }
    /* All 8 entries for this slot used. Should not happen with real
     * kernels; silently drop so we don't overrun the array. */
}

/* Commit a queued write at its firing time. Mirrors the immediate-apply
 * branches in tia_write, including side effects (GRP0 write also shuffles
 * GRP1 into the VDELP1 latch). */
static void delay_apply(struct tia *t, uint8_t reg, uint8_t data)
{
    switch (reg) {
    case 0x09: t->colubk = data; break;
    case 0x0B: t->refp0 = (data & 0x08) != 0; break;
    case 0x0C: t->refp1 = (data & 0x08) != 0; break;
    case 0x0D: t->pf0 = data; break;
    case 0x0E: t->pf1 = data; break;
    case 0x0F: t->pf2 = data; break;
    case 0x1B: /* GRP0 — also latches GRP1 for VDELP1 */
        t->grp0 = data;
        t->grp1_latch = t->grp1;
        break;
    case 0x1C: /* GRP1 — also latches GRP0 and ENABL */
        t->grp1 = data;
        t->grp0_latch = t->grp0;
        t->enabl_latch = t->enabl;
        break;
    case 0x1D: t->enam0 = (data & 0x02) != 0; break;
    case 0x1E: t->enam1 = (data & 0x02) != 0; break;
    case 0x1F: t->enabl = (data & 0x02) != 0; break;
    /* HMx writes: always land in the register.  If HMOVE is currently
     * mid-window, the new value ALSO replaces the pending pulse count,
     * so additional pulses fire through the rest of the window based
     * on the new motion — Cosmic Ark's starfield trick. */
    case 0x20: t->hmp0 = data;
               if (t->hmove_active) t->hm_remaining[0] = (int8_t)hm_decode(data);
               break;
    case 0x21: t->hmp1 = data;
               if (t->hmove_active) t->hm_remaining[1] = (int8_t)hm_decode(data);
               break;
    case 0x22: t->hmm0 = data;
               if (t->hmove_active) t->hm_remaining[2] = (int8_t)hm_decode(data);
               break;
    case 0x23: t->hmm1 = data;
               if (t->hmove_active) t->hm_remaining[3] = (int8_t)hm_decode(data);
               break;
    case 0x24: t->hmbl = data;
               if (t->hmove_active) t->hm_remaining[4] = (int8_t)hm_decode(data);
               break;
    case 0x2A: /* HMOVE — start the cycle-accurate motion window */
        t->hm_remaining[0] = (int8_t)hm_decode(t->hmp0);
        t->hm_remaining[1] = (int8_t)hm_decode(t->hmp1);
        t->hm_remaining[2] = (int8_t)hm_decode(t->hmm0);
        t->hm_remaining[3] = (int8_t)hm_decode(t->hmm1);
        t->hm_remaining[4] = (int8_t)hm_decode(t->hmbl);
        t->hmove_active    = true;
        t->hmove_ticks     = 0;
        t->hmove_blank     = 8;     /* visible 8-clock comb */
        break;
    case 0x2B: /* HMCLR — zero all HM registers AND any pulses in flight */
        t->hmp0 = t->hmp1 = t->hmm0 = t->hmm1 = t->hmbl = 0;
        t->hm_remaining[0] = t->hm_remaining[1] = t->hm_remaining[2] =
            t->hm_remaining[3] = t->hm_remaining[4] = 0;
        break;
    default: break;
    }
    (void)data;
}

static void delay_tick(struct tia *t)
{
    uint8_t head = t->delay_head;
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t reg = t->delay_slots[head][i].reg;
        if (reg == 0xFF) continue;
        delay_apply(t, reg, t->delay_slots[head][i].value);
        t->delay_slots[head][i].reg = 0xFF;
    }
    t->delay_head = (uint8_t)((head + 1) & (DELAY_SLOTS - 1));
}

void tia_write(struct tia *t, uint16_t addr, uint8_t data)
{
    switch (addr & 0x3F) {
    case 0x00: { /* VSYNC: bit 1 enables vertical sync pulse */
        bool new_vsync = (data & 0x02) != 0;
        if (new_vsync && !t->vsync) {
            /* Rising edge: previous frame complete. Promote pending to
             * libretro-visible fields, reset pending trackers. fb is NOT
             * cleared between frames — stale content from the previous frame
             * naturally fills any scanlines this frame didn't render (matches
             * CRT persistence and avoids flashing "missing line" artifacts
             * when the game's per-frame scanline count drifts by one). */
            t->visible_start = t->_pending_vstart;
            t->visible_end   = t->_pending_vend;
            t->_pending_vstart = 0xFFFF;
            t->_pending_vend   = 0xFFFF;
            /* Auto-detect: sample the just-finished frame's scanline count
             * starting from frame 1 (frame 0 often has partial/garbage
             * VSYNC while RAM initialises). After TIA_DETECT_FRAMES usable
             * samples, classify as NTSC (~262) or PAL (~312). The gap is
             * ~50 lines, far larger than any jitter, so a single pass of
             * "count votes >275" suffices. */
            if (!t->detect_locked && t->frame_number >= 1 &&
                t->detect_count < TIA_DETECT_FRAMES) {
                t->detect_samples[t->detect_count++] = t->scanline;
                if (t->detect_count == TIA_DETECT_FRAMES) {
                    int i, pal_votes = 0;
                    for (i = 0; i < TIA_DETECT_FRAMES; i++)
                        if (t->detect_samples[i] > 275) pal_votes++;
                    t->detected_region = (pal_votes * 2 > TIA_DETECT_FRAMES)
                                         ? TIA_REGION_PAL : TIA_REGION_NTSC;
                    t->detect_locked = true;
                }
            }
            t->frame_number++;
            t->frame_ready = true;
            t->scanline = 0;
        }
        t->vsync = new_vsync;
        break;
    }
    case 0x01: { /* VBLANK: bit 1 enables display blanking; bit 7 grounds INPT4/5 */
        bool new_vblank = (data & 0x02) != 0;
        if (!new_vblank && t->vblank && t->_pending_vstart == 0xFFFF)
            t->_pending_vstart = t->scanline;
        else if (new_vblank && !t->vblank && t->_pending_vstart != 0xFFFF &&
                 t->_pending_vend == 0xFFFF)
            t->_pending_vend = t->scanline;
        t->vblank       = new_vblank;
        t->inpt_ground  = (data & 0x80) != 0;
        break;
    }
    case 0x02: /* WSYNC strobe. Assert RDY until next HSYNC (scanline wrap).
                * If the write lands at hpos=0 (the scanline just wrapped
                * during this cycle's 3 TIA ticks), the WSYNC "missed" —
                * the beam is already at HSYNC, so no stall is needed. */
        if (t->hpos != 0)
            t->rdy_asserted = true;
        break;
    case 0x03: /* RSYNC strobe — aligns horizontal counter to HSYNC. Sets
                 hctr 3 clocks before wrap, shortening the current scanline
                 to finish 3 clocks from now (not zeroing it, which would
                 *extend* the scanline by the remaining distance to wrap). */
        t->hpos = (uint16_t)(TIA_SCANLINE_CLOCKS - 3);
        break;
    case 0x04: t->nusiz0 = data; break;
    case 0x05: t->nusiz1 = data; break;
    case 0x06: t->colup0 = data; break;
    case 0x07: t->colup1 = data; break;
    case 0x08: t->colupf = data; break;
    case 0x09: delay_push(t, 0x09, data, 2); break; /* COLUBK — 1 color-clock DAC pipeline delay (oracle vs csim) */
    case 0x0A: t->ctrlpf = data; break;
    case 0x0B: delay_push(t, 0x0B, data, 1); break; /* REFP0 — 1 clk */
    case 0x0C: delay_push(t, 0x0C, data, 1); break; /* REFP1 — 1 clk */
    case 0x0D: delay_push(t, 0x0D, data, 2); break; /* PF0 — 2 clk */
    case 0x0E: delay_push(t, 0x0E, data, 2); break; /* PF1 — 2 clk */
    case 0x0F: delay_push(t, 0x0F, data, 2); break; /* PF2 — 2 clk */
    case 0x10: /* RESP0 strobe */
        t->p0_pos = (int16_t)((int)t->hpos + 5 - TIA_HBLANK_CLOCKS);
        break;
    case 0x11: /* RESP1 strobe */
        t->p1_pos = (int16_t)((int)t->hpos + 5 - TIA_HBLANK_CLOCKS);
        break;
    case 0x12: /* RESM0 strobe */
        t->m0_pos = (int16_t)((int)t->hpos + 4 - TIA_HBLANK_CLOCKS);
        break;
    case 0x13: /* RESM1 strobe */
        t->m1_pos = (int16_t)((int)t->hpos + 4 - TIA_HBLANK_CLOCKS);
        break;
    case 0x14: /* RESBL strobe */
        t->bl_pos = (int16_t)((int)t->hpos + 4 - TIA_HBLANK_CLOCKS);
        break;
    case 0x1B: delay_push(t, 0x1B, data, 1); break; /* GRP0  — 1 clk (latches grp1) */
    case 0x1C: delay_push(t, 0x1C, data, 1); break; /* GRP1  — 1 clk (latches grp0 + enabl) */
    case 0x1D: delay_push(t, 0x1D, data, 1); break; /* ENAM0 — 1 clk */
    case 0x1E: delay_push(t, 0x1E, data, 1); break; /* ENAM1 — 1 clk */
    case 0x1F: delay_push(t, 0x1F, data, 1); break; /* ENABL — 1 clk */
    case 0x20: delay_push(t, 0x20, data, 2); break; /* HMP0  — 2 clk */
    case 0x21: delay_push(t, 0x21, data, 2); break; /* HMP1  — 2 clk */
    case 0x22: delay_push(t, 0x22, data, 2); break; /* HMM0  — 2 clk */
    case 0x23: delay_push(t, 0x23, data, 2); break; /* HMM1  — 2 clk */
    case 0x24: delay_push(t, 0x24, data, 2); break; /* HMBL  — 2 clk */
    case 0x25: t->vdelp0 = (data & 0x01) != 0; break;
    case 0x26: t->vdelp1 = (data & 0x01) != 0; break;
    case 0x27: t->vdelbl = (data & 0x01) != 0; break;
    case 0x28: t->resmp0 = (data & 0x02) != 0; break;
    case 0x29: t->resmp1 = (data & 0x02) != 0; break;
    case 0x15: t->aud[0].audc = data & 0x0F; break;
    case 0x16: t->aud[1].audc = data & 0x0F; break;
    case 0x17: t->aud[0].audf = data & 0x1F; break;
    case 0x18: t->aud[1].audf = data & 0x1F; break;
    case 0x19: t->aud[0].audv = data & 0x0F; break;
    case 0x1A: t->aud[1].audv = data & 0x0F; break;
    case 0x2A: delay_push(t, 0x2A, data, 6); break; /* HMOVE — 6 clk */
    case 0x2B: delay_push(t, 0x2B, data, 2); break; /* HMCLR — 2 clk */
    case 0x2C: /* CXCLR strobe: clear all collision bits */
        memset(t->cx, 0, sizeof(t->cx));
        break;
    default:
        /* Unhandled in M3a — later sub-milestones will add cases. */
        break;
    }
}

/* ============================================================
 *   Serialization — framebuffer is NOT serialized (transient).
 * ============================================================ */

size_t tia_serialize_size(void)
{
    /* M3a/b(20) + player(13) + m/b(14) + hmove(1) + cx(8) + audio(26) +
     * input(7) + paddle_state(16: 4*2 max + 4*2 cnt) = 105
     * + delay queue: 8 slots × 8 entries × 2 bytes + 1 head byte = 129.
     * + cycle-accurate hmove tail: 5 pending + active flag + ticks = 7. */
    return 20 + 13 + 14 + 1 + 8 + 26 + 7 + 16 + 129 + 7;
}

void tia_serialize(const struct tia *t, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    p[0]  = (uint8_t)(t->hpos & 0xFF);
    p[1]  = (uint8_t)(t->hpos >> 8);
    p[2]  = (uint8_t)(t->scanline & 0xFF);
    p[3]  = (uint8_t)(t->scanline >> 8);
    p[4]  = (uint8_t)(t->frame_number & 0xFF);
    p[5]  = (uint8_t)(t->frame_number >> 8);
    p[6]  = (uint8_t)(t->frame_number >> 16);
    p[7]  = (uint8_t)(t->frame_number >> 24);
    p[8]  = (uint8_t)(t->frame_ready  ? 1 : 0);
    p[9]  = (uint8_t)(t->vsync        ? 1 : 0);
    p[10] = (uint8_t)(t->vblank       ? 1 : 0);
    p[11] = (uint8_t)(t->rdy_asserted ? 1 : 0);
    p[12] = t->colubk;
    p[13] = t->colupf;
    p[14] = t->colup0;
    p[15] = t->colup1;
    p[16] = t->pf0;
    p[17] = t->pf1;
    p[18] = t->pf2;
    p[19] = t->ctrlpf;
    p[20] = t->grp0;
    p[21] = t->grp1;
    p[22] = t->grp0_latch;
    p[23] = t->grp1_latch;
    p[24] = t->nusiz0;
    p[25] = t->nusiz1;
    p[26] = (uint8_t)((t->refp0 ? 1 : 0) | (t->refp1 ? 2 : 0) |
                      (t->vdelp0 ? 4 : 0) | (t->vdelp1 ? 8 : 0));
    p[27] = (uint8_t)(t->p0_pos & 0xFF);
    p[28] = (uint8_t)((uint16_t)t->p0_pos >> 8);
    p[29] = (uint8_t)(t->p1_pos & 0xFF);
    p[30] = (uint8_t)((uint16_t)t->p1_pos >> 8);
    p[31] = t->hmp0;
    p[32] = t->hmp1;
    p[33] = (uint8_t)((t->enam0 ? 1 : 0) | (t->enam1 ? 2 : 0) |
                      (t->enabl ? 4 : 0) | (t->enabl_latch ? 8 : 0) |
                      (t->vdelbl ? 16 : 0) |
                      (t->resmp0 ? 32 : 0) | (t->resmp1 ? 64 : 0));
    p[34] = (uint8_t)(t->m0_pos & 0xFF);
    p[35] = (uint8_t)((uint16_t)t->m0_pos >> 8);
    p[36] = (uint8_t)(t->m1_pos & 0xFF);
    p[37] = (uint8_t)((uint16_t)t->m1_pos >> 8);
    p[38] = (uint8_t)(t->bl_pos & 0xFF);
    p[39] = (uint8_t)((uint16_t)t->bl_pos >> 8);
    p[40] = t->hmm0;
    p[41] = t->hmm1;
    p[42] = t->hmbl;
    p[43] = t->hmove_blank;
    memcpy(p + 44, t->cx, 8);
    {
        int i;
        uint8_t *q = p + 52;
        for (i = 0; i < 2; i++) {
            *q++ = t->aud[i].audc;
            *q++ = t->aud[i].audf;
            *q++ = t->aud[i].audv;
            *q++ = t->aud[i].div_counter;
            *q++ = t->aud[i].pulse_counter;
            *q++ = t->aud[i].noise_counter;
            *q++ = (uint8_t)((t->aud[i].clock_enable ? 1 : 0) |
                             (t->aud[i].noise_feedback ? 2 : 0) |
                             (t->aud[i].noise_counter_bit4 ? 4 : 0) |
                             (t->aud[i].pulse_counter_hold ? 8 : 0));
        }
        /* audio_sum (little-endian) + count */
        *q++ = (uint8_t)(t->audio_sum[0]);
        *q++ = (uint8_t)(t->audio_sum[0] >> 8);
        *q++ = (uint8_t)(t->audio_sum[1]);
        *q++ = (uint8_t)(t->audio_sum[1] >> 8);
        *q++ = (uint8_t)(t->audio_sum_ct);
        *q++ = (uint8_t)(t->audio_sum_ct >> 8);
        /* Sample queue is transient; don't serialize. */
        /* 26 bytes total: 7*2 + 6 + 6 tail = 20 used, 3 region/palette, 3 reserved.
         * Region and palette variant survive save/load; detect_samples/count do
         * not (if the save pre-dates the detect lock, detection re-runs over
         * the next ~5 VSYNCs). Older save files have these bytes as 0, which
         * maps to NTSC / standard-palette / unlocked — all safe defaults. */
        *q++ = (uint8_t)t->region;
        *q++ = (uint8_t)(((uint8_t)t->detected_region & 0x03) |
                         (t->detect_locked ? 0x80 : 0));
        *q++ = (uint8_t)t->palette_variant;
        *q++ = 0; *q++ = 0; *q++ = 0;
        /* Input (7 bytes): inpt[0..5] + ground flag. */
        {
            int i;
            for (i = 0; i < 6; i++) *q++ = t->inpt[i];
            *q++ = (uint8_t)(t->inpt_ground ? 1 : 0);
            /* Paddle state: 4 × (max lo/hi) then 4 × (cnt lo/hi). */
            for (i = 0; i < 4; i++) {
                *q++ = (uint8_t)(t->paddle_charge_max[i] & 0xFF);
                *q++ = (uint8_t)(t->paddle_charge_max[i] >> 8);
            }
            for (i = 0; i < 4; i++) {
                *q++ = (uint8_t)(t->paddle_charge_cnt[i] & 0xFF);
                *q++ = (uint8_t)(t->paddle_charge_cnt[i] >> 8);
            }
            /* Delay queue: 8 slots × 8 entries × (reg, value) + head. */
            {
                int s, e;
                for (s = 0; s < 8; s++)
                    for (e = 0; e < 8; e++) {
                        *q++ = t->delay_slots[s][e].reg;
                        *q++ = t->delay_slots[s][e].value;
                    }
                *q++ = t->delay_head;
            }
            /* Cycle-accurate HMOVE state (7 bytes): 5× signed pending
             * pulses + active flag + ticks elapsed. */
            for (i = 0; i < 5; i++) *q++ = (uint8_t)t->hm_remaining[i];
            *q++ = (uint8_t)(t->hmove_active ? 1 : 0);
            *q++ = t->hmove_ticks;
        }
    }
}

bool tia_deserialize(struct tia *t, const void *buf, size_t size)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint8_t flags;
    if (size < tia_serialize_size()) return false;
    t->hpos         = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    t->scanline     = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
    t->frame_number =  (uint32_t)p[4]
                    | ((uint32_t)p[5] << 8)
                    | ((uint32_t)p[6] << 16)
                    | ((uint32_t)p[7] << 24);
    t->frame_ready  = p[8]  != 0;
    t->vsync        = p[9]  != 0;
    t->vblank       = p[10] != 0;
    t->rdy_asserted = p[11] != 0;
    t->colubk = p[12];
    t->colupf = p[13];
    t->colup0 = p[14];
    t->colup1 = p[15];
    t->pf0    = p[16];
    t->pf1    = p[17];
    t->pf2    = p[18];
    t->ctrlpf = p[19];
    t->grp0       = p[20];
    t->grp1       = p[21];
    t->grp0_latch = p[22];
    t->grp1_latch = p[23];
    t->nusiz0     = p[24];
    t->nusiz1     = p[25];
    flags = p[26];
    t->refp0  = (flags & 0x01) != 0;
    t->refp1  = (flags & 0x02) != 0;
    t->vdelp0 = (flags & 0x04) != 0;
    t->vdelp1 = (flags & 0x08) != 0;
    t->p0_pos = (int16_t)(p[27] | ((uint16_t)p[28] << 8));
    t->p1_pos = (int16_t)(p[29] | ((uint16_t)p[30] << 8));
    t->hmp0   = p[31];
    t->hmp1   = p[32];
    flags = p[33];
    t->enam0       = (flags & 0x01) != 0;
    t->enam1       = (flags & 0x02) != 0;
    t->enabl       = (flags & 0x04) != 0;
    t->enabl_latch = (flags & 0x08) != 0;
    t->vdelbl      = (flags & 0x10) != 0;
    t->resmp0      = (flags & 0x20) != 0;
    t->resmp1      = (flags & 0x40) != 0;
    t->m0_pos = (int16_t)(p[34] | ((uint16_t)p[35] << 8));
    t->m1_pos = (int16_t)(p[36] | ((uint16_t)p[37] << 8));
    t->bl_pos = (int16_t)(p[38] | ((uint16_t)p[39] << 8));
    t->hmm0 = p[40];
    t->hmm1 = p[41];
    t->hmbl = p[42];
    t->hmove_blank = p[43];
    memcpy(t->cx, p + 44, 8);
    {
        int i;
        const uint8_t *q = p + 52;
        for (i = 0; i < 2; i++) {
            uint8_t f;
            t->aud[i].audc = *q++;
            t->aud[i].audf = *q++;
            t->aud[i].audv = *q++;
            t->aud[i].div_counter = *q++;
            t->aud[i].pulse_counter = *q++;
            t->aud[i].noise_counter = *q++;
            f = *q++;
            t->aud[i].clock_enable = (f & 1) != 0;
            t->aud[i].noise_feedback = (f & 2) != 0;
            t->aud[i].noise_counter_bit4 = (f & 4) != 0;
            t->aud[i].pulse_counter_hold = (f & 8) != 0;
        }
        t->audio_sum[0] = (uint32_t)q[0] | ((uint32_t)q[1] << 8); q += 2;
        t->audio_sum[1] = (uint32_t)q[0] | ((uint32_t)q[1] << 8); q += 2;
        t->audio_sum_ct = (uint32_t)q[0] | ((uint32_t)q[1] << 8); q += 2;
        /* Region + palette-variant bytes (see serialize for layout).
         * Palette variant must be set before tia_set_region so the latter
         * picks the correct table. */
        t->palette_variant  = (enum tia_palette_variant)(q[2] & 0x01);
        tia_set_region(t, (enum tia_region)(q[0] & 0x03));
        t->detected_region  = (enum tia_region)(q[1] & 0x03);
        t->detect_locked    = (q[1] & 0x80) != 0;
        t->detect_count     = t->detect_locked ? TIA_DETECT_FRAMES : 0;
        memset(t->detect_samples, 0, sizeof(t->detect_samples));
    }
    /* Input: 7 bytes inpt state + 16 bytes paddle state. Located at offset 78. */
    {
        const uint8_t *ip = (const uint8_t *)buf + 78;
        int i;
        for (i = 0; i < 6; i++) t->inpt[i] = ip[i];
        t->inpt_ground = ip[6] != 0;
        ip += 7;
        for (i = 0; i < 4; i++) {
            t->paddle_charge_max[i] = (uint16_t)(ip[0] | ((uint16_t)ip[1] << 8));
            ip += 2;
        }
        for (i = 0; i < 4; i++) {
            t->paddle_charge_cnt[i] = (uint16_t)(ip[0] | ((uint16_t)ip[1] << 8));
            ip += 2;
        }
        /* Delay queue: 8 slots × 8 entries × (reg, value) + head. */
        {
            int s, e;
            for (s = 0; s < 8; s++)
                for (e = 0; e < 8; e++) {
                    t->delay_slots[s][e].reg   = *ip++;
                    t->delay_slots[s][e].value = *ip++;
                }
            t->delay_head = *ip++;
        }
        /* Cycle-accurate HMOVE state (7 bytes). */
        for (i = 0; i < 5; i++) t->hm_remaining[i] = (int8_t)*ip++;
        t->hmove_active = *ip++ != 0;
        t->hmove_ticks  = *ip++;
    }
    return true;
}

size_t tia_drain_audio(struct tia *t, int16_t *out, size_t max)
{
    size_t n = t->audio_buf_len;
    if (n > max) n = max;
    if (n > 0) memcpy(out, t->audio_buf, n * sizeof(int16_t));
    if (t->audio_buf_len > n)
        memmove(t->audio_buf, t->audio_buf + n,
                (t->audio_buf_len - n) * sizeof(int16_t));
    t->audio_buf_len = (uint16_t)(t->audio_buf_len - n);
    return n;
}
