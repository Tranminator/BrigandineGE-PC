/* Widescreen: which screens use the wide canvas (gpu_hw.c), and how wide it is.
 *
 * The hardware renderer mirrors the display buffers into a target 2E pixels wider; a screen shows
 * it only where the game really draws the extra columns (otherwise the sides would hold leftovers
 * of other screens). Other screens stay 4:3 in the middle. Software renderer (Native): always 4:3.
 *
 * Screens:
 *   battle map (overlay id 6): its tile map draw ran in the last frames: main func 8002727C (16x16
 *   tiles) or, zoomed in, main func 80027810 (polygons, from the view rectangle below). The tile one
 *   drops the columns outside x 0..319 (0x800A68D4 / 0x800A68D8): widened by E on each side. The
 *   unit / terrain panel below the map is 320 wide: the sides beside it are kept black (g_ws_mask_y).
 *   strategy map (overlay 1, id 5): its terrain draw (ov1 func 800C3D58) ran in the last frames. Its terrain is drawn by main func 80028B14 through
 *   PsyQ DivideFT4, which drops a polygon whose four corners are all outside the screen width given in
 *   its DIVPOLYGON4 (pih = 320): that width is widened by 2E here, so the sides get their polygons
 *   too (the 4:3 picture is unchanged: the GPU clips the extra ones in VRAM). What is drawn at all comes
 *   from the view rectangle below. */
#include "rt.h"
#include "hle.h"
#include "gpu_hw.h"
#include "settings.h"

#define OVL_ID_ADDR  0x800B6838u
#define WS_SMAP_BOX_PX 103   /* the strategy camera box half width at the camera depth (3.22 columns) */

int g_ws_screen;                   /* this frame's screen draws past its 320 pixels */
int g_ws_mask_y;                   /* > 0: the wide sides from this line down are blacked out (gpu_hw.c) */

static const int k_extra[] = { 0, 53, 32 };   /* off, 16:9 (426x240), 16:10 (384x240) */

/* The map renderers project a screen rectangle (x, y, w, h as s16, centred: -160, -120, 320, 240)
 * onto the map plane to find what to draw (main func 80029918 -> visible rect 0x800A6748). With
 * the wide canvas the rectangle is widened by E on each side, and put back on other screens. */
#define RAM_VIEW_RECT 0x800A6740u
static unsigned s_vb, s_terrain_seen = 0x80000000u;
void hook_f_ov1_800C3D58(Ctx *c) { (void)c; s_terrain_seen = s_vb; }   /* strategy map terrain draw */
static int s_view_wide;            /* E written into the view rectangle (0 = original) */
static unsigned s_tiles_seen = 0x80000000u;
static unsigned s_flat_seen = 0x80000000u;   /* the top-down view (16x16 tiles) */
void hook_f_main_8002727C(Ctx *c) { (void)c; s_tiles_seen = s_flat_seen = s_vb; }   /* battle map tile draw */
void hook_f_main_80027810(Ctx *c) { (void)c; if (rd32(OVL_ID_ADDR) == 6) s_tiles_seen = s_vb; }   /* the same, tilted */

/* battle map: the tile map's column clip (x min / max, s32), widened by E */
#define RAM_TILE_CLIP_X0 0x800A68D4u
#define RAM_TILE_CLIP_X1 0x800A68D8u
#define BATTLE_PANEL_Y   194          /* first line of the unit / terrain panel (below its top rule) */
static int s_clip_wide;
static void set_tile_clip(int e) {
    if (e == s_clip_wide) return;
    if ((s32)rd32(RAM_TILE_CLIP_X0) != -s_clip_wide || (s32)rd32(RAM_TILE_CLIP_X1) != 319 + s_clip_wide) {
        s_clip_wide = 0;                              /* the game set other values: leave them */
        return;
    }
    wr32(RAM_TILE_CLIP_X0, (u32)-e);
    wr32(RAM_TILE_CLIP_X1, (u32)(319 + e));
    s_clip_wide = e;
}

static void set_view_rect(int e) {
    if (e == s_view_wide) return;
    if ((s16)rd16(RAM_VIEW_RECT + 4) != 320 + 2 * s_view_wide) { s_view_wide = 0; if (!e) return; }
    if ((s16)rd16(RAM_VIEW_RECT + 4) != 320 + 2 * s_view_wide) return;   /* not the rectangle we know */
    wr16(RAM_VIEW_RECT + 0, (u16)(s16)(-160 - e));
    wr16(RAM_VIEW_RECT + 4, (u16)(320 + 2 * e));
    s_view_wide = e;
}

/* the battle map shown wide: E (the pointer may go that far past each side of the 320 pixels), else 0 */
int ws_battle_extra(void) { return g_ws_screen && rd32(OVL_ID_ADDR) == 6 ? s_clip_wide : 0; }

/* the game picture's width for window sizes: 320, or 320 + 2E with widescreen chosen */
int ws_picture_w(void) {
    int mode = g_settings.widescreen;
    return 320 + 2 * (mode >= 1 && mode <= 2 ? k_extra[mode] : 0);
}

void ws_frame(void) {
    int mode = g_settings.widescreen;
    g_ws_extra = g_hw_on && mode >= 1 && mode <= 2 ? k_extra[mode] : 0;
    g_ws_screen = 0;
    s_vb++;
    /* the strategy map is on screen when the game drew its terrain just now (from the first frame
     * after a load or a menu; UI flag bit 3 only comes once the quill takes input) */
    u32 ovl = rd32(OVL_ID_ADDR);
    if (g_ws_extra && ovl == 5 && s_vb - s_terrain_seen <= 2) g_ws_screen = 1;
    /* the battle map likewise: its tile map was drawn just now (zoomed in, every other frame: one
     * slow frame more is allowed) */
    int battle = g_ws_extra && ovl == 6 && s_vb - s_tiles_seen <= 3;
    if (battle) g_ws_screen = 1;
    g_ws_mask_y = battle ? BATTLE_PANEL_Y : 0;
    set_view_rect(g_ws_screen ? g_ws_extra : 0);
    set_tile_clip(battle ? g_ws_extra : 0);
}

/* DivideFT4(v0..v3, uv0..uv3, rgbc, prim, ot, DIVPOLYGON4 *div): on entry the 12th argument (the
 * DIVPOLYGON4) is at sp + 44; its pih (screen width used for clipping) is at +4 */
void hook_f_main_800566BC(Ctx *c) {
    if (!g_ws_screen || !g_ws_extra) return;
    u32 div = rd32(c->r[29] + 44);
    if ((div & 0xFF000000u) != 0x80000000u) return;
    u32 pih = rd32(div + 4);
    if (pih == 320) wr32(div + 4, 320 + 2 * (u32)g_ws_extra);
}

/* Battle camera follow (overlay func 800B95D0, replaced while the battle map is wide): the camera
 * moves once the cursor leaves a box around it. Its x bounds are -0x88000 and 0x47000 over the zoom
 * (0x800A67C8), i.e. screen x 54..266 at zoom 4096 (~1.024 px per unit): with the wide picture they are
 * widened by E pixels each side, so the cursor can reach the sides before the camera follows. The z
 * part is the original (its bounds come from the projection, main func 80029918). */
int hook_f_ov2_800B95D0(Ctx *c) {
    if (!g_ws_screen || rd32(OVL_ID_ADDR) != 6 || !s_clip_wide) return 0;
    u32 sp0 = c->r[29], sp = sp0 - 0x28, ra = c->r[31];
    wr16(sp + 0x10, (u16)(s16)-0x68); wr16(sp + 0x12, (u16)(s16)-0x40);
    wr16(sp + 0x14, 0xCF); wr16(sp + 0x16, (u16)(rd16(0x800E7A24) - 0x68));
    c->r[29] = sp; c->r[4] = sp + 0x10; c->r[5] = sp + 0x18; c->r[6] = 0x480;
    f_main_80029918(c);
    c->r[29] = sp0; c->r[31] = ra;
    /* z (screen up / down): as the game */
    s32 cz = (s32)rd32(0x800A6AB8), b0 = (s32)rd32(0x800A67B0);
    s32 v1 = (((s32)(s16)rd16(sp + 0x1A) - 0x20) << 8) + b0;
    if (cz < v1) wr32(0x800A67B0, (u32)(b0 + (cz - v1)));
    v1 += (s32)(s16)rd16(sp + 0x1E) << 8;
    if (v1 < cz) wr32(0x800A67B0, (u32)((s32)rd32(0x800A67B0) + (cz - v1)));
    /* x: the box widened by E pixels each side */
    s32 zoom = (s32)rd32(0x800A67C8);
    if (zoom == 0) return 1;
    s32 extra = s_clip_wide * 4000;                   /* E pixels in the bounds' units (x zoom 4096) */
    s32 lo = (-0x88000 - extra) / zoom, hi = (0x47000 + extra) / zoom;
    s32 cx = (s32)rd32(0x800A6AB4), ac = (s32)rd32(0x800A67AC);
    v1 = (lo << 8) + ac;
    if (cx < v1) wr32(0x800A67AC, (u32)(ac + (cx - v1)));
    ac = (s32)rd32(0x800A67AC);
    v1 = (hi << 8) + ac;
    if (v1 < cx) wr32(0x800A67AC, (u32)(ac + (cx - v1)));
    return 1;
}

/* Battle camera clamp (overlay func 800B96F4, replaced on the wide top-down battle map): the camera
 * stays inside the map. Its x limit is (view x + 160) / 3 + 160, made for the tilted view (beyond
 * the map edge it shows the sky and the map's side); top-down, nothing is drawn there, so the wide
 * picture would show leftovers: the limit becomes 160 + E, the wide picture's half width. The z part
 * is the original. The tilted view keeps the original. */
int hook_f_ov2_800B96F4(Ctx *c) {
    (void)c;
    if (!g_ws_screen || rd32(OVL_ID_ADDR) != 6 || !s_clip_wide || s_vb - s_flat_seen > 3) return 0;
    s32 vy = (s16)rd16(RAM_VIEW_RECT + 8 + 2), vh = (s16)rd16(RAM_VIEW_RECT + 8 + 6);   /* 0x800A6748 */
    s32 b0 = (s32)rd32(0x800A67B0);
    s32 zmin = (0x78 - (vy + 0x78) / 2) << 8;
    if (b0 < zmin) { b0 = zmin; wr32(0x800A67B0, (u32)b0); }
    s32 zmax = (0x42F - (vy + vh)) << 8;
    if (zmax < b0) wr32(0x800A67B0, (u32)zmax);
    s32 xmin = (160 + s_clip_wide) << 8, xmax = 0x41F00 - xmin;
    s32 ac = (s32)rd32(0x800A67AC);
    if (ac < xmin) { ac = xmin; wr32(0x800A67AC, (u32)ac); }
    if (xmax < ac) wr32(0x800A67AC, (u32)xmax);
    return 1;
}

/* hook_list "imm:" instructions: the battle map's sky / map side fills in the tilted view (main func
 * around 80027EC8) are cut and tested at the 4:3 screen edges, x -1 / 0 on the left and 319 / 320 on
 * the right, so with the map edge between 320 and 320 + E nothing filled the wide side. On the wide
 * battle map those constants move out by E. */
s32 ws_imm(u32 addr, s32 value) {
    if (!g_ws_screen) return value;
    u32 ovl = rd32(OVL_ID_ADDR);
    if (addr >= 0x800B0000u) {                        /* strategy map camera follow box (ov1) */
        if (ovl != 5 || !g_ws_extra) return value;
        /* the box is +-3.22 columns, ~103 px at the camera's depth (32 px a column): E px more */
        return (s32)((s64)value * (WS_SMAP_BOX_PX + g_ws_extra) / WS_SMAP_BOX_PX);
    }
    if (!s_clip_wide || ovl != 6) return value;
    return addr < 0x80028040u ? value - s_clip_wide : value + s_clip_wide;   /* left part, then right */
}

/* the screen shown wide where the pointer goes past the 320 pixels: E pixels each side, else 0 */
int ws_pointer_extra(void) {
    if (!g_ws_screen) return 0;
    u32 ovl = rd32(OVL_ID_ADDR);
    return ovl == 6 ? s_clip_wide : ovl == 5 ? g_ws_extra : 0;
}
