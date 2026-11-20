/* Mouse control.
 *
 * Pointer: by default the mouse is captured (relative mode) and the game keeps its own pointer
 * in PS1 display pixels, moved by the mouse delta times a speed setting, so a high-DPI mouse
 * stays usable on a 320x240 picture. The pointer is drawn by ui.cpp. It is released while the
 * settings window is open or the game window is not focused. Without capture the OS pointer is
 * used directly.
 *
 * Battle map (overlay 6, map screen):
 *   - free cursor / choosing a hex: the game's cursor is placed on the hex under the pointer
 *     (by writing its world position; the game derives hex, unit panel, highlight and camera
 *     itself). Hover follows only real pointer movement, so the cursor never jumps when a menu
 *     closes. A click selects the hex at once; the game still validates every choice.
 *     Holding the pointer at a screen edge scrolls: the cursor steps outward one hex at a time
 *     and the game's camera glides after it.
 *   - command menu / system menu (the game's generic list menu): hovering an item highlights it,
 *     clicking it chooses it, clicking elsewhere closes the menu.
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
#define EDGE    5.0                   /* edge-scroll band, display pixels */

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
static double s_motion;                    /* distance moved since the last mode change */
static int s_captured;
static int s_view_x, s_view_y, s_view_w, s_view_h;   /* image rect, drawable pixels */
static float s_px_per_pt = 1.0f;

static u16 s_pulse_bits;                   /* button pulse in progress */
static int s_pulse_frames, s_gap_frames;
static int s_left, s_right, s_mid, s_wheel_up, s_wheel_down;   /* queued clicks */

static int s_last_mode = -1, s_last_ctx = -1;
static int s_confirm_wait, s_confirm_col, s_confirm_row, s_confirm_menu = -1;
static int s_edge_timer;

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
    if (!s_captured || s_vx < 0) return 0;
    display_to_pt(s_vx, s_vy, x, y);
    return 1;
}

static void place_cursor(int col, int row) {   /* instant; the game derives everything else */
    wr32(RAM_CUR_WX, (u32)(col << 13));
    wr32(RAM_CUR_WZ, (u32)((row << 13) + ((col & 1) ? 4096 : 0)));
}

/* display pixel -> hex (col,row) on the battle map */
static int hex_at(double sx, double sy, int *col, int *row) {
    double a = H[0][0] - sx * H[2][0], b = H[0][1] - sx * H[2][1], c = sx * H[2][2] - H[0][2];
    double d = H[1][0] - sy * H[2][0], e = H[1][1] - sy * H[2][1], f = sy * H[2][2] - H[1][2];
    double det = a * e - b * d;
    if (fabs(det) < 1e-9) return 0;
    double wx = (c * e - b * f) / det, wz = (a * f - c * d) / det;
    wx += (s32)rd32(RAM_CAM_X) / KX;
    wz += (s32)rd32(RAM_CAM_Z) / KZ;
    double best = 1e9; int bc = 0, br = 0;    /* nearest centre; x distances count 0.866 */
    int c0 = (int)floor(wx + 0.5);
    for (int cc = c0 - 1; cc <= c0 + 1; cc++) {
        double off = (cc % 2 == 0) ? 0.5 : 0.0;
        int rr = (int)floor(wz + off + 0.5);
        for (int r2 = rr - 1; r2 <= rr + 1; r2++) {
            double ddx = (wx - cc) * 0.866, ddz = wz - (r2 - off);
            double dist = ddx * ddx + ddz * ddz;
            if (dist < best) { best = dist; bc = cc; br = r2; }
        }
    }
    if (bc < 0 || br < 0 || bc >= MAP_SIZE || br >= MAP_SIZE) return 0;
    *col = bc; *row = br;
    return 1;
}

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
    case SDL_MOUSEMOTION: {
        double ox = s_vx, oy = s_vy;
        if (s_captured) {
            /* display pixels per window point, times the speed setting */
            double k = (s_view_w > 0 ? disp_w() * s_px_per_pt / s_view_w : 1.0) * g_settings.mouse_speed / 100.0;
            if (s_vx < 0) { s_vx = disp_w() / 2; s_vy = disp_h() / 2; ox = s_vx; oy = s_vy; }
            s_vx += e->motion.xrel * k; s_vy += e->motion.yrel * k;
            if (s_vx < 0) s_vx = 0;
            if (s_vy < 0) s_vy = 0;
            if (s_vx > disp_w() - 1) s_vx = disp_w() - 1;
            if (s_vy > disp_h() - 1) s_vy = disp_h() - 1;
        } else {
            double dx, dy;
            pt_to_display(e->motion.x, e->motion.y, &dx, &dy);
            s_vx = dx; s_vy = dy;
        }
        if (ox >= 0) s_motion += fabs(s_vx - ox) + fabs(s_vy - oy);
        s_moved = 1;
        return 0;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (!s_captured) { double dx, dy; pt_to_display(e->button.x, e->button.y, &dx, &dy); s_vx = dx; s_vy = dy; }
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
    SDL_Window *win = SDL_GetKeyboardFocus();
    int want = g_settings.mouse && g_settings.mouse_capture && win && !ui_is_open() && !getenv("BRIG_NO_CAPTURE");
    if (want == s_captured) return;
    if (want) {
        int mx, my; SDL_GetMouseState(&mx, &my);
        double dx, dy;
        if (pt_to_display(mx, my, &dx, &dy)) { s_vx = dx; s_vy = dy; }
        else { s_vx = disp_w() / 2; s_vy = disp_h() / 2; }
        SDL_SetRelativeMouseMode(SDL_TRUE);
    } else {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        if (win && s_vx >= 0) { float x, y; display_to_pt(s_vx, s_vy, &x, &y); SDL_WarpMouseInWindow(win, (int)x, (int)y); }
    }
    s_captured = want;
}

/* one hex toward a screen edge (the game's own zig-zag for left/right) */
static void step_cursor(int dcol, int drow) {
    int col = rd8(RAM_CUR_COL), row = rd8(RAM_CUR_ROW);
    if (dcol) { row += (col & 1) ? 1 : -1; col += dcol; }
    row += drow;
    if (col < 0 || row < 0 || col >= MAP_SIZE || row >= MAP_SIZE) return;
    place_cursor(col, row);
}

void mouse_update(void) {
    update_capture();
    if (!g_settings.mouse) return;
    if (s_pulse_frames > 0) { if (--s_pulse_frames == 0) s_pulse_bits = 0; return; }
    if (s_gap_frames > 0) { s_gap_frames--; return; }

    int battle = rd32(OVL_ID_ADDR) == 6 && rd32(RAM_SCREEN) == 2;
    int mode = battle ? (int)rd32(RAM_INPUT_MODE) : -1;
    int menu = battle && mode == 1 && rd32(RAM_MENU_TYPE) <= 1 && (s32)rd32(RAM_MENU_LAST) >= 0;
    int ctx = !battle ? 0 : menu ? 2 : (mode == 0 || mode == 2) ? 1 : 3;
    if (ctx != s_last_ctx || mode != s_last_mode) {   /* screen / mode changed: hover waits for real movement */
        s_motion = 0; s_confirm_wait = 0; s_edge_timer = 0;
        s_last_ctx = ctx; s_last_mode = mode;
    }
    int armed = s_motion > 3.0;
    double x = s_vx, y = s_vy;
    int have = x >= 0;

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
        int col, row;
        int on_map = have && y < PANEL_Y && hex_at(x, y, &col, &row);
        if (s_left) {
            s_left = 0;
            if (on_map) {
                place_cursor(col, row);
                s_confirm_col = col; s_confirm_row = row; s_confirm_menu = -1; s_confirm_wait = 2;
            }
            return;
        }
        if (armed && s_moved && on_map && (col != rd8(RAM_CUR_COL) || row != rd8(RAM_CUR_ROW))) place_cursor(col, row);
        s_moved = 0;
        /* edge scrolling (game pointer only: an OS pointer can leave the window) */
        if (s_captured && have) {
            int dc = x < EDGE ? -1 : x > disp_w() - 1 - EDGE ? 1 : 0;
            int dr = y < EDGE ? -1 : y > disp_h() - 1 - EDGE ? 1 : 0;
            if (dc || dr) { if (++s_edge_timer >= 6) { s_edge_timer = 0; step_cursor(dc, dr); } }
            else s_edge_timer = 0;
        }
    } else if (ctx == 2) {                            /* battle list menu */
        int k = have ? menu_item_at(x, y) : -1;
        if (s_left) {
            s_left = 0;
            if (k >= 0) { wr32(RAM_MENU_INDEX, (u32)k); s_confirm_menu = k; s_confirm_wait = 2; }
            else start_pulse(B_CROSS);                /* click outside the menu closes it */
            return;
        }
        if (armed && s_moved && k >= 0 && (s32)rd32(RAM_MENU_INDEX) != k) wr32(RAM_MENU_INDEX, (u32)k);
        s_moved = 0;
    } else {                                          /* anywhere else: buttons */
        s_moved = 0;
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); return; }
    }
    if (s_wheel_up) { s_wheel_up--; start_pulse(B_UP); }
    else if (s_wheel_down) { s_wheel_down--; start_pulse(B_DOWN); }
}

u16 mouse_pad_bits(void) { return s_pulse_bits; }
