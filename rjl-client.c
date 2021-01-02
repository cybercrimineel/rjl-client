#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include "libusb-1.0.24/libusb/libusb.h"
#include "SDL2-2.0.14/include/SDL.h"
#include "SDL2-2.0.14/include/SDL_render.h"

#define TYPE_JOY_CMD 1
#define TYPE_JOY_DAT 2
#define ASYNC_CMD_DEBUG 1

#define HOSTFS_MAGIC 0x782f0812
#define ASYNC_MAGIC 0x782f0813
#define BULK_MAGIC 0x782f0814
#define JOY_MAGIC 0x909accef
#define RJL_VERSION 190
#define HOSTFS_CMD_HELLO(ver) ((0x8ffc << 16) | (ver))

/* Screen commands */
#define SCREEN_CMD_ACTIVE (1 << 0)
#define SCREEN_CMD_SCROFF (1 << 1)
#define SCREEN_CMD_DEBUG (1 << 2)
#define SCREEN_CMD_ASYNC (1 << 3)

/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
/* |    ADRESS2    |    ADRESS1    |  PRIORITY |  MODE |FPS|A|D|S|A| */
/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
#define SCREEN_CMD_SET_TRNSFPS(x) ((x) << 4)
#define SCREEN_CMD_GET_TRNSFPS(x) (((x) >> 4) & 0x03)
#define SCREEN_CMD_SET_TRNSMODE(x) ((x) << 6)
#define SCREEN_CMD_GET_TRNSMODE(x) (((x) >> 6) & 0x0f)
#define SCREEN_CMD_SET_PRIORITY(x) ((x) << 10)
#define SCREEN_CMD_GET_PRIORITY(x) (((x) >> 10) & 0x3f)
#define SCREEN_CMD_SET_ADRESS1(x) ((x) << 16)
#define SCREEN_CMD_GET_ADRESS1(x) (((x) >> 16) & 0xFF)
#define SCREEN_CMD_SET_ADRESS2(x) ((x) << 24)
#define SCREEN_CMD_GET_ADRESS2(x) (((x) >> 24) & 0xFF)

/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
/* |      TRNSH      |    TRNSW    |      TRNSY      |    TRNSX    | */
/* +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ */
#define SCREEN_CMD_SET_TRNSX(x) ((x) << 0)
#define SCREEN_CMD_GET_TRNSX(x) (((x) >> 0) & 0x7f)
#define SCREEN_CMD_SET_TRNSY(x) ((x) << 7)
#define SCREEN_CMD_GET_TRNSY(x) (((x) >> 7) & 0x1ff)
#define SCREEN_CMD_SET_TRNSW(x) ((x) << 16)
#define SCREEN_CMD_GET_TRNSW(x) (((x) >> 16) & 0x7f)
#define SCREEN_CMD_SET_TRNSH(x) ((x) << 23)
#define SCREEN_CMD_GET_TRNSH(x) (((x) >> 23) & 0x1ff)

#define PSP_WIDTH 480
#define PSP_HEIGHT 272

// Magic stuff from RemoteJoy-Lite/Win32.
#define SONY_VID 0x054c
#define REMOTE_PID 0x01c9
#define REMOTE_PID2 0x02d2

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

static volatile sig_atomic_t g_thread_die;
static volatile sig_atomic_t g_thread_failed;
static uint32_t g_frame[PSP_WIDTH * PSP_HEIGHT];
static uint32_t g_frame_buffer[PSP_WIDTH * PSP_HEIGHT];
static sthread_t *g_thread;

static libusb_context *g_ctx;
static libusb_device_handle *g_dev;

static inline uint32_t read_le32(const uint8_t *buf)
{
   return (buf[0] << 0) | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

static inline void write_le32(uint8_t *buf, uint32_t val)
{
   buf[0] = (uint8_t)(val >> 0);
   buf[1] = (uint8_t)(val >> 8);
   buf[2] = (uint8_t)(val >> 16);
   buf[3] = (uint8_t)(val >> 24);
}

#define le32(x) (x)

#define PSP_WIDTH 480
#define PSP_HEIGHT 272

// Magic stuff from RemoteJoy-Lite/Win32.
#define SONY_VID 0x054c
#define REMOTE_PID 0x01c9
#define REMOTE_PID2 0x02d2

static SDL_Renderer *g_renderer;
static SDL_Texture *g_textures[] = {NULL, NULL, NULL, NULL};

static bool send_event(int type, int val1, int val2)
{
   //printf("Sending event ...\n");
   struct EventData data = {
       .async = {
           .magic = le32(ASYNC_MAGIC),
           .channel = le32(ASYNC_USER),
       },
       .event = {
           .magic = JOY_MAGIC,
           .type = type,
           .value1 = val1,
           .value2 = val2,
       },
   };

   int transferred;
   if (libusb_bulk_transfer(g_dev, 3, (uint8_t *)&data, sizeof(data), &transferred, 1000) < 0)
   {
      puts("send_event() failed.");
      return false;
   }

   return true;
}

static bool handle_hello(libusb_device_handle *dev)
{
   //printf("Handling hello!\n");

   struct HostFsCmd cmd = {
       .magic = le32(HOSTFS_MAGIC),
       .command = le32(HOSTFS_CMD_HELLO(RJL_VERSION)),
   };

   int transferred;
   if (libusb_bulk_transfer(dev, 2, (uint8_t *)&cmd, sizeof(cmd), &transferred, 1000) < 0)
   {
      puts("Failed hello.");
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
      puts("send_event() failed in hello().");
      return false;
   }

   return true;
}

static void process_bulk(const uint8_t *block, size_t size)
{
   struct JoyScrHeader *header = (struct JoyScrHeader *)block;
   printf("Buff mode: %u\n", le32(header->mode));
   printf("VCount: %d\n", le32(header->ref));
   printf("Size: %d\n", le32(header->size));

   //memcpy(g_frame, block + sizeof(*header), le32(header->size));
   int32_t mode = (header->mode >> 4) & 0x0f;

   if (mode < 0 || mode > 3)
   {
      printf("Unknown header mode %d.\n", (header->mode >> 4) & 0x0f);
      return;
   }

   int32_t size = le32(header->size);

   if (size > PSP_WIDTH * PSP_HEIGHT)
   {
      printf("Too big header size %d.\n", size);
      return;
   }

   SDL_Texture *texture = get_texture(mode);
   void **pixels = NULL;
   int *pitch = NULL;
   SDL_LockTexture(texture, NULL, &pixels, &pitch);
   memcpy(pixels, block + sizeof(*header), size);
   SDL_UnlockTexture(texture);
}

#define HOSTFS_MAX_BLOCK (1024 * 1024)
static bool handle_bulk(libusb_device_handle *dev, uint8_t *data, size_t size)
{
   static uint8_t bulk_block[HOSTFS_MAX_BLOCK];

   if (size < sizeof(struct BulkCommand))
      return false;

   struct BulkCommand *cmd = (struct BulkCommand *)data;
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

static int usb_check_device(void)
{
   uint8_t mag[4];
   write_le32(mag, HOSTFS_MAGIC);
   int transferred = 0;
   int ret = libusb_bulk_transfer(g_dev, 2, mag, sizeof(mag), &transferred, 1000);
   if (ret < 0)
   {
      printf("Failed to do magic init ... Error: %d", ret);
      goto error;
   }

   if (transferred == 4)
      return 0;

   puts("Didn't really transfer 4 bytes, wut ...");

error:
   return -1;
}

static void bulk_thread(void *dummy)
{
   (void)dummy;

   uint8_t buffer[512];

   usb_check_device();

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
         printf("Failed to do bulk with error: %d\n", ret);

         if (ret != LIBUSB_ERROR_TIMEOUT)
            goto error;
      }

      //printf("Transferred: %d\n", transferred);
      if (transferred < 4)
         continue;

      //for (int i = 0; i < transferred; i++)
      //   printf("0x%02x\n", buffer[i]);

      uint32_t code = read_le32(buffer + 0);

      switch (code)
      {
      case HOSTFS_MAGIC:
         //printf("HOSTFS_MAGIC\n");
         if (!handle_hello(g_dev))
            goto error;

         active = true;
         break;
      case ASYNC_MAGIC:
         //printf("ASYNC_MAGIC\n");
         if (!handle_async(g_dev))
            goto error;
         break;
      case BULK_MAGIC:
         //printf("BULK_MAGIC\n");
         if (!handle_bulk(g_dev, buffer, transferred))
            goto error;
         break;
      default:
         puts("Got other magic!");
      }
   }

   return;
error:
   g_thread_failed = true;
}

bool init_program(void)
{
   if (libusb_init(&g_ctx) < 0)
   {
      puts("libusb_init failed.");
      goto error;
   }

   g_dev = libusb_open_device_with_vid_pid(g_ctx, SONY_VID, REMOTE_PID);

   if (!g_dev)
   {
      puts("libusb_open_device_with_vid_pid failed, trying attempt 2...");

      g_dev = libusb_open_device_with_vid_pid(g_ctx, SONY_VID, REMOTE_PID2);

      if (!g_dev)
      {
         puts("libusb_open_device_with_vid_pid attempt 2 failed...");
         goto error;
      }
   }

   if (libusb_kernel_driver_active(g_dev, 0))
   {
#ifndef __WIN32__
      if (libusb_detach_kernel_driver(g_dev, 0) < 0)
      {
         puts("libusb_detach_kernel_driver failed.");
         goto error;
      }
#endif
   }

   if (libusb_set_configuration(g_dev, 1) < 0)
   {
      puts("libusb_set_configuration failed.");
      goto error;
   }

   if (libusb_claim_interface(g_dev, 0) < 0)
   {
      puts("libusb_claim_interface failed.");
      goto error;
   }

   for (uint32_t mode = 0; mode < 4; mode++)
      g_textures[mode] = SDL_CreateTexture(
          g_renderer,
          ((const uint32_t[]){
              SDL_PIXELFORMAT_RGB565,
              SDL_PIXELFORMAT_ARGB1555,
              SDL_PIXELFORMAT_ARGB4444,
              SDL_PIXELFORMAT_ARGB8888})[mode],
          SDL_TEXTUREACCESS_STREAMING,
          PSP_WIDTH,
          PSP_HEIGHT);

   g_thread_failed = false;
   g_thread_die = false;
   g_thread = sthread_create(bulk_thread, NULL);
   if (!g_thread)
      goto error;

   SDL_Init(SDL_INIT_VIDEO);

   SDL_Window *window = SDL_CreateWindow(
       "RJL-Client",
       SDL_WINDOWPOS_UNDEFINED,
       SDL_WINDOWPOS_UNDEFINED,
       PSP_WIDTH,
       PSP_HEIGHT,
       SDL_WINDOW_BORDERLESS);

   if (window == NULL)
   {
      puts("SDL_CreateWindow failed.");
      goto error;
   }

   g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

   if (g_renderer == NULL)
   {
      puts("SDL_CreateRenderer failed.");
      goto error;
   }

   return true;
error:
   deinit_program();
   return false;
}

void deinit_program(void)
{
   if (g_thread)
   {
      g_thread_die = true;
      sthread_join(g_thread);
      g_thread = NULL;
      g_thread_die = false;
      g_thread_failed = false;
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

   for (int32_t mode = 0; mode < 4; mode++)
      if (g_textures[mode] != NULL)
         SDL_DestroyTexture(g_textures[mode]);

   SDL_Quit();
}

void run_program(void)
{
   if (g_thread_failed)
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);

   // TODO: Employ a more "sane" scheme using conditional variables, etc.
   // slock_lock(g_lock);
   // memcpy(g_frame_buffer, g_frame, sizeof(g_frame));
   // slock_unlock(g_lock);

   // video_cb(g_frame_buffer, PSP_WIDTH, PSP_HEIGHT, PSP_WIDTH * sizeof(uint32_t));
   // No audio :(
   // TODO: Poll input here.
}
