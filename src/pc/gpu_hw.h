/* Hardware (OpenGL) PS1 renderer with internal upscaling (gpu_hw.c).
 *
 * The software GPU (gpu.c) keeps running at native resolution and stays the reference
 * VRAM (CPU readback, texture and CLUT data). Every drawing primitive is also queued here
 * and drawn by the GPU into an upscaled copy of VRAM, which is what gets displayed.
 * gpu.c / hle_gpu.c call these hooks; all of them do nothing while the renderer is off. */
#pragma once
#include "rt.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int x, y, r, g, b, u, v; } HwV;

extern int g_hw_on;                      /* hardware renderer active (scale >= 2) */

int  hw_set_scale(int scale);            /* 1 = off (software only); returns the scale in use */
int  hw_scale(void);
int  hw_max_scale(void);
void hw_invalidate(void);                /* VRAM replaced wholesale (reset, state load): resync */

/* drawing (called by gpu.c just before the software rasteriser draws the same thing) */
void hw_poly(const HwV *v, int nv, int gouraud, int textured, int raw, int semi, u32 tpage, u32 clut);
void hw_rect(int x, int y, int w, int h, int r, int g, int b, int textured, int raw, int semi, int u0, int v0, u32 clut);
void hw_line(HwV a, HwV b, int gouraud, int semi);

/* VRAM writes outside the rasteriser: call hw_pre_write before changing g_vram, then the
 * matching hook after */
void hw_pre_write(int x, int y, int w, int h);
void hw_upload(int x, int y, int w, int h);            /* CPU -> VRAM image */
void hw_fill(int x, int y, int w, int h, u16 color);   /* GP0 02 / ClearImage */
void hw_copy(int sx, int sy, int dx, int dy, int w, int h);   /* VRAM -> VRAM */

int  hw_debug_dump(const char *dir);     /* F10: scaled vs software VRAM images */

/* presentation: finish drawing and return the display area as an upscaled texture */
int  hw_display_texture(unsigned *tex, int *w, int *h, int show_vram);
int  hw_display_is_wide(void);           /* the last display texture was the wide canvas */
extern int g_ws_extra;                   /* widescreen: extra native pixels each side (0 = off) */

#ifdef __cplusplus
}
#endif
