/* Widescreen: which screens use the wide canvas (gpu_hw.c), and how wide it is.
 *
 * The hardware renderer mirrors the display buffers into a target 2E pixels wider; a screen shows
 * it only where the game really draws the extra columns (otherwise the sides would hold leftovers
 * of other screens). Other screens stay 4:3 in the middle. Software renderer (Native): always 4:3.
 *
 * Screens:
 *   strategy map (overlay 1, id 5): its terrain draw (ov1 func 800C3D58) ran in the last frames. Its terrain is drawn by main func 80028B14 through
 *   PsyQ DivideFT4, which drops a polygon whose four corners are all outside the screen width given in
 *   its DIVPOLYGON4 (pih = 320): that width is widened by 2E here, so the sides get their polygons
 *   too (the 4:3 picture is unchanged: the GPU clips the extra ones in VRAM). What is drawn at all comes
 *   from the view rectangle below. */
#include "rt.h"
#include "gpu_hw.h"
#include "settings.h"

#define OVL_ID_ADDR  0x800B6838u

int g_ws_screen;                   /* this frame's screen draws past its 320 pixels */

static const int k_extra[] = { 0, 53, 32 };   /* off, 16:9 (426x240), 16:10 (384x240) */

/* The map renderers project a screen rectangle (x, y, w, h as s16, centred: -160, -120, 320, 240)
 * onto the map plane to find what to draw (main func 80029918 -> visible rect 0x800A6748). With
 * the wide canvas the rectangle is widened by E on each side, and put back on other screens. */
#define RAM_VIEW_RECT 0x800A6740u
static unsigned s_vb, s_terrain_seen = 0x80000000u;
void hook_f_ov1_800C3D58(Ctx *c) { (void)c; s_terrain_seen = s_vb; }   /* strategy map terrain draw */
static int s_view_wide;            /* E written into the view rectangle (0 = original) */

static void set_view_rect(int e) {
    if (e == s_view_wide) return;
    if ((s16)rd16(RAM_VIEW_RECT + 4) != 320 + 2 * s_view_wide) { s_view_wide = 0; if (!e) return; }
    if ((s16)rd16(RAM_VIEW_RECT + 4) != 320 + 2 * s_view_wide) return;   /* not the rectangle we know */
    wr16(RAM_VIEW_RECT + 0, (u16)(s16)(-160 - e));
    wr16(RAM_VIEW_RECT + 4, (u16)(320 + 2 * e));
    s_view_wide = e;
}

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
    if (g_ws_extra && rd32(OVL_ID_ADDR) == 5 && s_vb - s_terrain_seen <= 2) g_ws_screen = 1;
    set_view_rect(g_ws_screen ? g_ws_extra : 0);
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
