/* Software PS1 GPU: VRAM 1024x512x16bpp, GP0 command processing, rasterizer. */
#include "gpu.h"
#include "state.h"
#include "rt.h"
#include "gpu_hw.h"
#include <stdlib.h>
#include <stdio.h>

u16 g_vram[VRAM_W * VRAM_H];
GpuState g_gpu;
int g_gpu_skip_draw;
int g_gpu_dump;
extern int g_ws_mask_y;
void ws_rune_bg(void);   /* widescreen.c */

/* Wide picture, menu screens on the rune stone background: the game draws it as a 320x240 picture
 * from two raw sprites (256 wide at x 0, 64 wide at x 256). Its lit circle fades to black ~10 px
 * before the left edge but is cut at the right edge (still ~15 / 255 bright at x 319), which the wide
 * picture showed as a straight cut. The circle is completed on the right: 20 more columns of the
 * mirrored edge, tinted to the brightness of the circle's mirror image on the left. The rest of the sides is painted black with the picture (it is
 * drawn first each frame), as the picture's surroundings. */
static inline u16 texel(int u, int v, u32 tp, u32 clut);
/* the rune picture's average brightness (r + g + b, 0..93) around texel (u, v): 5 columns x 9 rows */
static int rune_lum(int u, int v, u32 tp, u32 clut) {
    int sum = 0, n = 0;
    for (int dv = -4; dv <= 4; dv++) {
        int vv = v + dv;
        if (vv < 0 || vv > 239) continue;
        for (int du = -2; du <= 2; du++) {
            if (u + du < 0) continue;                /* left of the picture */
            u16 t = texel(u + du, vv, tp, clut);
            sum += (t & 31) + ((t >> 5) & 31) + ((t >> 10) & 31); n++;
        }
    }
    return n ? sum * 16 / n : 0;                  /* x16: keeps precision for the ratios */
}
#define RUNE_FADE_W 20   /* the circle's left side fades out over its first ~20 columns */
#define RUNE_CX2    338  /* twice the circle's centre x (measured: 169) */
static struct { u32 tp, clut; int u0, valid; } s_rune_left;   /* the left sprite (drawn after the right one) */
static void rune_bg_sides(int x, int y, int ww, u32 col, int raw, int semi, int u0, u32 clut) {
    ws_rune_bg();
    int E = g_ws_extra;
    if (x == 0) {                                 /* left side: dark; keep where its texture is */
        hw_rect(-E, y, E, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        s_rune_left.tp = g_gpu.texpage & 0x1FF; s_rune_left.clut = clut; s_rune_left.u0 = u0; s_rune_left.valid = 1;
        return;
    }
    if (E > RUNE_FADE_W) hw_rect(320 + RUNE_FADE_W, y, E - RUNE_FADE_W, 240, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    int c = raw ? 128 : (int)(col & 255);         /* raw = unmodulated (128 = 1.0); else the fade colour */
    u32 tp = g_gpu.texpage & 0x1FF;
    /* mirrored about the last column's centre: that column (x 319, a few odd bluish texels) is not
     * repeated next to itself */
    int ue = u0 + ww - 2;
    /* The edge columns mirrored keep the stone's grain; each cell of the strip is tinted so that its
     * brightness follows the circle instead of the mirrored picture: the row's edge brightness at the
     * seam, fading to black outwards (the mirrored columns alone are brighter near the top and bottom,
     * where the circle bends, and made bumps). Brightness is the local average read from VRAM. */
    enum { NX = 5, NY = 48 };
    int col_[NX + 1][NY + 1];
    for (int j = 0; j <= NY; j++) {
        int v = j * 240 / NY;
        int le = rune_lum(ue, v, tp, clut);
        /* the circle is symmetric about x 169: x 320 + k is as bright as x 18 - k on the left (scaled so
         * the seam matches this row's edge) */
        int lx = RUNE_CX2 - 320, l0 = s_rune_left.valid ? rune_lum(s_rune_left.u0 + lx, v, s_rune_left.tp, s_rune_left.clut) : 0;
        for (int i = 0; i <= NX; i++) {
            int k = i * RUNE_FADE_W / NX, want;
            if (l0 > 0) {
                int lk = lx - k;
                int lv = lk < -2 ? 0 : rune_lum(s_rune_left.u0 + lk, v, s_rune_left.tp, s_rune_left.clut);
                want = (int)((s64)lv * le / l0);
                if (want > le) want = le;
            } else want = le * (NX - i) / NX;         /* no left side seen yet: a plain fade */
            if (i == NX) want = 0;
            int have = rune_lum(ue - k, v, tp, clut);
            int m = have > 0 ? c * want / have : 0;
            col_[i][j] = m > 255 ? 255 : m;
        }
    }
    for (int j = 0; j < NY; j++)
        for (int i = 0; i < NX; i++) {
            int x0 = 320 + i * RUNE_FADE_W / NX, x1 = 320 + (i + 1) * RUNE_FADE_W / NX;
            int ua = ue - i * RUNE_FADE_W / NX, ub = ue - (i + 1) * RUNE_FADE_W / NX;
            int v0 = j * 240 / NY, v1 = (j + 1) * 240 / NY;
            int a0 = col_[i][j], b0 = col_[i + 1][j], a1 = col_[i][j + 1], b1 = col_[i + 1][j + 1];
            HwV q[4] = { { x0, y + v0, a0, a0, a0, ua, v0 }, { x1, y + v0, b0, b0, b0, ub, v0 },
                         { x0, y + v1, a1, a1, a1, ua, v1 }, { x1, y + v1, b1, b1, b1, ub, v1 } };
            hw_poly(q, 4, 1, 1, 0, semi, tp, clut);
        }
}   /* widescreen.c: the wide battle map (its panel top + 2), else 0 */   /* debug: log every GP0 packet while set (BRIG_GPUDUMP=<vblank>) */

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void gpu_reset(void) {
    memset(g_vram, 0, sizeof g_vram);
    memset(&g_gpu, 0, sizeof g_gpu);
    hw_invalidate();
    g_gpu.area_x1 = VRAM_W - 1;
    g_gpu.area_y1 = VRAM_H - 1;
    g_gpu.disp_w = 320; g_gpu.disp_h = 240;
}

/* ------------------------------------------------------------------ pixel ops */
static inline u16 blend(u16 bg, u16 fg, int mode) {
    int br = bg & 31, bgc = (bg >> 5) & 31, bb = (bg >> 10) & 31;
    int fr = fg & 31, fgc = (fg >> 5) & 31, fb = (fg >> 10) & 31;
    int r, g, b;
    switch (mode) {
    case 0: r = (br + fr) >> 1; g = (bgc + fgc) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = br + fr; g = bgc + fgc; b = bb + fb; break;
    case 2: r = br - fr; g = bgc - fgc; b = bb - fb; break;
    default: r = br + (fr >> 2); g = bgc + (fgc >> 2); b = bb + (fb >> 2); break;
    }
    r = clampi(r, 0, 31); g = clampi(g, 0, 31); b = clampi(b, 0, 31);
    return (u16)(r | (g << 5) | (b << 10) | (fg & 0x8000));
}

static inline void plot(int x, int y, u16 c, int semi, int check_bit15) {
    if (x < g_gpu.area_x0 || x > g_gpu.area_x1 || y < g_gpu.area_y0 || y > g_gpu.area_y1) return;
    u16 *p = &g_vram[(y & 511) * VRAM_W + (x & 1023)];
    if (g_gpu.mask_check && (*p & 0x8000)) return;
    if (semi && (!check_bit15 || (c & 0x8000))) c = blend(*p, c, g_gpu.semi_mode);
    if (g_gpu.mask_set) c |= 0x8000;
    *p = c;
}

/* Texture fetch. tp = texpage bits (x base, y base, depth), clut = packed clut field. */
static inline u16 texel(int u, int v, u32 tp, u32 clut) {
    u &= 0xFF; v &= 0xFF;
    u = (u & ~(g_gpu.tw_mw * 8)) | ((g_gpu.tw_ox & g_gpu.tw_mw) * 8);
    v = (v & ~(g_gpu.tw_mh * 8)) | ((g_gpu.tw_oy & g_gpu.tw_mh) * 8);
    int bx = (tp & 15) * 64, by = ((tp >> 4) & 1) * 256, depth = (tp >> 7) & 3;
    int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
    if (depth == 0) {
        u16 w = g_vram[((by + v) & 511) * VRAM_W + ((bx + u / 4) & 1023)];
        int idx = (w >> ((u & 3) * 4)) & 15;
        return g_vram[cy * VRAM_W + ((cx + idx) & 1023)];
    } else if (depth == 1) {
        u16 w = g_vram[((by + v) & 511) * VRAM_W + ((bx + u / 2) & 1023)];
        int idx = (w >> ((u & 1) * 8)) & 255;
        return g_vram[cy * VRAM_W + ((cx + idx) & 1023)];
    }
    return g_vram[((by + v) & 511) * VRAM_W + ((bx + u) & 1023)];
}

static inline u16 modulate(u16 t, int r, int g, int b) {
    int tr = ((t & 31) * r) >> 7, tg = (((t >> 5) & 31) * g) >> 7, tb = (((t >> 10) & 31) * b) >> 7;
    return (u16)(clampi(tr, 0, 31) | (clampi(tg, 0, 31) << 5) | (clampi(tb, 0, 31) << 10) | (t & 0x8000));
}

/* PS1 dither matrix, added to 8-bit components before truncation to 5 bits */
static const s8 DITHER[4][4] = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };

static inline u16 rgb24to15_dither(int r, int g, int b, int x, int y) {
    int d = DITHER[y & 3][x & 3];
    r += d; g += d; b += d;
    return (u16)((clampi(r, 0, 255) >> 3) | ((clampi(g, 0, 255) >> 3) << 5) | ((clampi(b, 0, 255) >> 3) << 10));
}

/* texture * colour in 8-bit precision with dithering (texture-blended polygons) */
static inline u16 modulate_dither(u16 t, int r, int g, int b, int x, int y) {
    int d = DITHER[y & 3][x & 3];
    int tr = ((t & 31) << 3) * r / 128 + d, tg = (((t >> 5) & 31) << 3) * g / 128 + d, tb = (((t >> 10) & 31) << 3) * b / 128 + d;
    return (u16)((clampi(tr, 0, 255) >> 3) | ((clampi(tg, 0, 255) >> 3) << 5) | ((clampi(tb, 0, 255) >> 3) << 10) | (t & 0x8000));
}

static inline u16 rgb24to15(int r, int g, int b) {
    return (u16)((clampi(r, 0, 255) >> 3) | ((clampi(g, 0, 255) >> 3) << 5) | ((clampi(b, 0, 255) >> 3) << 10));
}

/* ------------------------------------------------------------------ triangles */
typedef struct { int x, y, r, g, b, u, v; } Vtx;

static void draw_tri(Vtx a, Vtx b, Vtx c, int gouraud, int textured, int raw, int semi, u32 tp, u32 clut) {
    int area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (area == 0) return;
    if (area < 0) { Vtx t = b; b = c; c = t; area = -area; }
    int minx = clampi(a.x < b.x ? (a.x < c.x ? a.x : c.x) : (b.x < c.x ? b.x : c.x), g_gpu.area_x0, g_gpu.area_x1);
    int maxx = clampi(a.x > b.x ? (a.x > c.x ? a.x : c.x) : (b.x > c.x ? b.x : c.x), g_gpu.area_x0, g_gpu.area_x1);
    int miny = clampi(a.y < b.y ? (a.y < c.y ? a.y : c.y) : (b.y < c.y ? b.y : c.y), g_gpu.area_y0, g_gpu.area_y1);
    int maxy = clampi(a.y > b.y ? (a.y > c.y ? a.y : c.y) : (b.y > c.y ? b.y : c.y), g_gpu.area_y0, g_gpu.area_y1);
    if (maxx - minx > 1023 || maxy - miny > 511) return; /* hardware rejects huge polys */
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            /* edge functions with top-left rule approximated by half-open test */
            int w0 = (c.x - b.x) * (y - b.y) - (c.y - b.y) * (x - b.x);
            int w1 = (a.x - c.x) * (y - c.y) - (a.y - c.y) * (x - c.x);
            int w2 = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            /* PS1 fill rule: pixels exactly on an edge are drawn only for top and left edges
             * (bottom/right edges excluded). With this winding an edge p->q is top-left when
             * dy < 0, or dy == 0 with dx > 0. */
#define TOPLEFT(p, q) (((q).y - (p).y) < 0 || (((q).y - (p).y) == 0 && ((q).x - (p).x) > 0))
            if ((w0 == 0 && !TOPLEFT(b, c)) || (w1 == 0 && !TOPLEFT(c, a)) || (w2 == 0 && !TOPLEFT(a, b))) continue;
#undef TOPLEFT
            int r = a.r, g = a.g, bl = a.b;
            if (gouraud) {
                r = (a.r * w0 + b.r * w1 + c.r * w2) / area;
                g = (a.g * w0 + b.g * w1 + c.g * w2) / area;
                bl = (a.b * w0 + b.b * w1 + c.b * w2) / area;
            }
            u16 px;
            if (textured) {
                int u = (a.u * w0 + b.u * w1 + c.u * w2) / area;
                int v = (a.v * w0 + b.v * w1 + c.v * w2) / area;
                px = texel(u, v, tp, clut);
                if (px == 0) continue;
                if (!raw) px = g_gpu.dither ? modulate_dither(px, r, g, bl, x, y) : modulate(px, r, g, bl);
                plot(x, y, px, semi, 1);
            } else {
                plot(x, y, (gouraud && g_gpu.dither) ? rgb24to15_dither(r, g, bl, x, y) : rgb24to15(r, g, bl), semi, 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ lines / rects */
static void draw_line(Vtx a, Vtx b, int gouraud, int semi) {
    int dx = abs(b.x - a.x), dy = abs(b.y - a.y);
    int n = dx > dy ? dx : dy;
    if (n > 1023) return;
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (b.x - a.x) * i / n : a.x;
        int y = n ? a.y + (b.y - a.y) * i / n : a.y;
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud && n) { r += (b.r - a.r) * i / n; g += (b.g - a.g) * i / n; bl += (b.b - a.b) * i / n; }
        plot(x, y, (gouraud && g_gpu.dither) ? rgb24to15_dither(r, g, bl, x, y) : rgb24to15(r, g, bl), semi, 0);
    }
}

static void draw_rect(int x, int y, int w, int h, int r, int g, int b, int textured, int raw, int semi,
                      int u0, int v0, u32 clut) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (textured) {
                int u = g_gpu.flip_x ? u0 - i : u0 + i;
                int v = g_gpu.flip_y ? v0 - j : v0 + j;
                u16 px = texel(u, v, g_gpu.texpage, clut);
                if (px == 0) continue;
                if (!raw) px = modulate(px, r, g, b);
                plot(x + i, y + j, px, semi, 1);
            } else {
                plot(x + i, y + j, rgb24to15(r, g, b), semi, 0);
            }
        }
    }
}

/* ------------------------------------------------------------------ GP0 */
static inline int sx11(u32 v) { return ((s32)(v << 21)) >> 21; }

static void set_texpage(u32 v) {
    g_gpu.texpage = v & 0x1FF;
    g_gpu.dither = (v >> 9) & 1;
    g_gpu.semi_mode = (v >> 5) & 3;
    g_gpu.flip_x = (v >> 12) & 1;
    g_gpu.flip_y = (v >> 13) & 1;
}

/* Number of words for a GP0 command (including the command word). 0 = variable/unknown. */
int gp0_len(u32 cmd) {
    u32 op = cmd >> 24;
    if (op >= 0x20 && op < 0x40) {
        int verts = (op & 8) ? 4 : 3, gour = (op >> 4) & 1, tex = (op >> 2) & 1;
        return 1 + verts * (1 + tex) + (gour ? verts - 1 : 0);
    }
    if (op >= 0x40 && op < 0x60) return (op & 8) ? 0 : ((op & 0x10) ? 4 : 3);
    if (op >= 0x60 && op < 0x80) {
        int n = 2 + ((op >> 2) & 1);
        if (((op >> 3) & 3) == 0) n++;
        return n;
    }
    if (op == 0x02) return 3;
    if (op == 0x80) return 4;
    if (op == 0xA0 || op == 0xC0) return 3;
    return 1;
}

void gpu_gp0_packet(const u32 *w, int n) {
    u32 op = w[0] >> 24;
    if (g_gpu_dump) {
        char line[256]; int k = snprintf(line, sizeof line, "[gp0] %02X tp=%03X semi=%d dith=%d |", op, g_gpu.texpage,
                                         g_gpu.semi_mode, g_gpu.dither);
        for (int i = 0; i < n && i < 12 && k < 240; i++) k += snprintf(line + k, sizeof line - k, " %08X", w[i]);
        rt_log("%s", line);
    }
    if (op == 0x00 || op == 0x01) return;
    if (g_gpu_skip_draw && (op == 0x02 || (op >= 0x20 && op < 0x80))) return;   /* frame skip */
    if (op == 0x02) {
        int x = (w[1] & 0x3F0), y = (w[1] >> 16) & 0x1FF;
        int ww = ((w[2] & 0x3FF) + 15) & ~15, hh = (w[2] >> 16) & 0x1FF;
        u16 c = rgb24to15(w[0] & 255, (w[0] >> 8) & 255, (w[0] >> 16) & 255);
        hw_pre_write(x, y, ww, hh);
        for (int j = 0; j < hh; j++)
            for (int i = 0; i < ww; i++) g_vram[((y + j) & 511) * VRAM_W + ((x + i) & 1023)] = c;
        hw_fill(x, y, ww, hh, c);
        return;
    }
    if (op >= 0x20 && op < 0x40) {
        int quad = (op >> 3) & 1, gour = (op >> 4) & 1, tex = (op >> 2) & 1, semi = (op >> 1) & 1, raw = op & 1;
        int nv = quad ? 4 : 3;
        Vtx v[4];
        u32 clut = 0, tp = g_gpu.texpage;
        int k = 0;
        u32 col = w[k++];
        for (int i = 0; i < nv; i++) {
            if (i > 0 && gour) col = w[k++];
            u32 xy = w[k++];
            v[i].r = col & 255; v[i].g = (col >> 8) & 255; v[i].b = (col >> 16) & 255;
            v[i].x = sx11(xy) + g_gpu.ofs_x; v[i].y = sx11(xy >> 16) + g_gpu.ofs_y;
            v[i].u = v[i].v = 0;
            if (tex) {
                u32 uv = w[k++];
                v[i].u = uv & 255; v[i].v = (uv >> 8) & 255;
                if (i == 0) clut = uv >> 16;
                if (i == 1) { tp = (uv >> 16) & 0x1FF; g_gpu.semi_mode = (uv >> 21) & 3; g_gpu.texpage = tp; }
            }
        }
        if (g_hw_on) {
            HwV hv[4];
            for (int i = 0; i < nv; i++) { hv[i].x = v[i].x; hv[i].y = v[i].y; hv[i].r = v[i].r; hv[i].g = v[i].g;
                                           hv[i].b = v[i].b; hv[i].u = v[i].u; hv[i].v = v[i].v; }
            hw_poly(hv, nv, gour, tex, raw, semi, tp, clut);
            /* ... and on to the right after its last piece (x 256..320, u 0..64): u 64..128 from x 320 */
            if (g_ws_mask_y && quad && tex && v[0].x == 256 && v[0].y % 240 == g_ws_mask_y - 2 && v[3].x == 320 &&
                v[3].y == v[0].y + 48 && v[0].u == 0 && v[1].u == 64)
                hw_rect(320, v[0].y, 64, 48, v[0].r, v[0].g, v[0].b, tex, raw, semi, 64, v[0].v, clut);
        }
        draw_tri(v[0], v[1], v[2], gour, tex, raw, semi, tp, clut);
        if (quad) draw_tri(v[1], v[2], v[3], gour, tex, raw, semi, tp, clut);
        return;
    }
    if (op >= 0x40 && op < 0x60) {
        int gour = (op >> 4) & 1, semi = (op >> 1) & 1;
        Vtx prev = {0};
        int k = 0, first = 1;
        u32 col = w[k++];
        while (k < n) {
            if (!first && gour) {
                if ((w[k] & 0xF000F000u) == 0x50005000u) break;
                col = w[k++];
            }
            if (k >= n) break;
            if (!first && (w[k] & 0xF000F000u) == 0x50005000u) break;
            Vtx cur = { sx11(w[k]) + g_gpu.ofs_x, sx11(w[k] >> 16) + g_gpu.ofs_y,
                        (int)(col & 255), (int)((col >> 8) & 255), (int)((col >> 16) & 255), 0, 0 };
            k++;
            if (!first) {
                if (g_hw_on) {
                    HwV ha = { prev.x, prev.y, prev.r, prev.g, prev.b, 0, 0 }, hb = { cur.x, cur.y, cur.r, cur.g, cur.b, 0, 0 };
                    hw_line(ha, hb, gour, semi);
                }
                draw_line(prev, cur, gour, semi);
            }
            prev = cur; first = 0;
            if (!(op & 8) && k >= (gour ? 4 : 3)) break;
        }
        return;
    }
    if (op >= 0x60 && op < 0x80) {
        int tex = (op >> 2) & 1, semi = (op >> 1) & 1, raw = op & 1, sz = (op >> 3) & 3;
        int k = 1;
        int x = sx11(w[k]) + g_gpu.ofs_x, y = sx11(w[k] >> 16) + g_gpu.ofs_y; k++;
        int u = 0, v = 0; u32 clut = 0;
        if (tex) { u = w[k] & 255; v = (w[k] >> 8) & 255; clut = w[k] >> 16; k++; }
        int ww, hh;
        if (sz == 0) { ww = w[k] & 0x3FF; hh = (w[k] >> 16) & 0x1FF; }
        else if (sz == 1) ww = hh = 1;
        else if (sz == 2) ww = hh = 8;
        else ww = hh = 16;
        hw_rect(x, y, ww, hh, w[0] & 255, (w[0] >> 8) & 255, (w[0] >> 16) & 255, tex, raw, semi, u, v, clut);
        /* wide battle map: the panel's stone background (128x48 tiles from x 0, in either display buffer)
         * goes on to the left */
        if (g_hw_on && g_ws_mask_y && tex && x == 0 && y % 240 == g_ws_mask_y - 2 && ww == 128 && hh == 48 && u == 0) {
            hw_rect(-128, y, 128, hh, w[0] & 255, (w[0] >> 8) & 255, (w[0] >> 16) & 255, tex, raw, semi, u, v, clut);
        }
        if (g_hw_on && g_ws_extra && tex && hh == 240 && y % 240 == 0 && v == 0 &&
            ((x == 0 && ww == 256) || (x == 256 && ww == 64)))
            rune_bg_sides(x, y, ww, w[0], raw, semi, u, clut);
        draw_rect(x, y, ww, hh, w[0] & 255, (w[0] >> 8) & 255, (w[0] >> 16) & 255, tex, raw, semi, u, v, clut);
        return;
    }
    if (op == 0x80) {
        int sx = w[1] & 1023, sy = (w[1] >> 16) & 511, dx = w[2] & 1023, dy = (w[2] >> 16) & 511;
        int ww = w[3] & 0xFFFF, hh = w[3] >> 16;
        ww = ww ? ww : 1024; hh = hh ? hh : 512;
        hw_pre_write(dx, dy, ww, hh);
        u16 *tmp = malloc((size_t)ww * hh * 2);
        for (int j = 0; j < hh; j++) for (int i = 0; i < ww; i++)
            tmp[j * ww + i] = g_vram[((sy + j) & 511) * VRAM_W + ((sx + i) & 1023)];
        for (int j = 0; j < hh; j++) for (int i = 0; i < ww; i++)
            g_vram[((dy + j) & 511) * VRAM_W + ((dx + i) & 1023)] = tmp[j * ww + i];
        free(tmp);
        hw_copy(sx, sy, dx, dy, ww, hh);
        return;
    }
    switch (op) {
    case 0xE1: set_texpage(w[0]); return;
    case 0xE2:
        g_gpu.tw_mw = w[0] & 31; g_gpu.tw_mh = (w[0] >> 5) & 31;
        g_gpu.tw_ox = (w[0] >> 10) & 31; g_gpu.tw_oy = (w[0] >> 15) & 31; return;
    case 0xE3: g_gpu.area_x0 = w[0] & 1023; g_gpu.area_y0 = (w[0] >> 10) & 511; return;
    case 0xE4: g_gpu.area_x1 = w[0] & 1023; g_gpu.area_y1 = (w[0] >> 10) & 511; return;
    case 0xE5: g_gpu.ofs_x = sx11(w[0]); g_gpu.ofs_y = sx11(w[0] >> 11); return;
    case 0xE6: g_gpu.mask_set = w[0] & 1; g_gpu.mask_check = (w[0] >> 1) & 1;
        if ((w[0] & 3) && getenv("BRIG_MASKTRACE")) rt_log("[gpu] E6 mask %u", w[0] & 3);
        return;
    }
}

/* Feed a raw word stream (e.g. an OT node body). */
void gpu_gp0_stream(const u32 *w, int n) {
    int i = 0;
    while (i < n) {
        u32 op = w[i] >> 24;
        int len = gp0_len(w[i]);
        if (op >= 0x40 && op < 0x60 && (op & 8)) {  /* polyline: until terminator */
            int j = i + 1;
            while (j < n && (w[j] & 0xF000F000u) != 0x50005000u) j++;
            len = j - i + (j < n ? 1 : 0);
        }
        if (op == 0xA0) { /* CPU->VRAM inline image */
            if (i + 3 > n) return;
            int x = w[i + 1] & 1023, y = (w[i + 1] >> 16) & 511, ww = w[i + 2] & 0xFFFF, hh = w[i + 2] >> 16;
            int words = (ww * hh + 1) / 2;
            const u16 *src = (const u16 *)&w[i + 3];
            hw_pre_write(x, y, ww, hh);
            for (int j = 0; j < hh; j++) for (int k = 0; k < ww; k++)
                g_vram[((y + j) & 511) * VRAM_W + ((x + k) & 1023)] = src[j * ww + k];
            hw_upload(x, y, ww, hh);
            i += 3 + words;
            continue;
        }
        if (i + len > n) len = n - i;
        gpu_gp0_packet(&w[i], len);
        i += len;
    }
}

/* save state */
void gpu_state(StateIO *s) {
    st_io(s, g_vram, sizeof g_vram);
    ST(s, g_gpu);
    if (s->load) { g_gpu_skip_draw = 0; hw_invalidate(); }
}
