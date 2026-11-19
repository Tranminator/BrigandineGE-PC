#pragma once
#include "rt.h"
#include "recomp_funcs.h"

/* platform.c */
int  platform_init(const char *title);
void platform_vblank(void);       /* present + events + 60 Hz pacing */
u16  platform_pad_buttons(void);  /* PS1 layout, active-high */
int  platform_quit_requested(void);
void platform_set_movie_frame(const u8 *rgb, int w, int h); /* NULL = back to VRAM */
void platform_apply_video(void);  /* VSync setting changed */

/* disc.c */
int  disc_open(const char *bin_path);
int  disc_read_sector(u32 lba, u8 *dst2048);
int  disc_read_raw(u32 lba, u8 *dst2352);
int  disc_find_file(const char *path, u32 *lba, u32 *size);

/* hle_etc.c */
void hle_vblank_tick(Ctx *c);
extern u32 g_vblank_count;
