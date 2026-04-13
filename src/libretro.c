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
/* Nominal base height reported in retro_get_system_av_info. The actual
 * shipped height is dynamic per-frame (visible_end - visible_start) so the
 * frontend stretches game content to fill the display without us baking in
 * black letterbox bars. The frontend receives updated geometry via the
 * width/height args to video_cb. */
#define FRAME_HEIGHT_NOMINAL 192
/* Fallback shipped window used before the game has performed a VBLANK
 * transition (cold boot / a few frames of boot where visible_start is still
 * the 0xFFFF sentinel). */
#define SHIP_OFFSET_FALLBACK 30
#define SHIP_HEIGHT_FALLBACK 192
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

static struct {
    struct cpu  cpu;
    struct tia  tia;
    struct riot riot;
    struct cart cart;
    struct bus  bus;
    bool        loaded;
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
    static const struct retro_controller_description controllers[] = {
        { "Joystick", RETRO_DEVICE_JOYPAD },
    };
    static const struct retro_controller_info ports[] = {
        { controllers, 1 },
        { controllers, 1 },
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

void retro_init(void)  { memset(&sys, 0, sizeof(sys)); }
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
    (void)port;
    (void)device;
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

static void poll_inputs(void)
{
    uint8_t pa = 0xFF;
    uint8_t pb;
    /* SWCHA: joystick directions, active-low. P0 in high nibble, P1 in low. */
    if (js(0, RETRO_DEVICE_ID_JOYPAD_RIGHT)) pa &= (uint8_t)~0x80;
    if (js(0, RETRO_DEVICE_ID_JOYPAD_LEFT))  pa &= (uint8_t)~0x40;
    if (js(0, RETRO_DEVICE_ID_JOYPAD_DOWN))  pa &= (uint8_t)~0x20;
    if (js(0, RETRO_DEVICE_ID_JOYPAD_UP))    pa &= (uint8_t)~0x10;
    if (js(1, RETRO_DEVICE_ID_JOYPAD_RIGHT)) pa &= (uint8_t)~0x08;
    if (js(1, RETRO_DEVICE_ID_JOYPAD_LEFT))  pa &= (uint8_t)~0x04;
    if (js(1, RETRO_DEVICE_ID_JOYPAD_DOWN))  pa &= (uint8_t)~0x02;
    if (js(1, RETRO_DEVICE_ID_JOYPAD_UP))    pa &= (uint8_t)~0x01;
    sys.riot.pa_in = pa;

    /* SWCHB: console switches. Bits 0,1 = Reset/Select (active-low);
     * bit 3 = Color (1 = Color); bits 6,7 = Difficulty L/R (1 = A/pro).
     * Defaults: Color + Diff B/B, unused bits 2,4,5 high. */
    pb = 0x3B;
    if (js(0, RETRO_DEVICE_ID_JOYPAD_START))  pb &= (uint8_t)~0x01;
    if (js(0, RETRO_DEVICE_ID_JOYPAD_SELECT)) pb &= (uint8_t)~0x02;
    sys.riot.pb_in = pb;

    /* INPT4/5: joystick fire buttons (bit 7 only; rest are 0). */
    sys.tia.inpt[4] = js(0, RETRO_DEVICE_ID_JOYPAD_B) ? 0x00 : 0x80;
    sys.tia.inpt[5] = js(1, RETRO_DEVICE_ID_JOYPAD_B) ? 0x00 : 0x80;
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
        uint16_t offset, height;
        if (sys.tia.visible_start != 0xFFFF &&
            sys.tia.visible_end   != 0xFFFF &&
            sys.tia.visible_end   >  sys.tia.visible_start) {
            offset = sys.tia.visible_start;
            height = (uint16_t)(sys.tia.visible_end - sys.tia.visible_start);
        } else {
            offset = SHIP_OFFSET_FALLBACK;
            height = SHIP_HEIGHT_FALLBACK;
        }
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
        static const struct retro_input_descriptor desc[] = {
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Up" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Down" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Left" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Right" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Fire" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Reset" },
            { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
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
    bus_init(&sys.bus, &sys.cpu, &sys.tia, &sys.riot, &sys.cart);
    {
        struct cpu_bus cb;
        cb.read  = bus_read;
        cb.write = bus_write;
        cb.ctx   = &sys.bus;
        cpu_init(&sys.cpu, cb);
    }
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
#define CART_SER_BYTES (CART_MAX_SIZE + 4 + 1 + 1 + 4 + CART_SC_RAM_SIZE + 1)

size_t retro_serialize_size(void)
{
    return cpu_serialize_size()
         + riot_serialize_size()
         + tia_serialize_size()
         + CART_SER_BYTES;
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
