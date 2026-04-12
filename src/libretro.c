/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "libretro.h"

#define FRAME_WIDTH  160
#define FRAME_HEIGHT 210
#define FRAME_PIXELS (FRAME_WIDTH * FRAME_HEIGHT)

static retro_environment_t     environ_cb;
static retro_video_refresh_t   video_cb;
static retro_audio_sample_t    audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t      input_poll_cb;
static retro_input_state_t     input_state_cb;

static struct retro_log_callback logging;
static retro_log_printf_t        log_cb;

static uint32_t framebuffer[FRAME_PIXELS];

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

void retro_init(void)
{
   memset(framebuffer, 0, sizeof(framebuffer));
}

void retro_deinit(void) { }

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "tia";
   info->library_version  = "0.0.1";
   info->need_fullpath    = false;
   info->valid_extensions = "a26|bin";
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->geometry.base_width   = FRAME_WIDTH;
   info->geometry.base_height  = FRAME_HEIGHT;
   info->geometry.max_width    = FRAME_WIDTH;
   info->geometry.max_height   = 312;
   info->geometry.aspect_ratio = 4.0f / 3.0f;
   info->timing.fps            = 60.0;
   info->timing.sample_rate    = 31400.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_reset(void) { }

void retro_run(void)
{
   input_poll_cb();
   video_cb(framebuffer, FRAME_WIDTH, FRAME_HEIGHT, FRAME_WIDTH * sizeof(uint32_t));
}

bool retro_load_game(const struct retro_game_info *info)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

   (void)info;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
      return false;
   }
   return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

void retro_unload_game(void) { }

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

size_t retro_serialize_size(void)              { return 0; }
bool   retro_serialize(void *data, size_t size) { (void)data; (void)size; return false; }
bool   retro_unserialize(const void *data, size_t size) { (void)data; (void)size; return false; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }

void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
