/* Mouse control.
 *
 * Everywhere: left click = Circle, right click = Cross, wheel = Up/Down, middle click = Triangle.
 *
 * Battle map: the hex under the mouse is computed from the game's own projection and the
 * game's cursor is placed on it directly (its world position, which the game turns into the
 * current hex every frame: unit panel, highlight and camera follow update by themselves),
 * so hovering highlights instantly and a click selects at once. The game still validates
 * every choice itself (a click on a hex outside the move range is refused as with a pad).
 *
 * The battle map is a tilted plane: hex (col,row) has world position (col, row - 0.5 for even
 * columns), and the screen position is a homography of (world - camera). The homography and
 * camera scale were fitted to the game's cursor-highlight quads (0.3 px rms) and the camera
 * comes from RAM. RAM addresses were found by diffing save states (see docs). */
#include "mouse.h"
#include "rt.h"
#include "gpu.h"
#include "settings.h"
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

/* screen = H * (world - camera), camera = (camX / KX, camZ / KZ) */
static const double H[3][3] = {
    { 3.279508843e+01, -1.024355928e+01, 1.469460342e+02 },
    { -2.234536182e-02, 1.473237364e+01, 1.304963405e+02 },
    { -9.194202619e-04, -6.299702326e-02, 1.0 },
};
static const double KX = 3.260455542e+01, KZ = 3.198126134e+01;

/* PS1 pad bits */
#define B_UP     (1u << 4)
#define B_RIGHT  (1u << 5)
#define B_DOWN   (1u << 6)
#define B_LEFT   (1u << 7)
#define B_TRI    (1u << 12)
#define B_CIRCLE (1u << 13)
#define B_CROSS  (1u << 14)

static int s_mx = -1, s_my = -1;           /* mouse, window points */
static int s_moved;                        /* mouse moved since the last update */
static int s_view_x, s_view_y, s_view_w, s_view_h;   /* image rect, drawable pixels */
static float s_px_per_pt = 1.0f;

static u16 s_pulse_bits;                   /* button pulse in progress */
static int s_pulse_frames, s_gap_frames;
static int s_queue_circle, s_queue_cross, s_queue_tri, s_queue_up, s_queue_down;

static int s_hover_col = -1, s_hover_row = -1;
static int s_click_pending;                /* Circle once the cursor sits on the clicked hex */

/* put the game's cursor on a hex (instant; the game derives everything else) */
static void place_cursor(int col, int row) {
    wr32(RAM_CUR_WX, (u32)(col << 13));
    wr32(RAM_CUR_WZ, (u32)((row << 13) + ((col & 1) ? 4096 : 0)));
}

void mouse_set_view(int x, int y, int w, int h, float px_per_pt) {
    s_view_x = x; s_view_y = y; s_view_w = w; s_view_h = h; s_px_per_pt = px_per_pt > 0 ? px_per_pt : 1.0f;
}

/* window point -> display pixel (0..disp_w, 0..disp_h); 0 if outside the image */
static int to_display(double *dx, double *dy) {
    if (s_mx < 0 || s_view_w <= 0 || s_view_h <= 0) return 0;
    double px = s_mx * s_px_per_pt - s_view_x, py = s_my * s_px_per_pt - s_view_y;
    if (px < 0 || py < 0 || px >= s_view_w || py >= s_view_h) return 0;
    *dx = px * g_gpu.disp_w / s_view_w;
    *dy = py * g_gpu.disp_h / s_view_h;
    return 1;
}

static int battle_cursor_active(void) {
    if (!g_settings.mouse) return 0;
    if (rd32(OVL_ID_ADDR) != 6) return 0;
    if (rd32(RAM_SCREEN) != 2) return 0;
    u32 mode = rd32(RAM_INPUT_MODE);
    return mode == 0 || mode == 2;
}

/* display pixel -> hex (col,row) on the battle map */
static int hex_at(double sx, double sy, int *col, int *row) {
    /* invert the homography: solve H * (wx, wz, 1)^T ~ (sx, sy, 1) */
    double a = H[0][0] - sx * H[2][0], b = H[0][1] - sx * H[2][1], c = sx * H[2][2] - H[0][2];
    double d = H[1][0] - sy * H[2][0], e = H[1][1] - sy * H[2][1], f = sy * H[2][2] - H[1][2];
    double det = a * e - b * d;
    if (fabs(det) < 1e-9) return 0;
    double wx = (c * e - b * f) / det, wz = (a * f - c * d) / det;
    wx += (s32)rd32(RAM_CAM_X) / KX;
    wz += (s32)rd32(RAM_CAM_Z) / KZ;
    /* nearest hex centre; hexes are 1 apart in both axes, so x distances count 0.866 */
    double best = 1e9; int bc = 0, br = 0;
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

int mouse_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_MOUSEMOTION:
        s_mx = e->motion.x; s_my = e->motion.y; s_moved = 1;
        return 0;
    case SDL_MOUSEBUTTONDOWN:
        if (!g_settings.mouse) return 0;
        s_mx = e->button.x; s_my = e->button.y;
        if (e->button.button == SDL_BUTTON_LEFT) {
            double dx, dy; int col, row;
            if (battle_cursor_active() && to_display(&dx, &dy) && hex_at(dx, dy, &col, &row)) {
                place_cursor(col, row); s_hover_col = col; s_hover_row = row; s_click_pending = 3;
            } else s_queue_circle++;
        } else if (e->button.button == SDL_BUTTON_RIGHT) {
            s_click_pending = 0; s_queue_cross++;
        } else if (e->button.button == SDL_BUTTON_MIDDLE) s_queue_tri++;
        return 1;
    case SDL_MOUSEWHEEL:
        if (!g_settings.mouse) return 0;
        if (e->wheel.y > 0) s_queue_up++;
        else if (e->wheel.y < 0) s_queue_down++;
        return 1;
    }
    return 0;
}

void mouse_cancel(void) { s_click_pending = 0; }

static void start_pulse(u16 bits) { s_pulse_bits = bits; s_pulse_frames = 6; s_gap_frames = 2; }

void mouse_update(void) {
    if (s_pulse_frames > 0) { if (--s_pulse_frames == 0) s_pulse_bits = 0; return; }
    if (s_gap_frames > 0) { s_gap_frames--; return; }

    int active = battle_cursor_active();
    if (!active) { s_click_pending = 0; s_hover_col = -1; }

    if (active && s_click_pending) {                /* give the game a frame to take the new hex */
        if (--s_click_pending == 0 && rd8(RAM_CUR_COL) == s_hover_col && rd8(RAM_CUR_ROW) == s_hover_row)
            start_pulse(B_CIRCLE);
        s_moved = 0;
        return;
    }

    /* hover: the cursor jumps to the hex under the mouse whenever the mouse moves to a new hex */
    if (active && s_moved) {
        double dx, dy; int col, row;
        if (to_display(&dx, &dy) && hex_at(dx, dy, &col, &row) && (col != s_hover_col || row != s_hover_row ||
                                                                   col != rd8(RAM_CUR_COL) || row != rd8(RAM_CUR_ROW))) {
            place_cursor(col, row);
            s_hover_col = col; s_hover_row = row;
            if (getenv("BRIG_TRACE")) rt_log("[mouse] hover -> hex %d,%d (display %.0f,%.0f)", col, row, dx, dy);
        }
    }
    s_moved = 0;

    if (s_queue_cross) { s_queue_cross--; start_pulse(B_CROSS); }
    else if (s_queue_circle) { s_queue_circle--; start_pulse(B_CIRCLE); }
    else if (s_queue_tri) { s_queue_tri--; start_pulse(B_TRI); }
    else if (s_queue_up) { s_queue_up--; start_pulse(B_UP); }
    else if (s_queue_down) { s_queue_down--; start_pulse(B_DOWN); }
}

u16 mouse_pad_bits(void) { return s_pulse_bits; }
