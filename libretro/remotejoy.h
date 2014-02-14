#ifndef _REMOTE_JOY_H
#define _REMOTE_JOY_H

#include "libretro.h"

#define PSP_WIDTH 480
#define PSP_HEIGHT 272

// Magic stuff from RemoteJoy-Lite/Win32.
#define SONY_VID 0x054c
#define REMOTE_PID 0x01c9
#define REMOTE_PID2 0x02d2

extern retro_log_printf_t log_cb;
extern retro_environment_t environ_cb;
extern retro_video_refresh_t video_cb;

bool init_program(void);
void deinit_program(void);
void run_program(void);

#endif
