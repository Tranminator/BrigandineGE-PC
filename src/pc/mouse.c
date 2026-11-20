/* Mouse control.
 *
 * One pointer: the mouse is captured (relative mode) and its movement, times a speed setting,
 * drives the game's own cursor; no second pointer is drawn. On the battle map the pointer is
 * anchored to the map (world coordinates): when the game moves its cursor by itself (menu closed,
 * keyboard / pad, camera scroll) the pointer follows it, so mouse and cursor never disagree.
 * In the battle menus the pointer moves between the items, starting at the highlighted one.
 * The mouse is released while the settings window is open or the game window is not focused;
 * without capture the OS pointer is used directly (and warped onto the cursor).
 *
 * Battle map (overlay 6, map screen):
 *   - free cursor / choosing a hex: the game's cursor is placed on the hex under the pointer
 *     (by writing its world position; the game derives hex, unit panel, highlight and camera
 *     itself). A click selects the hex; the game still validates every choice. Pushing the
 *     pointer against a screen edge scrolls: the game's camera follows its cursor.
 *   - command menu / system menu (the game's generic list menu): moving the mouse up / down
 *     changes the item, a click chooses it, right click closes the menu.
 * Everywhere else: left = Circle, right = Cross, middle = Triangle, wheel = Up/Down.
 *
 * The battle map is a tilted plane: hex (col,row) has world position (col, row - 0.5 for even
 * columns) and the screen position is a homography of (world - camera), fitted to the game's
 * cursor-highlight quads (0.3 px rms); the camera comes from RAM. Addresses found by diffing
 * save states and reading the recompiled overlay code. */
#include "mouse.h"
#include "rt.h"
#include "gpu.h"
#include "settings.h"
#include "ui.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>

/* battle RAM */
#define OVL_ID_ADDR     0x800B6838u   /* overlay id: 6 = battle */
#define RAM_CUR_COL     0x800E7C48u   /* current hex; 0x800E7C49..4C hold the previous 4 positions */
#define RAM_CUR_ROW     0x800E7C50u
#define RAM_CUR_WX      0x800A6AB4u   /* cursor world position, 8192 per column (s32) */
#define RAM_CUR_WZ      0x800A6AB8u   /* 8192 per row, +4096 on odd columns */
#define MAP_SIZE        32            /* every battle map is a 32x32 hex grid */
#define RAM_CAM_X       0x800A6794u   /* s32 */
#define RAM_CAM_Z       0x800A6798u   /* s32 */
#define RAM_INPUT_MODE  0x800A6994u   /* 0 free cursor, 1 menu open, 2 choosing a hex */
#define RAM_SCREEN      0x800A69DCu   /* 2 = battle map (3 = unit status screen) */
/* generic list menu (main EXE, $gp-relative) */
#define RAM_MENU_INDEX  0x800A6950u   /* selected item */
#define RAM_MENU_LAST   0x800A69E0u   /* index of the last item, -1 = no menu */
#define RAM_MENU_TYPE   0x800A69A4u   /* 0 = battle system menu, 1 = battle command menu */
/* both battle menus: items stacked at the top right */
#define MENU_X0 224.0
#define MENU_X1 306.0
#define MENU_Y0 19.5                  /* top of item 0 */
#define MENU_DY 19.2                  /* item pitch */
#define PANEL_Y 198.0                 /* the unit / terrain panel starts here (not map) */

/* screen = H * (world - camera), camera = (camX / KX, camZ / KZ) */
static const double H[3][3] = {
    { 3.279508843e+01, -1.024355928e+01, 1.469460342e+02 },
    { -2.234536182e-02, 1.473237364e+01, 1.304963405e+02 },
    { -9.194202619e-04, -6.299702326e-02, 1.0 },
};
static const double KX = 3.260455542e+01, KZ = 3.198126134e+01;

/* PS1 pad bits */
#define B_UP     (1u << 4)
#define B_DOWN   (1u << 6)
#define B_TRI    (1u << 12)
#define B_CIRCLE (1u << 13)
#define B_CROSS  (1u << 14)

static double s_vx = -1, s_vy = -1;        /* pointer, display pixels (-1 = unknown) */
static int s_moved;                        /* pointer moved since the last update */
static int s_captured;
static int s_view_x, s_view_y, s_view_w, s_view_h;   /* image rect, drawable pixels */
static float s_px_per_pt = 1.0f;

static u16 s_pulse_bits;                   /* button pulse in progress */
static int s_pulse_frames, s_gap_frames;
static int s_left, s_right, s_mid, s_wheel_up, s_wheel_down;   /* queued clicks */

static double s_acc_x, s_acc_y;            /* captured movement not applied yet, display pixels */
static int s_abs;                          /* OS pointer moved (not captured): s_vx/s_vy is absolute */
static int s_last_mode = -1, s_last_ctx = -1;
static int s_confirm_wait, s_confirm_col, s_confirm_row, s_confirm_menu = -1;
static double s_wx, s_wz;                  /* map pointer, world coordinates */
static int s_map_valid;
static u32 s_wr_x, s_wr_z;                 /* world position last written (or seen) */
static int s_cool;                         /* frames to ignore movement after a click */
static double s_menu_y;                    /* menu pointer, display y */
static int s_menu_valid;

void mouse_set_view(int x, int y, int w, int h, float px_per_pt) {
    s_view_x = x; s_view_y = y; s_view_w = w; s_view_h = h; s_px_per_pt = px_per_pt > 0 ? px_per_pt : 1.0f;
}

static double disp_w(void) { return g_gpu.disp_w > 0 ? g_gpu.disp_w : 320; }
static double disp_h(void) { return g_gpu.disp_h > 0 ? g_gpu.disp_h : 240; }

/* window point <-> display pixel */
static int pt_to_display(double x, double y, double *dx, double *dy) {
    if (s_view_w <= 0 || s_view_h <= 0) return 0;
    double px = x * s_px_per_pt - s_view_x, py = y * s_px_per_pt - s_view_y;
    *dx = px * disp_w() / s_view_w; *dy = py * disp_h() / s_view_h;
    return px >= 0 && py >= 0 && px < s_view_w && py < s_view_h;
}
static void display_to_pt(double dx, double dy, float *x, float *y) {
    *x = (float)((s_view_x + dx * s_view_w / disp_w()) / s_px_per_pt);
    *y = (float)((s_view_y + dy * s_view_h / disp_h()) / s_px_per_pt);
}

int mouse_cursor_pos(float *x, float *y) {
    /* the game's own cursor / menu highlight is the pointer everywhere handled so far */
    (void)x; (void)y;
    return 0;
}

/* Map pointer <-> the game's cursor world position (8192 per hex). Our world z is the hex row
 * minus 0.5 on even columns; the game stores z + 0.5. The game derives the current hex as
 * col = (x + 4096) >> 13, row = (z [+ 4096 on even columns]) >> 13, and draws the highlight
 * at the exact position, so writing a continuous position makes it glide. */
static void write_world(double wx, double wz) {
    s_wr_x = (u32)(s32)lround(wx * 8192.0);
    s_wr_z = (u32)(s32)lround((wz + 0.5) * 8192.0);
    wr32(RAM_CUR_WX, s_wr_x); wr32(RAM_CUR_WZ, s_wr_z);
}
static void game_hex(u32 gx, u32 gz, int *col, int *row) {
    *col = ((s32)gx + 4096) >> 13;
    *row = (*col & 1) ? ((s32)gz >> 13) : (((s32)gz + 4096) >> 13);
}

static void camera(double *cx, double *cz) { *cx = (s32)rd32(RAM_CAM_X) / KX; *cz = (s32)rd32(RAM_CAM_Z) / KZ; }

/* display pixel <-> world position on the battle map */
static int screen_to_world(double sx, double sy, double *wx, double *wz) {
    double a = H[0][0] - sx * H[2][0], b = H[0][1] - sx * H[2][1], c = sx * H[2][2] - H[0][2];
    double d = H[1][0] - sy * H[2][0], e = H[1][1] - sy * H[2][1], f = sy * H[2][2] - H[1][2];
    double det = a * e - b * d;
    if (fabs(det) < 1e-9) return 0;
    double cx, cz; camera(&cx, &cz);
    *wx = (c * e - b * f) / det + cx; *wz = (a * f - c * d) / det + cz;
    return 1;
}
static void world_to_screen(double wx, double wz, double *sx, double *sy) {
    double cx, cz; camera(&cx, &cz);
    double x = wx - cx, z = wz - cz;
    double w = H[2][0] * x + H[2][1] * z + H[2][2];
    *sx = (H[0][0] * x + H[0][1] * z + H[0][2]) / w;
    *sy = (H[1][0] * x + H[1][1] * z + H[1][2]) / w;
}
static void hex_center(int col, int row, double *wx, double *wz) { *wx = col; *wz = row - ((col % 2 == 0) ? 0.5 : 0.0); }
static int menu_item_at(double x, double y) {   /* battle list menu item under the pointer, -1 none */
    if (x < MENU_X0 || x > MENU_X1 || y < MENU_Y0) return -1;
    int k = (int)floor((y - MENU_Y0) / MENU_DY);
    int last = (s32)rd32(RAM_MENU_LAST);
    return k >= 0 && k <= last ? k : -1;
}

/* ------------------------------------------------------------------ input events */
int mouse_event(const SDL_Event *e) {
    if (!g_settings.mouse) return 0;
    switch (e->type) {
    case SDL_MOUSEMOTION:
        if (s_captured) {
            /* display pixels per window point, times the speed setting */
            double k = (s_view_w > 0 ? disp_w() * s_px_per_pt / s_view_w : 1.0) * g_settings.mouse_speed / 100.0;
            s_acc_x += e->motion.xrel * k; s_acc_y += e->motion.yrel * k;
        } else {
            double dx, dy;
            pt_to_display(e->motion.x, e->motion.y, &dx, &dy);
            s_vx = dx; s_vy = dy; s_abs = 1;
        }
        s_moved = 1;
        return 0;
    case SDL_MOUSEBUTTONDOWN:
        if (!s_captured) { double dx, dy; pt_to_display(e->button.x, e->button.y, &dx, &dy); s_vx = dx; s_vy = dy; s_abs = 1; }
        if (e->button.button == SDL_BUTTON_LEFT) s_left++;
        else if (e->button.button == SDL_BUTTON_RIGHT) s_right++;
        else if (e->button.button == SDL_BUTTON_MIDDLE) s_mid++;
        return 1;
    case SDL_MOUSEWHEEL:
        if (e->wheel.y > 0) s_wheel_up++;
        else if (e->wheel.y < 0) s_wheel_down++;
        return 1;
    }
    return 0;
}

void mouse_cancel(void) { s_confirm_wait = 0; }

static void start_pulse(u16 bits) { s_pulse_bits = bits; s_pulse_frames = 6; s_gap_frames = 2; }

/* pointer capture follows focus and the settings window */
static void update_capture(void) {
    if (getenv("BRIG_FAKE_CAPTURE")) { s_captured = 1; return; }   /* tests: behave as captured */
    SDL_Window *win = SDL_GetKeyboardFocus();
    int want = g_settings.mouse && g_settings.mouse_capture && win && !ui_is_open() && !getenv("BRIG_NO_CAPTURE");
    if (want == s_captured) return;
    if (want) {
        s_acc_x = s_acc_y = 0;
        SDL_SetRelativeMouseMode(SDL_TRUE);
    } else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        if (win && s_vx >= 0) { float x, y; display_to_pt(s_vx, s_vy, &x, &y); SDL_WarpMouseInWindow(win, (int)x, (int)y); }
    }
    s_captured = want;
}

/* not captured: put the OS pointer on a display position */
static void warp_os_pointer(double dx, double dy) {
    SDL_Window *win = SDL_GetMouseFocus();
    if (s_captured || !win || getenv("BRIG_NO_CAPTURE")) return;
    float x, y; display_to_pt(dx, dy, &x, &y);
    SDL_WarpMouseInWindow(win, (int)x, (int)y);
}

void mouse_update(void) {
    update_capture();
    if (!g_settings.mouse) return;
    /* movement is taken every frame; whatever arrives while busy is dropped, never replayed later */
    double mvx = s_acc_x, mvy = s_acc_y;
    s_acc_x = s_acc_y = 0;
    int abs_move = s_abs; s_abs = 0;
    if (s_cool > 0) { s_cool--; s_moved = 0; mvx = mvy = 0; }
    if (s_pulse_frames > 0) { if (--s_pulse_frames == 0) s_pulse_bits = 0; s_moved = 0; return; }
    if (s_gap_frames > 0) { s_gap_frames--; s_moved = 0; return; }

    int battle = rd32(OVL_ID_ADDR) == 6 && rd32(RAM_SCREEN) == 2;
    int mode = battle ? (int)rd32(RAM_INPUT_MODE) : -1;
    int menu = battle && mode == 1 && rd32(RAM_MENU_TYPE) <= 1 && (s32)rd32(RAM_MENU_LAST) >= 0;
    int ctx = !battle ? 0 : menu ? 2 : (mode == 0 || mode == 2) ? 1 : 3;
    if (ctx != s_last_ctx || mode != s_last_mode) {   /* screen / mode changed: re-anchor the pointer */
        s_confirm_wait = 0; s_map_valid = 0; s_menu_valid = 0;
        s_last_ctx = ctx; s_last_mode = mode;
    }
    /* a click placed something: confirm once the game has taken it */
    if (s_confirm_wait > 0) {
        if (--s_confirm_wait == 0) {
            if (s_confirm_menu >= 0) { if ((s32)rd32(RAM_MENU_INDEX) == s_confirm_menu) start_pulse(B_CIRCLE); }
            else if (rd8(RAM_CUR_COL) == s_confirm_col && rd8(RAM_CUR_ROW) == s_confirm_row) start_pulse(B_CIRCLE);
        }
        s_left = 0; s_moved = 0;
        return;
    }

    if (s_right) { s_right = 0; s_left = 0; start_pulse(B_CROSS); return; }
    if (s_mid) { s_mid = 0; start_pulse(B_TRI); return; }

    if (ctx == 1) {                                   /* battle map: free cursor / choosing a hex */
        u32 gx = rd32(RAM_CUR_WX), gz = rd32(RAM_CUR_WZ);
        /* the game moved its cursor by itself (menu closed, keyboard / pad, unit moved) or we have no
         * anchor yet: the pointer continues from the game's cursor, pending movement is dropped */
        if (!s_map_valid || gx != s_wr_x || gz != s_wr_z) {
            s_wx = (s32)gx / 8192.0; s_wz = (s32)gz / 8192.0 - 0.5;
            s_wr_x = gx; s_wr_z = gz; s_map_valid = 1;
            double sx, sy; world_to_screen(s_wx, s_wz, &sx, &sy); warp_os_pointer(sx, sy);
            s_moved = 0;
        }
        if (s_moved) {
            double sx, sy;
            if (abs_move) { sx = s_vx; sy = s_vy; }
            else { world_to_screen(s_wx, s_wz, &sx, &sy); sx += mvx; sy += mvy; }
            /* stay on the map picture; pushing against an edge lets the camera follow */
            if (sx < 0) sx = 0;
            if (sx > disp_w() - 1) sx = disp_w() - 1;
            if (sy < 0) sy = 0;
            if (sy > PANEL_Y - 1) sy = PANEL_Y - 1;
            double wx, wz;
            if (screen_to_world(sx, sy, &wx, &wz)) {
                if (wx < 0) wx = 0;
                if (wz < 0) wz = 0;
                if (wx > MAP_SIZE - 1) wx = MAP_SIZE - 1;
                if (wz > MAP_SIZE - 1) wz = MAP_SIZE - 1;
                s_wx = wx; s_wz = wz;
                write_world(wx, wz);
            }
        }
        s_moved = 0;
        if (s_left) {                                 /* select the highlighted hex: snap to its centre */
            s_left = 0;
            int col, row; game_hex(s_wr_x, s_wr_z, &col, &row);
            if (col >= 0 && row >= 0 && col < MAP_SIZE && row < MAP_SIZE) {
                hex_center(col, row, &s_wx, &s_wz);
                write_world(s_wx, s_wz);
                s_confirm_col = col; s_confirm_row = row; s_confirm_menu = -1; s_confirm_wait = 2;
                s_cool = 20;
            }
            return;
        }
    } else if (ctx == 2) {                            /* battle list menu: the pointer walks the items */
        int last = (s32)rd32(RAM_MENU_LAST), cur = (s32)rd32(RAM_MENU_INDEX);
        if (!s_menu_valid) { s_menu_y = MENU_Y0 + (cur + 0.5) * MENU_DY; s_menu_valid = 1; }
        if (s_moved) {
            if (abs_move) s_menu_y = s_vy; else s_menu_y += mvy;
            double y0 = MENU_Y0 + 0.5, y1 = MENU_Y0 + (last + 1) * MENU_DY - 0.5;
            if (s_menu_y < y0) s_menu_y = y0;
            if (s_menu_y > y1) s_menu_y = y1;
            int k = menu_item_at((MENU_X0 + MENU_X1) / 2, s_menu_y);
            if (k >= 0 && k != cur) wr32(RAM_MENU_INDEX, (u32)k);
        }
        s_moved = 0;
        if (s_left) {
            s_left = 0;
            int k = (s32)rd32(RAM_MENU_INDEX);
            if (abs_move || !s_captured) {            /* OS pointer: the item under it, or close when outside */
                int kk = s_vx >= 0 ? menu_item_at(s_vx, s_vy) : -1;
                if (kk < 0) { start_pulse(B_CROSS); return; }
                k = kk; wr32(RAM_MENU_INDEX, (u32)k);
            }
            s_confirm_menu = k; s_confirm_wait = 2; s_cool = 20;
            return;
        }
    } else {                                          /* anywhere else: buttons */
        s_moved = 0;
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); return; }
    }
    if (s_wheel_up) { s_wheel_up--; start_pulse(B_UP); }
    else if (s_wheel_down) { s_wheel_down--; start_pulse(B_DOWN); }
}

u16 mouse_pad_bits(void) { return s_pulse_bits; }
