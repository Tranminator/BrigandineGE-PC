/* HLE for libgpu entry points that touch hardware. Pure helpers (SetPolyFT4, AddPrim,
 * SetDefDrawEnv, LoadTPage...) stay recompiled; LoadTPage/LoadClut reach LoadImage here. */
#include "gpu.h"
#include "hle.h"
#include <SDL.h>
#include <stdlib.h>

typedef struct { s16 x, y, w, h; } PsxRect;

static PsxRect get_rect(u32 a) {
    PsxRect r; memcpy(&r, ram_ptr(a), sizeof r); return r;
}

void hle_ResetGraph(Ctx *c) {
    if ((c->r[4] & 3) == 0 || (c->r[4] & 3) == 3) gpu_reset();
    c->r[2] = 0;
}
void hle_SetGraphDebug(Ctx *c) { c->r[2] = 0; }
void hle_SetDispMask(Ctx *c) { g_gpu.disp_enabled = c->r[4] & 1; }
void hle_DrawSync(Ctx *c) { c->r[2] = 0; }

int g_stat_otag, g_stat_pkts;
void hle_DrawOTag(Ctx *c) {
    u32 p = c->r[4] & 0xFFFFFF;
    int guard = 0;
    g_stat_otag++;
    {   /* debug: BRIG_SLOW_MS=<n> simulates a slow machine by spending n ms per drawn frame */
        static int slow = -1;
        if (slow < 0) { const char *e = getenv("BRIG_SLOW_MS"); slow = e ? atoi(e) : 0; }
        if (slow && !g_gpu_skip_draw) SDL_Delay((Uint32)slow);
    }
    while (p != 0xFFFFFF && guard++ < 0x100000) {
        u32 hdr = rd32(0x80000000u | p);
        int n = hdr >> 24;
        if (n) { gpu_gp0_stream((const u32 *)ram_ptr(p + 4), n); g_stat_pkts++; }
        p = hdr & 0xFFFFFF;
    }
}

void hle_DrawPrim(Ctx *c) {
    u32 p = c->r[4];
    int n = rd32(p) >> 24;
    if (n) gpu_gp0_stream((const u32 *)ram_ptr(p + 4), n);
}

void hle_PutDrawEnv(Ctx *c) {
    u32 e = c->r[4];
    PsxRect clip = get_rect(e);
    s16 ofs[2]; memcpy(ofs, ram_ptr(e + 8), 4);
    PsxRect tw = get_rect(e + 12);
    u16 tpage = (u16)rd16(e + 20);
    u8 isbg = (u8)rd8(e + 24);
    g_gpu.area_x0 = clip.x; g_gpu.area_y0 = clip.y;
    g_gpu.area_x1 = clip.x + clip.w - 1; g_gpu.area_y1 = clip.y + clip.h - 1;
    if (g_gpu.area_x1 > 1023) g_gpu.area_x1 = 1023;
    if (g_gpu.area_y1 > 511) g_gpu.area_y1 = 511;
    g_gpu.ofs_x = ofs[0]; g_gpu.ofs_y = ofs[1];
    g_gpu.tw_mw = (~(tw.w - 1) >> 3) & 31; g_gpu.tw_mh = (~(tw.h - 1) >> 3) & 31;
    if (tw.w == 0) g_gpu.tw_mw = 0;
    if (tw.h == 0) g_gpu.tw_mh = 0;
    g_gpu.tw_ox = (tw.x >> 3) & 31; g_gpu.tw_oy = (tw.y >> 3) & 31;
    u32 e1 = 0xE1000000u | tpage;
    gpu_gp0_packet(&e1, 1);
    if (isbg) {
        u32 fill[3] = { 0x02000000u | rd8(e + 25) | (rd8(e + 26) << 8) | (rd8(e + 27) << 16),
                        (u32)(clip.x & 0x3FF) | ((u32)(clip.y & 0x1FF) << 16),
                        (u32)(clip.w & 0x3FF) | ((u32)(clip.h & 0x1FF) << 16) };
        gpu_gp0_packet(fill, 3);
    }
    c->r[2] = e;
}

void hle_PutDispEnv(Ctx *c) {
    u32 e = c->r[4];
    PsxRect d = get_rect(e);
    g_gpu.disp_x = d.x; g_gpu.disp_y = d.y;
    g_gpu.disp_w = d.w ? d.w : 320; g_gpu.disp_h = d.h ? d.h : 240;
    g_gpu.disp_24bit = rd8(e + 17) & 1;
    c->r[2] = e;
}

void hle_LoadImage(Ctx *c) {
    PsxRect r = get_rect(c->r[4]);
    const u16 *src = (const u16 *)ram_ptr(c->r[5]);
    for (int j = 0; j < r.h; j++)
        for (int i = 0; i < r.w; i++)
            g_vram[((r.y + j) & 511) * VRAM_W + ((r.x + i) & 1023)] = src[j * r.w + i];
    c->r[2] = 0;
}

void hle_StoreImage(Ctx *c) {
    PsxRect r = get_rect(c->r[4]);
    u16 *dst = (u16 *)ram_ptr(c->r[5]);
    for (int j = 0; j < r.h; j++)
        for (int i = 0; i < r.w; i++)
            dst[j * r.w + i] = g_vram[((r.y + j) & 511) * VRAM_W + ((r.x + i) & 1023)];
    c->r[2] = 0;
}

void hle_MoveImage(Ctx *c) {
    PsxRect r = get_rect(c->r[4]);
    u32 cmd[4] = { 0x80000000u, (u32)(r.x & 1023) | ((u32)(r.y & 511) << 16),
                   (c->r[5] & 1023) | ((c->r[6] & 511) << 16), (u32)(u16)r.w | ((u32)(u16)r.h << 16) };
    gpu_gp0_packet(cmd, 4);
    c->r[2] = 0;
}

void hle_ClearImage(Ctx *c) {
    PsxRect r = get_rect(c->r[4]);
    u16 col = (u16)(((c->r[5] & 255) >> 3) | (((c->r[6] & 255) >> 3) << 5) | (((c->r[7] & 255) >> 3) << 10));
    for (int j = 0; j < r.h; j++)
        for (int i = 0; i < r.w; i++)
            g_vram[((r.y + j) & 511) * VRAM_W + ((r.x + i) & 1023)] = col;
    c->r[2] = 0;
}
