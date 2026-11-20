/* Mouse control.
 *
 * Everywhere: left click = Circle, right click = Cross, wheel = Up/Down, middle click = Triangle.
 *
 * Battle map: the hex under the mouse is computed from the game's own projection, and the
 * game's cursor is walked there with virtual d-pad presses (one hex per press, checked
 * against RAM), so the game reacts exactly as to a pad: unit panel, camera scroll, range
 * limits. A click confirms (Circle) once the cursor has arrived.
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
#define RAM_CUR_COL     0x800E7C48u   /* current; 0x800E7C49..4C hold the previous 4 positions */
#define RAM_CUR_ROW     0x800E7C50u
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

static int s_has_target, s_tcol, s_trow, s_confirm_on_arrival;
static int s_step_wait, s_settle, s_retries, s_last_col = -1, s_last_row = -1;

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
    if (bc < 0 || br < 0 || bc > 255 || br > 255) return 0;
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
                s_has_target = 1; s_tcol = col; s_trow = row; s_confirm_on_arrival = 1; s_step_wait = 0;
            } else s_queue_circle++;
        } else if (e->button.button == SDL_BUTTON_RIGHT) {
            s_has_target = 0; s_confirm_on_arrival = 0; s_queue_cross++;
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

void mouse_cancel(void) { s_has_target = 0; s_confirm_on_arrival = 0; }

static void start_pulse(u16 bits) { s_pulse_bits = bits; s_pulse_frames = 6; s_gap_frames = 2; }

void mouse_update(void) {
    if (s_pulse_frames > 0) { if (--s_pulse_frames == 0) s_pulse_bits = 0; return; }
    if (s_gap_frames > 0) { s_gap_frames--; return; }

    int active = battle_cursor_active();
    if (!active) s_has_target = 0;

    /* hover: the cursor follows the mouse on the battle map */
    if (active && s_moved && !s_confirm_on_arrival) {
        double dx, dy; int col, row;
        if (to_display(&dx, &dy) && hex_at(dx, dy, &col, &row) &&
            (!s_has_target || col != s_tcol || row != s_trow)) {
            s_has_target = 1; s_tcol = col; s_trow = row; s_step_wait = 0; s_retries = 0;
            if (getenv("BRIG_TRACE")) rt_log("[mouse] hover -> hex %d,%d (display %.0f,%.0f), cursor %d,%d", col, row, dx, dy,
                                             rd8(RAM_CUR_COL), rd8(RAM_CUR_ROW));
        }
    }
    s_moved = 0;

    if (s_has_target) {
        int col = rd8(RAM_CUR_COL), row = rd8(RAM_CUR_ROW);
        if (s_settle > 0) { s_settle--; return; }    /* let the cursor slide finish */
        if (s_step_wait > 0) {                       /* waiting for the last press to move the cursor */
            if (col != s_last_col || row != s_last_row) { s_step_wait = 0; s_retries = 0; s_settle = 4; return; }
            if (--s_step_wait > 0) return;
            if (++s_retries >= 4) {                  /* stuck: map edge / out of range */
                if (getenv("BRIG_TRACE")) rt_log("[mouse] cursor stuck at %d,%d (target %d,%d)", col, row, s_tcol, s_trow);
                s_has_target = 0; s_confirm_on_arrival = 0; s_retries = 0;
                return;
            }
        }
        if (col == s_tcol && row == s_trow) {
            s_has_target = 0; s_retries = 0;
            if (getenv("BRIG_TRACE")) rt_log("[mouse] cursor arrived at %d,%d%s", col, row, s_confirm_on_arrival ? " -> Circle" : "");
            if (s_confirm_on_arrival) { s_confirm_on_arrival = 0; start_pulse(B_CIRCLE); }
            return;
        }
        u16 dir = col < s_tcol ? B_RIGHT : col > s_tcol ? B_LEFT : row < s_trow ? B_DOWN : B_UP;
        s_last_col = col; s_last_row = row; s_step_wait = 10;
        start_pulse(dir);
        return;
    }

    if (s_queue_cross) { s_queue_cross--; start_pulse(B_CROSS); }
    else if (s_queue_circle) { s_queue_circle--; start_pulse(B_CIRCLE); }
    else if (s_queue_tri) { s_queue_tri--; start_pulse(B_TRI); }
    else if (s_queue_up) { s_queue_up--; start_pulse(B_UP); }
    else if (s_queue_down) { s_queue_down--; start_pulse(B_DOWN); }
}

u16 mouse_pad_bits(void) { return s_pulse_bits; }
