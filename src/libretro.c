/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"
#include "cpu.h"
#include "tia.h"
#include "riot.h"
#include "cart.h"
#include "bus.h"

#ifndef CORE_VERSION
#define CORE_VERSION "0+unknown"
#endif

#define FRAME_WIDTH       TIA_VISIBLE_WIDTH
/* Fixed height handed to video_cb and advertised in retro_get_system_av_info.
 * Kept constant frame-to-frame so the frontend's rescale is stable: if we
 * shipped a variable height (e.g. visible_end - visible_start, which drifts
 * by a scanline or two as VSYNC/VBLANK edges wobble), the same TIA scanline
 * would land at a different output Y each frame and stationary content would
 * appear to jitter vertically. The per-frame anchor (offset) still tracks
 * visible_start so games with different visible regions center correctly. */
#define FRAME_HEIGHT_NOMINAL 228
/* Fallback anchor used before the game has performed a VBLANK transition
 * (cold boot / a few frames of boot where visible_start is still the
 * 0xFFFF sentinel). */
#define SHIP_OFFSET_FALLBACK 30
#define MAX_CYCLES_PER_RUN 200000        /* safety cap if VSYNC never fires */
#define AUDIO_BUF_MONO    1024

static retro_environment_t        environ_cb;
static retro_video_refresh_t      video_cb;
static retro_audio_sample_t       audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t         input_poll_cb;
static retro_input_state_t        input_state_cb;

static struct retro_log_callback  logging;
static retro_log_printf_t         log_cb;

/* Controller device codes. Paddles and driving are subclasses of ANALOG so
 * the frontend can show them in its core-option menu. The keypad uses the
 * stock RETRO_DEVICE_KEYBOARD. */
#define DEV_JOYPAD  RETRO_DEVICE_JOYPAD
#define DEV_PADDLE  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)
#define DEV_DRIVING RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)
#define DEV_KEYPAD  RETRO_DEVICE_KEYBOARD

/* Paddle capacitor charge at maximum rotation, in TIA color clocks. Real
 * HW takes roughly one NTSC frame (~59,736 clocks) for the pot's RC
 * network to cross the comparator threshold. We use a slightly generous
 * value so games that expect the full-range dump have headroom. */
#define PADDLE_CHARGE_MAX_CLOCKS 65000u

/* Gray code for driving controller output on SWCHA bits 5:4 (P0) or 1:0
 * (P1). Turning the wheel advances through this table in one direction
 * or the other; games infer direction from the transition pattern. */
static const uint8_t driving_gray[4] = { 0x00, 0x01, 0x03, 0x02 };

static struct {
    struct cpu  cpu;
    struct tia  tia;
    struct riot riot;
    struct cart cart;
    struct bus  bus;
    unsigned    port_device[2];
    bool        loaded;
    /* Console switch state — latched, toggled by button edges. The six
     * switch-setting retropad buttons (L/L2/L3/R/R2/R3) each drive one
     * side of a physical toggle: L sets left-diff A, L2 sets left-diff
     * B, and so on. Initial values match common factory defaults. */
    bool        sw_color;        /* true = color, false = B&W */
    bool        sw_left_diff_a;  /* true = A (amateur), false = B (pro) */
    bool        sw_right_diff_a;
    /* Previous-frame button state for edge detection on the six
     * switch-setting buttons (one bit per JOYPAD_L..R3). */
    uint8_t     sw_prev;

    /* Keypad button state for ports in DEV_KEYPAD mode. One bit per key
     * in the order {1,2,3,4,5,6,7,8,9,*,0,#} -> bits 0..11. Bit set =
     * pressed. Updated by poll_inputs, consumed by keypad_update() on
     * every SWCHA/DDR change. */
    uint16_t    keypad_pressed[2];
} sys;

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
    va_list va;
    (void)level;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb)
{
    /* Port 0 advertises every 2600 controller type; port 1 omits the
     * single-port-only Driving controller (Indy 500 uses only P0). */
    static const struct retro_controller_description p0_ctrls[] = {
        { "Joystick", DEV_JOYPAD  },
        { "Paddles",  DEV_PADDLE  },
        { "Driving",  DEV_DRIVING },
        { "Keypad",   DEV_KEYPAD  },
    };
    static const struct retro_controller_description p1_ctrls[] = {
        { "Joystick", DEV_JOYPAD  },
        { "Paddles",  DEV_PADDLE  },
        { "Keypad",   DEV_KEYPAD  },
    };
    static const struct retro_controller_info ports[] = {
        { p0_ctrls, 4 },
        { p1_ctrls, 3 },
        { NULL, 0 },
    };

    environ_cb = cb;

    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
        log_cb = logging.log;
    else
        log_cb = fallback_log;

    cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void *)ports);
}

void retro_set_video_refresh(retro_video_refresh_t cb)       { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)         { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb)             { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb)           { input_state_cb = cb; }

void retro_init(void)
{
    memset(&sys, 0, sizeof(sys));
    sys.port_device[0] = DEV_JOYPAD;
    sys.port_device[1] = DEV_JOYPAD;
    /* Factory defaults for the console toggles: Color on, both
     * difficulty switches B (pro). Players flip them with L/L2/L3/R/R2/R3. */
    sys.sw_color        = true;
    sys.sw_left_diff_a  = false;
    sys.sw_right_diff_a = false;
}
void retro_deinit(void) { }

void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name     = "tia";
    info->library_version  = CORE_VERSION;
    info->need_fullpath    = false;
    info->valid_extensions = "a26|bin";
    info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = FRAME_WIDTH;
    info->geometry.base_height  = FRAME_HEIGHT_NOMINAL;
    info->geometry.max_width    = FRAME_WIDTH;
    info->geometry.max_height   = TIA_MAX_SCANLINES;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps            = 60.0;
    info->timing.sample_rate    = 31400.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port < 2) sys.port_device[port] = device;
}

void retro_reset(void)
{
    if (!sys.loaded) return;
    tia_reset(&sys.tia);
    riot_reset(&sys.riot);
    cpu_reset(&sys.cpu);
}

static int16_t js(unsigned port, unsigned id)
{
    return input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id);
}

static int16_t ax(unsigned port, unsigned idx, unsigned id)
{
    return input_state_cb(port, RETRO_DEVICE_ANALOG, idx, id);
}

/* Set `pa` nibble for port `port` (high nibble if port==0, low if port==1)
 * from a joystick's direction bits + fire. 4 bits active-low. */
static uint8_t pa_bits_joypad(uint8_t pa, unsigned port)
{
    unsigned shift = (port == 0) ? 4 : 0;
    if (js(port, RETRO_DEVICE_ID_JOYPAD_RIGHT)) pa &= (uint8_t)~(0x08u << shift);
    if (js(port, RETRO_DEVICE_ID_JOYPAD_LEFT))  pa &= (uint8_t)~(0x04u << shift);
    if (js(port, RETRO_DEVICE_ID_JOYPAD_DOWN))  pa &= (uint8_t)~(0x02u << shift);
    if (js(port, RETRO_DEVICE_ID_JOYPAD_UP))    pa &= (uint8_t)~(0x01u << shift);
    return pa;
}

/* Paddle mode: two paddles per port share SWCHA's direction bits for their
 * fire triggers. Layout per nibble: paddle-A trigger = bit 3 of nibble
 * (where joystick RIGHT lived), paddle-B trigger = bit 2 (joystick LEFT).
 * Bits 1,0 (DOWN/UP) stay high. Map RETRO A button to paddle-A fire and
 * RETRO B button to paddle-B fire so the common case — one-paddle games
 * like Kaboom — works from either A or B. */
static uint8_t pa_bits_paddle(uint8_t pa, unsigned port)
{
    unsigned shift = (port == 0) ? 4 : 0;
    int fire_a = js(port, RETRO_DEVICE_ID_JOYPAD_A)
              || js(port, RETRO_DEVICE_ID_JOYPAD_R);
    int fire_b = js(port, RETRO_DEVICE_ID_JOYPAD_B)
              || js(port, RETRO_DEVICE_ID_JOYPAD_L);
    if (fire_a) pa &= (uint8_t)~(0x08u << shift);
    if (fire_b) pa &= (uint8_t)~(0x04u << shift);
    return pa;
}

/* Convert a libretro analog axis [-32768, 32767] to a paddle charge-target
 * in TIA color clocks. 0 resistance (fully left) yields a 0-clock charge
 * (bit 7 flips high immediately). Max resistance (fully right) takes
 * PADDLE_CHARGE_MAX_CLOCKS clocks. */
static uint16_t paddle_charge_from_axis(int16_t axis)
{
    int v = axis + 32768;                  /* 0..65535 */
    unsigned long scaled = (unsigned long)v * PADDLE_CHARGE_MAX_CLOCKS / 65536ul;
    if (scaled > 0xFFFF) scaled = 0xFFFF;
    return (uint16_t)scaled;
}

/* Driving controller (Indy 500, P0 only). Libretro analog X is mapped to
 * a continuous rotation, and SWCHA bits 5:4 are set to the Gray code of
 * the current step. The game infers direction from bit-pair transitions,
 * not from absolute position. */
static uint8_t pa_bits_driving(uint8_t pa)
{
    int16_t axis = ax(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    /* 4 Gray-code steps per full libretro sweep. Multiply by a larger
     * factor to get one step per small wheel twitch; 64 here means the
     * full [-32768, 32767] range advances the wheel through 128 clicks,
     * which feels reasonable for Indy 500's steering sensitivity. */
    unsigned step = ((unsigned)(axis + 32768) / 512u) & 3u;
    uint8_t gray  = driving_gray[step];     /* bits 0:1 = gray pair */
    /* Drop the current bits 4,5 and overlay gray pair there. */
    pa &= (uint8_t)~0x30u;
    pa |= (uint8_t)((gray & 1u) << 4) | (uint8_t)(((gray >> 1) & 1u) << 5);
    return pa;
}

/* Notify the frontend's OSD that a console toggle changed. Prefers the
 * richer _EXT variant; falls back to the legacy one so old RetroArch
 * builds still see something. */
static void notify_switch_change(const char *text)
{
    struct retro_message_ext ext;
    struct retro_message     legacy;
    ext.msg      = text;
    ext.duration = 2000;                 /* ms */
    ext.priority = 1;
    ext.level    = RETRO_LOG_INFO;
    ext.target   = RETRO_MESSAGE_TARGET_OSD;
    ext.type     = RETRO_MESSAGE_TYPE_NOTIFICATION;
    ext.progress = -1;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &ext))
        return;
    legacy.msg    = text;
    legacy.frames = 120;                 /* ~2 s at 60 Hz */
    if (environ_cb) environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &legacy);
}

/* Flip a switch latch, but only notify when the requested position differs
 * from the current one — mirrors physical-switch semantics (pushing the
 * lever up when it's already up is a no-op). Pass msg==NULL to toggle
 * silently (the TV-type change is visible on screen and doesn't need a
 * notification). */
static void switch_set(bool *latch, bool value, const char *msg)
{
    if (*latch == value) return;
    *latch = value;
    if (msg) notify_switch_change(msg);
}

/* --- Keypad controller ---
 *
 * The Atari keypad is a 12-button 4-row × 3-column matrix:
 *    1  2  3
 *    4  5  6
 *    7  8  9
 *    *  0  #
 *
 * Row drivers are on SWCHA's output bits (the same bits that drive the
 * joystick direction pins). Column sense lines are on INPT0-3 / INPT4-5:
 *   Port 0 (left):  col 0 → INPT0,  col 1 → INPT1,  col 2 → INPT4
 *   Port 1 (right): col 0 → INPT2,  col 1 → INPT3,  col 2 → INPT5
 *
 * SWCHA output bits, per port, are (from row 0 to row 3):
 *   Port 0 rows: bit 7, bit 6, bit 5, bit 4   (high nibble)
 *   Port 1 rows: bit 3, bit 2, bit 1, bit 0   (low nibble)
 * When a row driver is pulled LOW (output 0) AND a button in that row is
 * pressed, that button's column pin reads LOW. Otherwise column reads HIGH.
 *
 * Retropad binding (our design; 12 buttons total, grouping the D-pad on
 * keypad 2/4/6/8 — the "arrow keys" of a phone keypad — so center button
 * "5" sits naturally on the primary action button):
 *   Keypad  Retropad        Column Row
 *     1     Y               0      0
 *     2     UP              1      0
 *     3     L               2      0
 *     4     LEFT            0      1
 *     5     A               1      1
 *     6     RIGHT           2      1
 *     7     X               0      2
 *     8     DOWN            1      2
 *     9     L2              2      2
 *     *     L3              0      3
 *     0     B               1      3
 *     #     R               2      3
 * START remains free as the console Reset button (so games like Star
 * Raiders can be (re)started from the keypad overlay without switching
 * back to the joystick overlay). SELECT, R2, R3 are unbound on the
 * keypad overlay. */
#define KP_1  0
#define KP_2  1
#define KP_3  2
#define KP_4  3
#define KP_5  4
#define KP_6  5
#define KP_7  6
#define KP_8  7
#define KP_9  8
#define KP_ST 9   /* * */
#define KP_0  10
#define KP_HS 11  /* # */

static const struct {
    uint8_t retro_id;
    uint8_t bit;
} keypad_map[12] = {
    { RETRO_DEVICE_ID_JOYPAD_Y,     KP_1  },
    { RETRO_DEVICE_ID_JOYPAD_UP,    KP_2  },
    { RETRO_DEVICE_ID_JOYPAD_L,     KP_3  },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,  KP_4  },
    { RETRO_DEVICE_ID_JOYPAD_A,     KP_5  },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT, KP_6  },
    { RETRO_DEVICE_ID_JOYPAD_X,     KP_7  },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,  KP_8  },
    { RETRO_DEVICE_ID_JOYPAD_L2,    KP_9  },
    { RETRO_DEVICE_ID_JOYPAD_L3,    KP_ST },
    { RETRO_DEVICE_ID_JOYPAD_B,     KP_0  },
    { RETRO_DEVICE_ID_JOYPAD_R,     KP_HS },
};

/* Which column contains each key (indexed by KP_*). */
static const uint8_t kp_col[12] = {
    0, 1, 2,  /* 1, 2, 3 */
    0, 1, 2,  /* 4, 5, 6 */
    0, 1, 2,  /* 7, 8, 9 */
    0, 1, 2,  /* *, 0, # */
};
/* Which row contains each key (indexed by KP_*). */
static const uint8_t kp_row[12] = {
    0, 0, 0,
    1, 1, 1,
    2, 2, 2,
    3, 3, 3,
};

/* Update INPT bits for any keypad-mode port, based on current SWCHA
 * output state + keypad button state. Called from riot_write's pa_changed
 * callback whenever the CPU strobes SWCHA or changes its DDR, and also
 * directly from poll_inputs (once per frame) to seed the initial state. */
static void keypad_update(void *ctx)
{
    unsigned p;
    uint8_t  pa_effective;
    /* Effective Port A pin state: bits with DDR=output reflect pa_out;
     * bits with DDR=input float high (1). */
    pa_effective = (uint8_t)((sys.riot.pa_out & sys.riot.pa_ddr)
                 | (uint8_t)~sys.riot.pa_ddr);
    (void)ctx;

    for (p = 0; p < 2; p++) {
        uint8_t  col_inpt[3];   /* col0, col1, col2: default HIGH (not pressed) */
        unsigned col;
        int      key;
        uint8_t  row_mask[4];   /* 1 if that row's driver is pulled LOW */
        int      r;

        if (sys.port_device[p] != DEV_KEYPAD) continue;

        col_inpt[0] = 0x80;
        col_inpt[1] = 0x80;
        col_inpt[2] = 0x80;

        /* Row r (0..3) is driven by DB-9 Pins One through Four.
         * Pin One = UP   = SWCHA bit 4 (port 0) / bit 0 (port 1)
         * Pin Two = DOWN = bit 5 / bit 1
         * Pin Three = LEFT = bit 6 / bit 2
         * Pin Four = RIGHT = bit 7 / bit 3
         * So row r maps to bit (4+r) for port 0, bit r for port 1. */
        for (r = 0; r < 4; r++) {
            uint8_t bit_idx = (uint8_t)((p ? 0 : 4) + r);
            uint8_t bit = (uint8_t)(1u << bit_idx);
            row_mask[r] = ((pa_effective & bit) == 0) ? 1 : 0;
        }

        for (key = 0; key < 12; key++) {
            if (!(sys.keypad_pressed[p] & (1u << key))) continue;
            if (!row_mask[kp_row[key]]) continue;
            col = kp_col[key];
            col_inpt[col] = 0x00;   /* column pulled LOW */
        }

        /* Map columns to INPT indices. */
        if (p == 0) {
            sys.tia.inpt[0] = col_inpt[0];
            sys.tia.inpt[1] = col_inpt[1];
            sys.tia.inpt[4] = col_inpt[2];
        } else {
            sys.tia.inpt[2] = col_inpt[0];
            sys.tia.inpt[3] = col_inpt[1];
            sys.tia.inpt[5] = col_inpt[2];
        }
    }
}

static void poll_inputs(void)
{
    uint8_t pa = 0xFF;
    uint8_t pb;
    unsigned p;

    /* SWCHA: each port contributes a nibble. High nibble (bits 4-7) = port 0,
     * low nibble (bits 0-3) = port 1. Active-low throughout. */
    for (p = 0; p < 2; p++) {
        switch (sys.port_device[p]) {
        case DEV_PADDLE:
            pa = pa_bits_paddle(pa, p);
            break;
        case DEV_DRIVING:
            /* Driving is P0-only. On port 1 it falls back to joypad. */
            if (p == 0) pa = pa_bits_driving(pa);
            else        pa = pa_bits_joypad(pa, p);
            break;
        case DEV_KEYPAD:
            /* Keypad leaves SWCHA high nibble (port 0) / low nibble (port 1)
             * untouched — the console drives rows by writing SWCHA, and the
             * keypad reports columns via INPT, not via SWCHA reads. The
             * per-port nibble here stays active-high (no movement bits). */
            break;
        case DEV_JOYPAD:
        default:
            pa = pa_bits_joypad(pa, p);
            break;
        }
    }
    sys.riot.pa_in = pa;

    /* SWCHB: console switches. Read from port 0 regardless of active
     * controller — these are on the console, not the controller.
     *   bit 0: Reset   (active-low; 0 = pressed)
     *   bit 1: Select  (active-low)
     *   bit 2: unused, reads as 1
     *   bit 3: Color/B&W           (1 = Color)
     *   bits 4-5: unused, read as 1
     *   bit 6: Left Difficulty     (1 = A / amateur)
     *   bit 7: Right Difficulty    (1 = A / amateur)
     *
     * The three toggle switches are state machines driven by button edges.
     * Button → switch assignment follows the common 2600 touchscreen overlay
     * (common-overlays/gamepads/Named_Overlays/Atari - 2600.cfg): each switch
     * gets an adjacent row-pair (L/R for left-diff, L2/R2 for right-diff,
     * L3/R3 for TV type), upper = A, lower = B.
     *
     * Skip when port 0 is in keypad mode — L, R, L2, L3 are keypad keys
     * in that configuration and pressing them should NOT toggle the console
     * switches. Users can switch back to the joystick overlay to reach the
     * toggles, or use a physical gamepad whose shoulder buttons aren't
     * consumed by the keypad overlay. */
    if (sys.port_device[0] != DEV_KEYPAD)
    {
        uint8_t edge_now = 0;
        if (js(0, RETRO_DEVICE_ID_JOYPAD_L))  edge_now |= 0x01;
        if (js(0, RETRO_DEVICE_ID_JOYPAD_R))  edge_now |= 0x02;
        if (js(0, RETRO_DEVICE_ID_JOYPAD_L2)) edge_now |= 0x04;
        if (js(0, RETRO_DEVICE_ID_JOYPAD_R2)) edge_now |= 0x08;
        if (js(0, RETRO_DEVICE_ID_JOYPAD_L3)) edge_now |= 0x10;
        if (js(0, RETRO_DEVICE_ID_JOYPAD_R3)) edge_now |= 0x20;
        /* Rising-edge only, so holding doesn't chatter the latch. On an
         * actual state change the OSD notifies the user so they see what
         * they just toggled (console switches are easy to hit by accident
         * and hard to diagnose otherwise). A/B are just the switch
         * positions; their meaning is game-specific (e.g. A is *harder*
         * in Adventure but easier in Combat). */
        {
            uint8_t rising = (uint8_t)(edge_now & ~sys.sw_prev);
            if (rising & 0x01) switch_set(&sys.sw_left_diff_a,  true,
                                "Left Difficulty: A");
            if (rising & 0x02) switch_set(&sys.sw_left_diff_a,  false,
                                "Left Difficulty: B");
            if (rising & 0x04) switch_set(&sys.sw_right_diff_a, true,
                                "Right Difficulty: A");
            if (rising & 0x08) switch_set(&sys.sw_right_diff_a, false,
                                "Right Difficulty: B");
            if (rising & 0x10) switch_set(&sys.sw_color,        true,  NULL);
            if (rising & 0x20) switch_set(&sys.sw_color,        false, NULL);
        }
        sys.sw_prev = edge_now;
    }
    pb = 0x3F;                                          /* unused bits high */
    if (js(0, RETRO_DEVICE_ID_JOYPAD_START))  pb &= (uint8_t)~0x01;
    if (js(0, RETRO_DEVICE_ID_JOYPAD_SELECT)) pb &= (uint8_t)~0x02;
    if (!sys.sw_color)       pb &= (uint8_t)~0x08;
    if (sys.sw_left_diff_a)  pb |= 0x40;
    if (sys.sw_right_diff_a) pb |= 0x80;
    sys.riot.pb_in = pb;

    /* Read retropad → keypad button state for any keypad-mode port. The
     * actual INPT bits get computed by keypad_update() after we finish
     * updating the pressed-key bitmap, and again whenever the CPU writes
     * SWCHA / SWACNT (via the riot pa_changed callback). */
    for (p = 0; p < 2; p++) {
        if (sys.port_device[p] != DEV_KEYPAD) { sys.keypad_pressed[p] = 0; continue; }
        {
            uint16_t m = 0;
            int      k;
            for (k = 0; k < 12; k++)
                if (js(p, keypad_map[k].retro_id))
                    m |= (uint16_t)(1u << keypad_map[k].bit);
            sys.keypad_pressed[p] = m;
        }
    }

    /* INPT4/5: joystick/driving fire buttons (bit 7 only; rest are 0).
     * Paddles don't use INPT4/5 (their triggers are on SWCHA). Keypad
     * mode uses INPT4/5 as its column-2 sense line; that's set below by
     * keypad_update(). */
    for (p = 0; p < 2; p++) {
        unsigned idx = 4 + p;
        switch (sys.port_device[p]) {
        case DEV_PADDLE:
            sys.tia.inpt[idx] = 0x80;       /* unused in paddle mode */
            break;
        case DEV_DRIVING:
            sys.tia.inpt[idx] = (p == 0 && js(p, RETRO_DEVICE_ID_JOYPAD_B))
                              ? 0x00 : 0x80;
            break;
        case DEV_KEYPAD:
            /* set by keypad_update() below */
            break;
        case DEV_JOYPAD:
        default:
            sys.tia.inpt[idx] = js(p, RETRO_DEVICE_ID_JOYPAD_B) ? 0x00 : 0x80;
            break;
        }
    }

    /* Compute keypad INPT bits with the freshly-polled button state. */
    keypad_update(NULL);

    /* INPT0-3: paddle potentiometer capacitor dumps. Each port contributes
     * two paddles (A = LEFT stick X, B = RIGHT stick X) when in paddle
     * mode; otherwise the charge target is 0 so bit 7 stays high. */
    {
        unsigned slot = 0;
        for (p = 0; p < 2; p++) {
            if (sys.port_device[p] == DEV_PADDLE) {
                int16_t a_axis = ax(p, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                                       RETRO_DEVICE_ID_ANALOG_X);
                int16_t b_axis = ax(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                                       RETRO_DEVICE_ID_ANALOG_X);
                sys.tia.paddle_charge_max[slot + 0] = paddle_charge_from_axis(a_axis);
                sys.tia.paddle_charge_max[slot + 1] = paddle_charge_from_axis(b_axis);
            } else {
                sys.tia.paddle_charge_max[slot + 0] = 0;
                sys.tia.paddle_charge_max[slot + 1] = 0;
            }
            slot += 2;
        }
    }
}

void retro_run(void)
{
    int cycles = 0;
    if (!sys.loaded) return;

    input_poll_cb();
    poll_inputs();

    while (!sys.tia.frame_ready && !sys.cpu.halted && cycles < MAX_CYCLES_PER_RUN) {
        cpu_step(&sys.cpu);
        cycles++;
    }
    sys.tia.frame_ready = false;

    {
        /* 228-row output: 18 rows of top VBLANK padding (rendered black in
         * tia_tick) + ~189 rows of visible content + bottom VBLANK/overscan
         * padding. Anchor to visible_start - 18 so the top-VBLANK padding
         * lands at the top of the output frame, which is where the NTSC
         * video signal starts each field; most 2600 cores follow the same
         * convention so screenshots align across cores. */
        uint16_t anchor = (sys.tia.visible_start != 0xFFFF)
                        ? sys.tia.visible_start
                        : SHIP_OFFSET_FALLBACK;
        uint16_t offset = (anchor > 18) ? (uint16_t)(anchor - 18) : 0;
        uint16_t height = FRAME_HEIGHT_NOMINAL;
        if (offset + height > TIA_MAX_SCANLINES)
            height = (uint16_t)(TIA_MAX_SCANLINES - offset);
        video_cb(sys.tia.fb + (size_t)offset * FRAME_WIDTH,
                 FRAME_WIDTH, height,
                 FRAME_WIDTH * sizeof(uint32_t));
    }

    {
        int16_t mono[AUDIO_BUF_MONO];
        int16_t stereo[AUDIO_BUF_MONO * 2];
        size_t n = tia_drain_audio(&sys.tia, mono, AUDIO_BUF_MONO);
        size_t i;
        for (i = 0; i < n; i++) {
            stereo[i * 2]     = mono[i];
            stereo[i * 2 + 1] = mono[i];
        }
        if (n > 0) audio_batch_cb(stereo, n);
    }
}

bool retro_load_game(const struct retro_game_info *info)
{
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

    if (!info || !info->data) return false;

    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
        return false;
    }

    if (!cart_load(&sys.cart, info->data, info->size)) {
        log_cb(RETRO_LOG_ERROR, "Unsupported ROM size %zu (only 2048 or 4096).\n",
               info->size);
        return false;
    }

    {
        /* Button assignments follow the common 2600 touchscreen overlay
         * (common-overlays: "Atari - 2600.cfg"): each toggle switch gets a
         * dedicated row-pair of shoulder buttons. Console switches live on
         * port 0 only — they're on the console, not the controller. */
        static const struct retro_input_descriptor desc[] = {
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Reset" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Left Difficulty A" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Left Difficulty B" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Right Difficulty A" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Right Difficulty B" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Color" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Black/White" },
            { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
            { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
            { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
            { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
            { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" },
            { 0, 0, 0, 0, NULL }
        };
        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
    }

    tia_init(&sys.tia);
    riot_init(&sys.riot);
    sys.riot.pa_changed     = keypad_update;
    sys.riot.pa_changed_ctx = NULL;
    bus_init(&sys.bus, &sys.cpu, &sys.tia, &sys.riot, &sys.cart);
    {
        struct cpu_bus cb;
        cb.read  = bus_read;
        cb.write = bus_write;
        cb.ctx   = &sys.bus;
        cpu_init(&sys.cpu, cb);
    }
    sys.cart.cpu_cycles = &sys.cpu.cycles;
    cpu_reset(&sys.cpu);
    sys.loaded = true;
    return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

void retro_unload_game(void) { sys.loaded = false; }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

/* Cart serialise layout: full ROM (CART_MAX_SIZE) + 4-byte size + 1 mapper +
 * 1 bank + 4 e0_slots + CART_SC_RAM_SIZE sc_ram + 1 sc_enabled flag.
 * Serialising the full ROM at max size (not just sys.cart.size) keeps the
 * serialize_size constant, which libretro prefers — the frontend only calls
 * retro_serialize_size once, then reuses the buffer. */
/* DPC state: 8 tops + 8 bottoms + 16 counter bytes + 8 flags + 3 music
 * mode + 1 random + 8 audio_cycles + 8 frac_clocks = 60 bytes. */
#define CART_DPC_SER_BYTES 60
#define CART_SER_BYTES (CART_MAX_SIZE + 4 + 1 + 1 + 4 + CART_SC_RAM_SIZE + 1 + 1 + CART_DPC_SER_BYTES)
#define SYS_SER_BYTES  1                /* packed console-switch state */

size_t retro_serialize_size(void)
{
    return cpu_serialize_size()
         + riot_serialize_size()
         + tia_serialize_size()
         + CART_SER_BYTES
         + SYS_SER_BYTES;
}

bool retro_serialize(void *data, size_t size)
{
    uint8_t *p = (uint8_t *)data;
    size_t need = retro_serialize_size();
    if (size < need) return false;
    cpu_serialize(&sys.cpu, p);   p += cpu_serialize_size();
    riot_serialize(&sys.riot, p); p += riot_serialize_size();
    tia_serialize(&sys.tia, p);   p += tia_serialize_size();

    memcpy(p, sys.cart.data, CART_MAX_SIZE);     p += CART_MAX_SIZE;
    p[0] = (uint8_t)(sys.cart.size & 0xFF);
    p[1] = (uint8_t)((sys.cart.size >>  8) & 0xFF);
    p[2] = (uint8_t)((sys.cart.size >> 16) & 0xFF);
    p[3] = (uint8_t)((sys.cart.size >> 24) & 0xFF);
    p += 4;
    *p++ = sys.cart.mapper;
    *p++ = sys.cart.bank;
    memcpy(p, sys.cart.e0_slots, 4);             p += 4;
    memcpy(p, sys.cart.sc_ram, CART_SC_RAM_SIZE); p += CART_SC_RAM_SIZE;
    *p++ = sys.cart.sc_enabled ? 1u : 0u;
    *p++ = sys.cart.fe_pending ? 1u : 0u;

    /* DPC state */
    memcpy(p, sys.cart.dpc_tops, 8);     p += 8;
    memcpy(p, sys.cart.dpc_bottoms, 8);  p += 8;
    { int i; for (i = 0; i < 8; i++) {
        p[0] = (uint8_t)(sys.cart.dpc_counters[i] & 0xFF);
        p[1] = (uint8_t)(sys.cart.dpc_counters[i] >> 8);
        p += 2;
    }}
    memcpy(p, sys.cart.dpc_flags, 8);    p += 8;
    p[0] = sys.cart.dpc_music_mode[0] ? 1u : 0u;
    p[1] = sys.cart.dpc_music_mode[1] ? 1u : 0u;
    p[2] = sys.cart.dpc_music_mode[2] ? 1u : 0u;
    p += 3;
    *p++ = sys.cart.dpc_random;
    { int i; for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(sys.cart.dpc_audio_cycles >> (i * 8));
    } p += 8; }
    memcpy(p, &sys.cart.dpc_frac_clocks, 8); p += 8;

    *p++ = (uint8_t)((sys.sw_color        ? 0x01u : 0u)
                   | (sys.sw_left_diff_a  ? 0x02u : 0u)
                   | (sys.sw_right_diff_a ? 0x04u : 0u));
    return true;
}

bool retro_unserialize(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t need = retro_serialize_size();
    if (size < need) return false;
    if (!cpu_deserialize(&sys.cpu,  p, cpu_serialize_size()))  return false;
    p += cpu_serialize_size();
    if (!riot_deserialize(&sys.riot, p, riot_serialize_size())) return false;
    p += riot_serialize_size();
    if (!tia_deserialize(&sys.tia,  p, tia_serialize_size()))  return false;
    p += tia_serialize_size();

    memcpy(sys.cart.data, p, CART_MAX_SIZE); p += CART_MAX_SIZE;
    sys.cart.size = (uint32_t)p[0]
                  | ((uint32_t)p[1] << 8)
                  | ((uint32_t)p[2] << 16)
                  | ((uint32_t)p[3] << 24);
    p += 4;
    sys.cart.mapper = *p++;
    sys.cart.bank   = *p++;
    memcpy(sys.cart.e0_slots, p, 4);            p += 4;
    memcpy(sys.cart.sc_ram, p, CART_SC_RAM_SIZE); p += CART_SC_RAM_SIZE;
    sys.cart.sc_enabled = *p++ != 0;
    sys.cart.fe_pending = *p++ != 0;

    /* DPC state */
    memcpy(sys.cart.dpc_tops, p, 8);     p += 8;
    memcpy(sys.cart.dpc_bottoms, p, 8);  p += 8;
    { int i; for (i = 0; i < 8; i++) {
        sys.cart.dpc_counters[i] = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        p += 2;
    }}
    memcpy(sys.cart.dpc_flags, p, 8);    p += 8;
    sys.cart.dpc_music_mode[0] = p[0] != 0;
    sys.cart.dpc_music_mode[1] = p[1] != 0;
    sys.cart.dpc_music_mode[2] = p[2] != 0;
    p += 3;
    sys.cart.dpc_random = *p++;
    { int i; sys.cart.dpc_audio_cycles = 0;
      for (i = 0; i < 8; i++)
        sys.cart.dpc_audio_cycles |= ((uint64_t)p[i] << (i * 8));
      p += 8;
    }
    memcpy(&sys.cart.dpc_frac_clocks, p, 8); p += 8;

    {
        uint8_t sw = *p++;
        sys.sw_color        = (sw & 0x01) != 0;
        sys.sw_left_diff_a  = (sw & 0x02) != 0;
        sys.sw_right_diff_a = (sw & 0x04) != 0;
        /* sw_prev is transient frame state, not saved. Reset so the first
         * post-load poll doesn't mis-fire a rising-edge. */
        sys.sw_prev = 0;
    }
    return true;
}

void *retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SYSTEM_RAM) return sys.riot.ram;
    return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SYSTEM_RAM) return 128;
    return 0;
}

void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}
