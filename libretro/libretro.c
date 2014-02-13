#include "libretro.h"
#include "thread.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include "libusb/libusb/libusb.h"

#define PSP_WIDTH 480
#define PSP_HEIGHT 272

// Magic stuff from RemoteJoy-Lite/Win32.
#define SONY_VID 0x054c
#define REMOTE_PID 0x01c9
#define REMOTE_PID2 0x02d2

#define TYPE_JOY_CMD 1
#define TYPE_JOY_DAT 2
#define ASYNC_CMD_DEBUG 1

#define HOSTFS_MAGIC 0x782f0812
#define ASYNC_MAGIC  0x782f0813
#define BULK_MAGIC   0x782f0814
#define JOY_MAGIC    0x909accef
#define RJL_VERSION				190
#define HOSTFS_CMD_HELLO(ver)	((0x8ffc << 16) | (ver))

/* Screen commands */
#define SCREEN_CMD_ACTIVE		(1 << 0)
#define SCREEN_CMD_SCROFF		(1 << 1)
#define SCREEN_CMD_DEBUG		(1 << 2)
#define SCREEN_CMD_ASYNC		(1 << 3)

/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
/* |    ADRESS2    |    ADRESS1    |  PRIORITY |  MODE |FPS|A|D|S|A| */
/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
#define SCREEN_CMD_SET_TRNSFPS(x)	((x) << 4)
#define SCREEN_CMD_GET_TRNSFPS(x)	(((x) >> 4) & 0x03)
#define SCREEN_CMD_SET_TRNSMODE(x)	((x) << 6)
#define SCREEN_CMD_GET_TRNSMODE(x)	(((x) >> 6) & 0x0f)
#define SCREEN_CMD_SET_PRIORITY(x)	((x) << 10)
#define SCREEN_CMD_GET_PRIORITY(x)	(((x) >> 10) & 0x3f)
#define SCREEN_CMD_SET_ADRESS1(x)	((x) << 16)
#define SCREEN_CMD_GET_ADRESS1(x)	(((x) >> 16) & 0xFF)
#define SCREEN_CMD_SET_ADRESS2(x)	((x) << 24)
#define SCREEN_CMD_GET_ADRESS2(x)	(((x) >> 24) & 0xFF)

/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
/* |      TRNSH      |    TRNSW    |      TRNSY      |    TRNSX    | */
/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
#define SCREEN_CMD_SET_TRNSX(x)		((x) << 0)
#define SCREEN_CMD_GET_TRNSX(x)		(((x) >> 0) & 0x7f)
#define SCREEN_CMD_SET_TRNSY(x)		((x) << 7)
#define SCREEN_CMD_GET_TRNSY(x)		(((x) >> 7) & 0x1ff)
#define SCREEN_CMD_SET_TRNSW(x)		((x) << 16)
#define SCREEN_CMD_GET_TRNSW(x)		(((x) >> 16) & 0x7f)
#define SCREEN_CMD_SET_TRNSH(x)		((x) << 23)
#define SCREEN_CMD_GET_TRNSH(x)		(((x) >> 23) & 0x1ff)

enum async_channels
{
   ASYNC_SHELL = 0,
   ASYNC_GDB,
   ASYNC_STDOUT,
   ASYNC_STDERR,
   ASYNC_USER
};

struct HostFsCmd
{
   uint32_t magic;
   uint32_t command;
   uint32_t extralen;
} __attribute__((packed));

struct JoyEvent
{
   uint32_t magic;
   int type;
   uint32_t value1;
   uint32_t value2;
} __attribute__((packed));

struct AsyncCommand
{
   uint32_t magic;
   uint32_t channel;
} __attribute__((packed));

struct BulkCommand
{
   uint32_t magic;
   uint32_t channel;
   uint32_t size;
} __attribute__((packed));

struct JoyScrHeader
{
   uint32_t magic;
   int32_t mode;
   int32_t size;
   int32_t ref;
} __attribute__((packed));

struct EventData
{
   struct AsyncCommand async;
   struct JoyEvent event;
} __attribute__((packed));

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static uint32_t g_frame[PSP_WIDTH * PSP_HEIGHT];
static uint32_t g_frame_buffer[PSP_WIDTH * PSP_HEIGHT];
static sthread_t *g_thread;
static slock_t   *g_lock;
static volatile sig_atomic_t g_thread_die;
static volatile sig_atomic_t g_thread_failed;

static libusb_context *g_ctx;
static libusb_device_handle *g_dev;

static inline uint32_t read_le32(const uint8_t *buf)
{
   return (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

static inline void write_le32(uint8_t *buf, uint32_t val)
{
   buf[0] = (uint8_t)(val >>  0);
   buf[1] = (uint8_t)(val >>  8);
   buf[2] = (uint8_t)(val >> 16);
   buf[3] = (uint8_t)(val >> 24);
}

#define le32(x) (x)

static bool send_event(int type, int val1, int val2)
{
   //printf("Sending event ...\n");
   struct EventData data = {
      .async = {
         .magic   = le32(ASYNC_MAGIC),
         .channel = le32(ASYNC_USER),
      },
      .event = {
         .magic  = JOY_MAGIC,
         .type   = type,
         .value1 = val1,
         .value2 = val2,
      },
   };

   int transferred;
   if (libusb_bulk_transfer(g_dev, 3, (uint8_t*)&data, sizeof(data), &transferred, 1000) < 0)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "send_event() failed.\n");
      return false;
   }

   return true;
}

static bool handle_hello(libusb_device_handle *dev)
{
   //printf("Handling hello!\n");

   struct HostFsCmd cmd = {
      .magic   = le32(HOSTFS_MAGIC),
      .command = le32(HOSTFS_CMD_HELLO(RJL_VERSION)),
   };

   int transferred;
   if (libusb_bulk_transfer(dev, 2, (uint8_t*)&cmd, sizeof(cmd), &transferred, 1000) < 0)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Failed hello.\n");
      return false;
   }

   uint32_t arg1 = SCREEN_CMD_ACTIVE | SCREEN_CMD_ASYNC;
   arg1 |= SCREEN_CMD_SET_TRNSFPS(0);
   arg1 |= SCREEN_CMD_SET_TRNSMODE(0);
   arg1 |= SCREEN_CMD_SET_PRIORITY(16);
   arg1 |= SCREEN_CMD_SET_ADRESS1(((0x086c0000 - 0x08400000) / 0x8000));
   arg1 |= SCREEN_CMD_SET_ADRESS2(((0x8b000000 - 0x8a000000) / 0x40000));

   uint32_t arg2 = SCREEN_CMD_SET_TRNSX(0) | SCREEN_CMD_SET_TRNSY(0) | SCREEN_CMD_SET_TRNSW(PSP_WIDTH / 32) | SCREEN_CMD_SET_TRNSH(PSP_HEIGHT / 2);

   if (!send_event(TYPE_JOY_CMD, le32(arg1), le32(arg2)))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "send_event() failed in hello().\n");
      return false;
   }

   return true;
}

static void texture_rgb565(const void *block_, size_t size)
{
   const uint16_t *block = block_;
   size >>= 1;

   if (size > PSP_WIDTH * PSP_HEIGHT)
      size = PSP_WIDTH * PSP_HEIGHT;

   for (size_t i = 0; i < size; i++)
   {
      uint16_t col = block[i];
      uint32_t r = (col >>  0) & 0x1f;
      uint32_t g = (col >>  5) & 0x3f;
      uint32_t b = (col >> 11) & 0x1f;
      r = (r << 3) | (r >> 2);
      g = (g << 2) | (g >> 4);
      b = (b << 3) | (b >> 2);
      g_frame[i] = (r << 16) | (g << 8) | (b << 0);
   }
}

static void texture_argb1555(const void *block_, size_t size)
{
   const uint16_t *block = block_;
   size >>= 1;

   if (size > PSP_WIDTH * PSP_HEIGHT)
      size = PSP_WIDTH * PSP_HEIGHT;

   for (size_t i = 0; i < size; i++)
   {
      uint16_t col = block[i];
      uint32_t r = (col >>  0) & 0x1f;
      uint32_t g = (col >>  5) & 0x1f;
      uint32_t b = (col >> 10) & 0x1f;
      r = (r << 3) | (r >> 2);
      g = (g << 3) | (g >> 2);
      b = (b << 3) | (b >> 2);
      g_frame[i] = (r << 16) | (g << 8) | (b << 0);
   }
}

static void texture_argb4444(const void *block_, size_t size)
{
   const uint16_t *block = block_;
   size >>= 1;

   if (size > PSP_WIDTH * PSP_HEIGHT)
      size = PSP_WIDTH * PSP_HEIGHT;

   for (size_t i = 0; i < size; i++)
   {
      uint16_t col = block[i];
      uint32_t r = (col >>  0) & 0x0f;
      uint32_t g = (col >>  4) & 0x0f;
      uint32_t b = (col >>  8) & 0x0f;
      r = (r << 4) | (r >> 0);
      g = (g << 4) | (g >> 0);
      b = (b << 4) | (b >> 0);
      g_frame[i] = (r << 16) | (g << 8) | (b << 0);
   }
}

static void texture_argb8888(const void *block_, size_t size)
{
   const uint32_t *block = block_;

   size >>= 2;
   if (size > PSP_WIDTH * PSP_HEIGHT)
      size = PSP_WIDTH * PSP_HEIGHT;

   for (size_t i = 0; i < size; i++)
   {
      uint32_t col = block[i];
      uint32_t r = (col >>  0) & 0xff;
      uint32_t g = (col >>  8) & 0xff;
      uint32_t b = (col >> 16) & 0xff;
      uint32_t a = (col >> 24) & 0xff;
      g_frame[i] = (r << 16) | (g << 8) | (b << 0) | (a << 24);
   }
}

static void process_bulk(const uint8_t *block, size_t size)
{
   struct JoyScrHeader *header = (struct JoyScrHeader*)block;
   //printf("Buff mode: %u\n", le32(header->mode));
   //printf("VCount: %d\n", le32(header->ref));
   //printf("Size: %d\n", le32(header->size));

   slock_lock(g_lock);

   //memcpy(g_frame, block + sizeof(*header), le32(header->size));

   switch ((header->mode >> 4) & 0x0f)
   {
      case 0x00:
         texture_rgb565(block + sizeof(*header), le32(header->size));
         break;

      case 0x01:
         texture_argb1555(block + sizeof(*header), le32(header->size));
         break;

      case 0x02:
         texture_argb4444(block + sizeof(*header), le32(header->size));
         break;

      case 0x03:
         texture_argb8888(block + sizeof(*header), le32(header->size));
         break;
      default:
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Unknown header mode %d.\n", (header->mode >> 4) & 0x0f);
   }

   slock_unlock(g_lock);
}

#define HOSTFS_MAX_BLOCK (1024 * 1024)
static bool handle_bulk(libusb_device_handle *dev, uint8_t *data, size_t size)
{
   static uint8_t bulk_block[HOSTFS_MAX_BLOCK];

   if (size < sizeof(struct BulkCommand))
      return false;

   struct BulkCommand *cmd = (struct BulkCommand*)data;
   size_t read_size = 0;
   size_t data_size = le32(cmd->size);
   //printf("Data size: %zu\n", data_size);

   while (read_size < data_size)
   {
      size_t to_read = data_size - read_size;
      if (to_read > HOSTFS_MAX_BLOCK)
         to_read = HOSTFS_MAX_BLOCK;

      int transferred = 0;
      int ret = libusb_bulk_transfer(dev, 0x01 | LIBUSB_ENDPOINT_IN,
            bulk_block + read_size, to_read, &transferred, 3000);

      if (ret < 0)
         return false;

      read_size += transferred;
   }

   process_bulk(bulk_block, data_size);
   return true;
}

static bool handle_async(libusb_device_handle *dev)
{
   (void)dev;
   return true;
}

static void bulk_thread(void *dummy)
{
   (void)dummy;

   uint8_t buffer[512];

   uint8_t mag[4];
   write_le32(mag, HOSTFS_MAGIC);
   int transferred = 0;
   int ret = libusb_bulk_transfer(g_dev, 2, mag, sizeof(mag), &transferred, 1000);
   if (ret < 0)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Failed to do magic init ... Error: %d\n", ret);
      goto error;
   }

   if (transferred < 4)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Didn't really transfer 4 bytes, wut ...\n");
      goto error;
   }

   bool active = false;

   while (!g_thread_die)
   {
      // TODO: Support joypad input.
      if (active)
      {
         if (!send_event(TYPE_JOY_DAT, 0, 0))
            return;
      }

      int transferred = 0;
      int ret = libusb_bulk_transfer(g_dev, 0x01 | LIBUSB_ENDPOINT_IN,
            buffer, sizeof(buffer), &transferred, 1000);

      if (ret < 0)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "Failed to do bulk with error: %d\n", ret);

         if (ret != LIBUSB_ERROR_TIMEOUT)
            goto error;
      }

      //printf("Transferred: %d\n", transferred);
      if (transferred < 4)
         continue;

      //for (int i = 0; i < transferred; i++)
      //   printf("0x%02x\n", buffer[i]);

      uint32_t code = read_le32(buffer + 0);
      if (code == HOSTFS_MAGIC)
      {
         //printf("HOSTFS_MAGIC\n");
         if (!handle_hello(g_dev))
            goto error;

         active = true;
      }
      else if (code == ASYNC_MAGIC)
      {
         //printf("ASYNC_MAGIC\n");
         if (!handle_async(g_dev))
            goto error;
      }
      else if (code == BULK_MAGIC)
      {
         //printf("BULK_MAGIC\n");
         if (!handle_bulk(g_dev, buffer, transferred))
            goto error;
      }
      //else
         //printf("Got other magic!\n");
   }

   return;
error:
   g_thread_failed = true;
}

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
   if (g_thread_failed)
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);

   // TODO: Employ a more "sane" scheme using conditional variables, etc.
   slock_lock(g_lock);
   memcpy(g_frame_buffer, g_frame, sizeof(g_frame));
   slock_unlock(g_lock);

   video_cb(g_frame_buffer, PSP_WIDTH, PSP_HEIGHT, PSP_WIDTH * sizeof(uint32_t));
   // No audio :(
   // TODO: Poll input here.
}

static void deinit_program(void)
{
   if (g_thread)
   {
      g_thread_die = true;
      sthread_join(g_thread);
      g_thread        = NULL;
      g_thread_die    = false;
      g_thread_failed = false;
   }

   if (g_lock)
   {
      slock_free(g_lock);
      g_lock = NULL;
   }

   if (g_dev)
   {
      libusb_release_interface(g_dev, 0);
      libusb_attach_kernel_driver(g_dev, 0);
      libusb_close(g_dev);
      g_dev = NULL;
   }

   if (g_ctx)
   {
      libusb_exit(g_ctx);
      g_ctx = NULL;
   }
}

bool retro_load_game(const struct retro_game_info *info)
{
   (void)info;

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,
            &(enum retro_pixel_format) { RETRO_PIXEL_FORMAT_XRGB8888 }))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "XRGB8888 isn't supported. Cannot continue ...\n");
      goto error;
   }

   if (libusb_init(&g_ctx) < 0)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "libusb_init failed.\n");
      goto error;
   }

   g_dev = libusb_open_device_with_vid_pid(g_ctx, SONY_VID, REMOTE_PID);
   if (!g_dev)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "libusb_open_device_with_vid_pid failed, trying attempt 2...\n");

      g_dev = libusb_open_device_with_vid_pid(g_ctx, SONY_VID, REMOTE_PID2);
      
      if (!g_dev)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "libusb_open_device_with_vid_pid attempt 2 failed...\n");
         goto error;
      }
   }

   if (libusb_kernel_driver_active(g_dev, 0))
   {
      if (libusb_detach_kernel_driver(g_dev, 0) < 0)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "libusb_detach_kernel_driver failed.\n");
         goto error;
      }
   }

   if (libusb_set_configuration(g_dev, 1) < 0)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "libusb_set_configuration failed.\n");
      goto error;
   }

   if (libusb_claim_interface(g_dev, 0) < 0)
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "libusb_claim_interface failed.\n");
      goto error;
   }

   g_lock = slock_new();
   if (!g_lock)
      goto error;

   g_thread_failed = false;
   g_thread_die    = false;
   g_thread = sthread_create(bulk_thread, NULL);
   if (!g_thread)
      goto error;

   return true;

error:
   deinit_program();
   return false;
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

