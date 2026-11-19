#pragma once
#include "rt.h"

#define VRAM_W 1024
#define VRAM_H 512

typedef struct {
    int area_x0, area_y0, area_x1, area_y1;
    int ofs_x, ofs_y;
    u32 texpage;
    int semi_mode, flip_x, flip_y;
    int tw_mw, tw_mh, tw_ox, tw_oy;
    int mask_set, mask_check;
    int disp_x, disp_y, disp_w, disp_h, disp_24bit, disp_enabled;
} GpuState;

extern u16 g_vram[VRAM_W * VRAM_H];
extern GpuState g_gpu;

void gpu_reset(void);
int  gp0_len(u32 cmd);
void gpu_gp0_packet(const u32 *w, int n);
void gpu_gp0_stream(const u32 *w, int n);
