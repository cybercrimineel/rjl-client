#include "libretro.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "libretro.h"
#include "remotejoy.h"

retro_log_printf_t log_cb;
retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;
}

void retro_deinit(void)
{}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "RemoteJoy";
   info->library_version  = "v1";
   info->need_fullpath    = false;
   info->valid_extensions = "exe"; // Anything is fine, we don't care.
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing = (struct retro_system_timing) {
      .fps = 60.0,
      .sample_rate = 32000.0,
   };

   info->geometry = (struct retro_game_geometry) {
      .base_width   = PSP_WIDTH,
      .base_height  = PSP_HEIGHT,
      .max_width    = PSP_WIDTH,
      .max_height   = PSP_HEIGHT,
   };
}


void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
   bool tmp = true;
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &tmp);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{}

void retro_run(void)
{
   run_program();
}

bool retro_load_game(const struct retro_game_info *info)
{
   (void)info;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,
            &(enum retro_pixel_format) { RETRO_PIXEL_FORMAT_XRGB8888 }))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "XRGB8888 isn't supported. Cannot continue ...\n");
      return false;
   }

   return init_program();
}

void retro_unload_game(void)
{
   deinit_program();
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

bool retro_unserialize(const void *data, size_t size)
{
   (void)data;
   (void)size;
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}
