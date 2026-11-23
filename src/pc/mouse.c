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
 *     pointer against a screen edge scrolls: the game's camera follows its cursor. A push that reaches
 *     the camera's follow box holds that D-pad direction (game speed) until the mouse moves back.
 *     Attack / spell targets ("Target Who?") use the same hex cursor.
 *   - command menu / system menu (the game's generic list menu): moving the mouse up / down
 *     changes the item, a click chooses it, right click closes the menu.
 * Icon menus, anywhere (battle commands, castle menu, Organize menu...): item boxes come from the
 * game's own item position table.
 * Strategy map: the quill follows the pointer (see smap_*), a click is Circle; pushing past the
 * camera box scrolls, and keeps scrolling at the game's speed (D-pad held) until the mouse moves back.
 * Organize grids (squads / monster stock): the pointer picks the cell.
 * Scrolling lists, anywhere (main EXE func 80021EB8: attacks, magic, skills, items...): a hook on the
 * recompiled function gives the item count; the pointer walks the items, pushing past the first /
 * last visible row (or the wheel) scrolls, a click chooses. The SUMMON monster list works the same way.
 * Yes / No boxes (main func 800219B4): the pointer's x picks the answer. Unit status page 2 (main func
 * 8002F840): the pointer picks the attack / spell / skill described. Icon menus count only while the game
 * runs their input (main func 8002B940): their state outlives them, under SUMMON, STATUS...
 * Events (main func 80032004 sets 0x800A69F4 while a script runs): holding the left button holds Square,
 * the game's fast-forward. Also on the battle map while the other side takes its turn (0x800A6BE0 != 0).
 * Title screen (overlay 3): a click passes PUSH START BUTTON (Start), its menu is a list. SELECT RULER: a
 * sideways push or the wheel is Left / Right.
 * Organize EQUIP / ITEM item grid (strategy func 800CA034): 2 columns, the pointer picks the cell, pushing
 * past the top / bottom row scrolls.
 * Organize NAME letter grid (strategy func 800CF8A8): the pointer picks the letter, empty cells are skipped.
 * Also lists: UNITS (main func 80049AD4; above the first knight is the header row, x picks the sort column) and the memory card file boxes (main func 80045A74). DOMAIN
 * overview map (strategy func 800C57D8): the box follows the pointer, a click jumps there. In the troop pickers Ctrl / Shift click
 * or middle click is Square (mark a troop; the game then moves to the next row, as with the pad).
 * Everywhere else: left = Circle, right = Cross, middle = Triangle, wheel = Up/Down. Start: the side
 * buttons, or right click on a map with the free cursor (strategy / battle, input mode 0).
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
#define RAM_MENU_STATE  0x800A696Cu   /* 2 = taking input */
#define MENU_POS        0x800ADCE0u   /* per item: x, y (s32, 8.8 fixed); the icon is at x - 8 */
#define MENU_W          80.0          /* item box: x - 8 .. x + 72, y - 3 .. y + 16 */
#define MENU_H          19.0
#define RAM_UI_FLAGS    0x800A6AB0u   /* bit 0: a free cursor takes input, bit 3: strategy map, bit 8: on a castle */
#define RAM_CASTLE_CUR  0x800A698Cu   /* castle under the strategy cursor (valid with flag bit 8) */
#define CASTLE_TABLE    0x80096760u   /* 28 bytes per castle: x, y (u16, world >> 8) at +4 / +6 */
#define RAM_CASTLE_EXTRA 0x800DACD8u  /* bit 5: a 42nd castle exists */
#define PANEL_Y 198.0                 /* the unit / terrain panel starts here (not map) */
#define BM_FOLLOW_X0 54.0              /* the battle camera follows its cursor out of this box (screen) */
#define BM_FOLLOW_X1 266.0
#define BM_FOLLOW_Y0 83.0
#define BM_FOLLOW_Y1 135.0
/* generic scrolling list (main EXE func 80021EB8: magic, skills, attacks, items...), $gp-relative */
#define RAM_LIST_TOP    0x800A6820u   /* first visible item */
#define RAM_LIST_WIN    0x800A6824u   /* window handle: window table 0x800ACE28 + 18 * handle (x, y, w, h s16) */
#define RAM_LIST_INDEX  0x800A6834u   /* selected item */
#define RAM_LIST_STATE  0x800A6854u   /* 2 = taking input */
#define RAM_LIST_REDRAW 0x800A6860u   /* 2 = item texts must be redrawn (scrolled) */
#define RAM_LIST_VIS    0x800A6864u   /* visible rows */
#define WIN_TABLE       0x800ACE28u
#define RAM_FADE        0x800A67BCu   /* bit 3: screen fading, no input */
#define RAM_EVENT       0x800A69F4u   /* 1 while an event script runs (set by main func 80032004) */
#define RAM_BATTLE_SIDE 0x800A6BE0u   /* battle: side taking its turn (0 = the player's, units carry theirs at +37) */
#define FF_DELAY        20            /* frames the left button must be held before it fast-forwards */
/* title screen (overlay 3, id 7, func 800B73F0): state 100 = PUSH START BUTTON (Start only), 110 = menu
 * NEWGAME / CONTINUE / TUTORIAL / RECOLLECTION, index in the generic list index; rows y 114 + 18 k */
#define TITLE_OVL       7u
#define RAM_TITLE_STATE 0x800C6B38u
#define LIST_ROW        16.0          /* row pitch; row 0 starts 7 px below the window top, 8 px in */

/* screen = H * (world - camera), camera = (camX / KX, camZ / KZ) */
static const double H[3][3] = {
    { 3.279508843e+01, -1.024355928e+01, 1.469460342e+02 },
    { -2.234536182e-02, 1.473237364e+01, 1.304963405e+02 },
    { -9.194202619e-04, -6.299702326e-02, 1.0 },
};
static const double KX = 3.260455542e+01, KZ = 3.198126134e+01;

/* PS1 pad bits */
#define B_START  (1u << 3)
#define B_UP     (1u << 4)
#define B_RIGHT  (1u << 5)
#define B_DOWN   (1u << 6)
#define B_LEFT   (1u << 7)
#define B_TRI    (1u << 12)
#define B_CIRCLE (1u << 13)
#define B_CROSS  (1u << 14)
#define B_SQUARE (1u << 15)

static double s_vx = -1, s_vy = -1;        /* pointer, display pixels (-1 = unknown) */
static int s_moved;                        /* pointer moved since the last update */
static int s_captured;
static int s_view_x, s_view_y, s_view_w, s_view_h;   /* image rect, drawable pixels */
static float s_px_per_pt = 1.0f;

static u16 s_pulse_bits;                   /* button pulse in progress */
static int s_pulse_frames, s_gap_frames;
static u16 s_edge_bits;                    /* maps: pointer held against an edge = D-pad held (the game scrolls) */
static int s_left, s_right, s_mid, s_wheel_up, s_wheel_down;   /* queued clicks */
static int s_side;                         /* side (back / forward) button: Start */
static double s_hpush;                     /* unit list: horizontal push towards the next / previous page */
static int s_left_mod;                     /* the left click had Ctrl or Shift held */
static int s_left_held;                    /* left button down now */
static unsigned s_left_down;               /* frame it went down */
static u16 s_ff_bits;                      /* event fast-forward: Square held */

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
static u32 s_confirm_addr;                 /* menu index variable the pending click is checked against */
static unsigned s_frame;                   /* mouse_update calls */
static unsigned s_list_seen = 0x80000000u;               /* frame the game last ran its list menu */
static int s_list_count;                   /* items in that list */
static double s_list_pos;                  /* list pointer, in items (index + fraction) */
static int s_list_valid, s_list_wr = -1;
static double s_list_push;                 /* pointer pushed past the visible rows (pad-scrolled lists), rows */

/* the game runs its scrolling list menu this frame (hook from the recompiled function; see
 * hook_list.txt): a0 = item count, a1 = visible rows, a2 = disabled flags, a3 = x, then w, centre y */
void hook_f_main_80021EB8(Ctx *c) {
    s_list_count = (s32)c->r[4];
    s_list_seen = s_frame;
    if (getenv("BRIG_MOUSE_TRACE2")) rt_log("[mouse] list hook %u count %d vis %d", s_frame, (s32)c->r[4], (s32)c->r[5]);
}
/* Organize squad grid (strategy overlay func 800CDB1C, CLASS / EQUIP / ...): the game keeps a pixel
 * cursor (x = 42 * column, column 0 = leader; y = row pitch * row) and derives the unit from it.
 * Rows: knights (pitch 44, 3 visible), or the monster stock (pitch 40). The quill is drawn at
 * (x + 33, y - scroll + 104 / 108). */
#define GRID_MODE    0x800DC660u   /* 0 = squads, 312 = stock, between = sliding */
#define GRID_X       0x800DC664u
#define GRID_SCROLL  0x800DC668u   /* squads */
#define GRID_Y       0x800DC66Cu
#define GRID_SCROLL2 0x800DC670u   /* stock */
#define GRID_SCROLLV 0x800DC688u   /* squads: scroll animation speed, 0 = still */
#define GRID_SCROLLN 0x800DC84Cu   /* squads: scroll animation frames left */
#define RAM_KNIGHTS  0x800A690Cu   /* knights in the castle = squad rows */
static unsigned s_grid_seen = 0x80000000u;
static double s_grid_sx, s_grid_sy;        /* grid pointer, display pixels */
static int s_grid_valid;
static double s_grid_push_x, s_grid_push_y;   /* pointer pushed past the grid edge */
static u32 s_grid_wx = 0xFFFFFFFF, s_grid_wy = 0xFFFFFFFF;
void hook_f_ov1_800CDB1C(Ctx *c) { (void)c; s_grid_seen = s_frame; }
static int grid_active(void) {
    u32 m = rd32(GRID_MODE);
    return s_frame - s_grid_seen <= 2 && (m == 0 || m == 312) && rd32(OVL_ID_ADDR) == 5;
}
typedef struct { double pitch, top; u32 scroll_addr; int rows; } GridLayout;
static GridLayout grid_layout(void) {
    GridLayout g;
    if (rd32(GRID_MODE) == 0) { g.pitch = 44; g.top = 104; g.scroll_addr = GRID_SCROLL; g.rows = (s32)rd32(RAM_KNIGHTS); }
    else { g.pitch = 40; g.top = 108; g.scroll_addr = GRID_SCROLL2; g.rows = 760 / 40 + 1; }
    if (g.rows < 1) g.rows = 1;
    return g;
}
/* Lists the pointer walks: the generic list above, the strategy troop pickers (main func 80022C84:
 * MOVE / QUEST "Select Troop"...; they share the list's variables and window but have their own
 * loop: no hook, no scrolling, all rows shown, so the item count comes from the window height,
 * rows * 16 + 14), and the SUMMON monster list (strategy overlay func 800D09A0, state 0 = choosing:
 * 3 boxes of 47 px, scrolled by the game's own top variable). */
#define RAM_SUMMON_STATE 0x800A6910u  /* 0 = choosing a monster */
#define RAM_SUMMON_INDEX 0x800A6948u
#define RAM_SUMMON_COUNT 0x800DC8A4u
#define RAM_SUMMON_TOP   0x800DC8A8u
/* Unit lists (main func 80049AD4: Start > UNITS knights / monsters...): 10 rows of 16 px from y 47, row 0
 * is the column header (sorting), state 20 = choosing */
#define RAM_UL_STATE 0x800A6CF4u
#define RAM_UL_COUNT 0x800A6CDCu      /* s16 */
#define RAM_UL_TOP   0x800A6CDEu      /* s16 */
#define RAM_UL_ROW   0x800A6CE2u      /* s16, visible row 1..10, 0 = header */
/* UNITS header row (sort): column s8 0x800A6CE0 (0 = Knights at x 32, 58 wide; column c > 0: x s16 at
 * 0x8009DB00 + 12 * page + 2 * (c - 1), width u8 at 0x8009DB48 + 6 * page + c - 1), page s8 0x800A6CE8,
 * last column of page p = (0x332564 >> 4p) & 15; boxes y 24..40. Circle sorts by the column. */
#define RAM_UL_HCOL  0x800A6CE0u
#define RAM_UL_PAGE  0x800A6CE8u
static int s_ulh;                    /* the pointer is on the header row */
static double s_ulh_x, s_ulh_push;
static int ul_hcol_box(int c, double *x, double *w) {
    int page = (s8)rd8(RAM_UL_PAGE);
    if (page < 0 || page > 5 || c < 0 || c > ((0x332564 >> (4 * page)) & 15)) return 0;
    if (c == 0) { *x = 32; *w = 58; return 1; }
    *x = (s16)rd16(0x8009DB00u + (u32)(12 * page + 2 * (c - 1)));
    *w = rd8(0x8009DB48u + (u32)(6 * page + c - 1));
    return *w > 0;
}
static int ul_hcol_at(double px) {    /* the column under x, else the nearest */
    int best = 0; double bd = 1e9, x, w;
    for (int c = 0; c < 7; c++) {
        if (!ul_hcol_box(c, &x, &w)) continue;
        if (px >= x && px < x + w) return c;
        double d = fabs(px - (x + w / 2));
        if (d < bd) { bd = d; best = c; }
    }
    return best;
}
static unsigned s_ul_seen = 0x80000000u;
void hook_f_main_80049AD4(Ctx *c) { (void)c; s_ul_seen = s_frame; }
/* DOMAIN overview map (strategy overlay, drawn by func 800C57D8): a free box cursor, position in
 * 1/256 px (screen = X / 256 + 35, Y / 256 - 5); Circle jumps the strategy map there */
#define RAM_DOM_X    0x800DACD0u
#define RAM_DOM_Y    0x800DACD4u
#define RAM_DOM_HELP 0x800DB4FCu      /* non-zero: help text shown, no cursor */
static unsigned s_dom_seen = 0x80000000u;
static double s_dom_x, s_dom_y;
static int s_dom_valid;
static u32 s_dom_wx, s_dom_wy;
void hook_f_ov1_800C57D8(Ctx *c) { (void)c; s_dom_seen = s_frame; }
static int dom_active(void) {
    return rd32(OVL_ID_ADDR) == 5 && s_frame - s_dom_seen <= 2 && !(rd32(RAM_FADE) & 8) && rd32(RAM_DOM_HELP) == 0;
}
/* SELECT RULER (new game, overlay 3, func 800B81EC): state 10 = choosing (20 = the Yes / No that follows); Left / Right step through the
 * 6 rulers (index 0x800C6B70), Circle picks */
#define RAM_RULER_STATE 0x800C6B44u
static unsigned s_ruler_seen = 0x80000000u;
void hook_f_ov3_800B81EC(Ctx *c) { (void)c; s_ruler_seen = s_frame; }
static int ruler_active(void) {
    return rd32(OVL_ID_ADDR) == TITLE_OVL && s_frame - s_ruler_seen <= 2 && rd32(RAM_RULER_STATE) == 10 && !(rd32(RAM_FADE) & 8);
}
/* item grid (Organize EQUIP / ITEM, strategy func 800CA034): 2 columns x 4 visible rows of 144 x 16 px
 * from (16, 138); row = generic list index, first visible row = generic list top, column 0x800DC894,
 * item = row * 2 + column, count 0x800DC680 */
#define IT_COUNT 0x800DC680u
#define IT_COL   0x800DC894u
#define IT_X 16.0
#define IT_Y 138.0
#define IT_W 144.0
#define IT_H 16.0
static unsigned s_it_seen = 0x80000000u;
static double s_it_x, s_it_y, s_it_push;
static int s_it_valid;
static u32 s_it_wr;
void hook_f_ov1_800CA034(Ctx *c) { (void)c; s_it_seen = s_frame; }
static int it_active(void) {
    return rd32(OVL_ID_ADDR) == 5 && s_frame - s_it_seen <= 2 && (s32)rd32(IT_COUNT) > 0
        && rd32(0x800A6910u) == 0 && !(rd32(RAM_FADE) & 8);
}
/* NAME entry (Organize NAME, strategy func 800CF8A8): letter grid, column 0x800DC658 (0..24 letters, 25 =
 * Kana / Eng. / Space / Cancel / Finish), row 0x800DC65C (0..4); cell 10 x 20 px from (22, 98), the
 * command column 27 px wide. Letter table 0x800DA4F0 + column + 26 * row + 130 * page (0x800DC88C),
 * 255 = empty cell (the pad skips those) */
#define NM_COL   0x800DC658u
#define NM_ROW   0x800DC65Cu
#define NM_PAGE  0x800DC88Cu
#define NM_TABLE 0x800DA4F0u
static unsigned s_nm_seen = 0x80000000u;
static double s_nm_x, s_nm_y;
static int s_nm_valid;
static u32 s_nm_wr;
void hook_f_ov1_800CF8A8(Ctx *c) { (void)c; s_nm_seen = s_frame; }
static int nm_active(void) {
    return rd32(OVL_ID_ADDR) == 5 && s_frame - s_nm_seen <= 2 && !(rd32(RAM_FADE) & 8) && rd32(0x800A6938u) == 0
        && rd32(NM_PAGE) < 4;
}
static int nm_empty(int col, int row) { return rd8(NM_TABLE + (u32)(col + 26 * row + 130 * (int)rd32(NM_PAGE))) == 255; }
/* memory card SAVE / LOAD (main func 80045A74): state 40 = choosing one of 3 file boxes (50 px from y 36) */
#define RAM_MC_STATE 0x800A6CB4u
#define RAM_MC_FILE  0x800A6CB0u
static unsigned s_mc_seen = 0x80000000u;
void hook_f_main_80045A74(Ctx *c) { (void)c; s_mc_seen = s_frame; }
/* small menus (main func 800257F8, OPTION and its pages...): a0 = last index, index 0x800A6948, drawn
 * in the list window */
#define RAM_SMALL_INDEX 0x800A6948u
static unsigned s_small_seen = 0x80000000u;
static int s_small_count;
void hook_f_main_800257F8(Ctx *c) { s_small_seen = s_frame; s_small_count = (s32)c->r[4] + 1; }
static unsigned s_summon_seen = 0x80000000u;
static unsigned s_menu_seen = 0x80000000u;                /* frame the game last ran its icon menu input (main 8002B940) */
void hook_f_main_8002B940(Ctx *c) { (void)c; s_menu_seen = s_frame; }
void hook_f_ov1_800D09A0(Ctx *c) { (void)c; s_summon_seen = s_frame; }
/* Yes / No box (main func 800219B4, args x, y, w, h): "Yes" left of the box centre, "No" right */
#define RAM_YN_STATE 0x800A67F8u      /* 2 = taking input */
#define RAM_YN_WIN   0x800A6804u      /* window handle */
#define RAM_YN_INDEX 0x800A685Cu      /* 0 Yes, 1 No */
static unsigned s_yn_seen = 0x80000000u;
static double s_yn_x;                 /* pointer x */
static int s_yn_valid, s_yn_wr = -1;
void hook_f_main_800219B4(Ctx *c) { (void)c; s_yn_seen = s_frame; }
/* Unit status, page 2 (main func 8002F840): three lists, ATTACK (x 14, all rows), MAGIC (x 168, 8 rows,
 * scrolls) and SKILL (x 14, y 159, 3 rows, scrolls); rows 16 px from y 79, 138 px wide. The selected
 * one is only described (no action), so the pointer just picks what is described. */
#define RAM_SP_SECT   0x800A6944u     /* 0 attack, 1 magic, 2 skill */
#define RAM_SP_INDEX  0x800A6948u
#define RAM_SP_NATK   0x800A6908u
#define RAM_SP_NMAG   0x800A69D4u
#define RAM_SP_MAGTOP 0x800A69D8u
#define RAM_SP_NSKL   0x800A69E4u
#define RAM_SP_SKLTOP 0x800A69E8u
static unsigned s_sp_seen = 0x80000000u;
static double s_sp_x, s_sp_y, s_sp_push;
static int s_sp_valid;
static u32 s_sp_wr = 0xFFFFFFFF;
void hook_f_main_8002F840(Ctx *c) { (void)c; s_sp_seen = s_frame; }
static int sp_active(void) { return s_frame - s_sp_seen <= 2 && !(rd32(RAM_FADE) & 8); }
typedef struct { double x, y; int n, vis; u32 top; } SpSect;
static SpSect sp_sect(int k) {
    SpSect t;
    if (k == 0) { t.x = 14; t.y = 79; t.n = (s32)rd32(RAM_SP_NATK); t.vis = t.n; t.top = 0; }
    else if (k == 1) { t.x = 168; t.y = 79; t.n = (s32)rd32(RAM_SP_NMAG); t.vis = 8; t.top = RAM_SP_MAGTOP; }
    else { t.x = 14; t.y = 159; t.n = (s32)rd32(RAM_SP_NSKL); t.vis = 3; t.top = RAM_SP_SKLTOP; }
    if (t.vis > t.n) t.vis = t.n;
    return t;
}
static int yn_active(void) { return s_frame - s_yn_seen <= 2 && rd32(RAM_YN_STATE) == 2; }
static void yn_box(double *x, double *y, double *w, double *h) {
    u32 e = WIN_TABLE + 18 * rd32(RAM_YN_WIN);
    *x = (s16)rd16(e); *y = (s16)rd16(e + 2); *w = (s16)rd16(e + 4); *h = (s16)rd16(e + 6);
}
typedef struct {
    u32 idx, top, redraw;        /* index / first visible row variables (top 0 = never scrolls) */
    int count, vis;              /* items, visible rows */
    double x, y, w, pitch;       /* first row's box, row pitch */
    int kind;                    /* 1 generic, 2 troop picker, 3 summon, 4 unit list, 5 memory card files, 6 small menu, 7 title */
    int rel;                     /* idx / top are s16 and idx holds the visible row, 1-based (unit list) */
    int pad_scroll;              /* total unknown: pick among the visible rows, scroll with Up / Down */
} ListDesc;
static ListDesc s_ld;
static int s_list_fixed;         /* troop picker */
static double disp_w(void);
static int list_active(void) {
    ListDesc *L = &s_ld;
    s_list_fixed = 0; L->rel = 0; L->pad_scroll = 0;
    if (rd32(OVL_ID_ADDR) == TITLE_OVL && rd32(RAM_TITLE_STATE) == 110 && !(rd32(RAM_FADE) & 8)) {
        L->idx = RAM_LIST_INDEX; L->top = 0; L->redraw = 0;
        L->count = 4; L->vis = 4; L->x = disp_w() * 0.3; L->y = 114; L->w = disp_w() * 0.4;   /* the title runs 640 wide */
        L->pitch = 18; L->kind = 7;
        return 1;
    }
    if (s_frame - s_ul_seen <= 2 && rd32(RAM_UL_STATE) == 20 && !(rd32(RAM_FADE) & 8)) {
        int n = (s16)rd16(RAM_UL_COUNT);
        if (n < 1) return 0;
        L->idx = RAM_UL_ROW; L->top = RAM_UL_TOP; L->redraw = 0; L->rel = 1;
        L->count = n; L->vis = n < 10 ? n : 10; L->x = 8; L->y = 47; L->w = 304; L->pitch = 16; L->kind = 4;
        return 1;
    }
    if (s_frame - s_mc_seen <= 2 && rd32(RAM_MC_STATE) == 40 && !(rd32(RAM_FADE) & 8)) {
        L->idx = RAM_MC_FILE; L->top = 0; L->redraw = 0;
        L->count = 3; L->vis = 3; L->x = 8; L->y = 36; L->w = 304; L->pitch = 50; L->kind = 5;
        return 1;
    }
    if (rd32(OVL_ID_ADDR) == 5 && s_frame - s_summon_seen <= 2 && rd32(RAM_SUMMON_STATE) == 0) {
        int n = (s32)rd32(RAM_SUMMON_COUNT);
        if (n < 1) return 0;
        L->idx = RAM_SUMMON_INDEX; L->top = RAM_SUMMON_TOP; L->redraw = 0;
        L->count = n; L->vis = 3; L->x = 8; L->y = 64; L->w = 170; L->pitch = 47; L->kind = 3;
        return 1;
    }
    if ((s32)rd32(RAM_LIST_STATE) != 2) return 0;
    u32 e = WIN_TABLE + 18 * rd32(RAM_LIST_WIN);
    double wx = (s16)rd16(e), wy = (s16)rd16(e + 2), ww = (s16)rd16(e + 4), wh = (s16)rd16(e + 6);
    L->idx = RAM_LIST_INDEX; L->x = wx + 8; L->y = wy + 7; L->w = ww - 16; L->pitch = LIST_ROW;
    if (s_frame - s_list_seen <= 2 && s_list_count > 0) {
        L->top = RAM_LIST_TOP; L->redraw = RAM_LIST_REDRAW;
        L->count = s_list_count; L->vis = (s32)rd32(RAM_LIST_VIS); L->kind = 1;
        return 1;
    }
    if (s_frame - s_small_seen <= 2 && s_small_count > 0 && s_small_count <= 16) {
        L->idx = RAM_SMALL_INDEX; L->top = 0; L->redraw = 0;
        L->count = s_small_count; L->vis = s_small_count; L->kind = 6;
        return 1;
    }
    if (rd32(OVL_ID_ADDR) != 5) return 0;
    int n = (int)lround((wh - 14) / LIST_ROW);
    if (n < 1 || n > 16) return 0;
    /* the troop pickers scroll (LOCATION: all knights) but their total is not stored anywhere */
    L->top = RAM_LIST_TOP; L->redraw = 0; L->vis = n; L->count = (s32)rd32(RAM_LIST_TOP) + n; L->kind = 2;
    L->pad_scroll = 1;
    s_list_fixed = 1;
    return 1;
}
static int list_top(void) { return !s_ld.top ? 0 : s_ld.rel ? (s16)rd16(s_ld.top) : (s32)rd32(s_ld.top); }
/* selected item (-1: none, the unit list's header row) */
static int list_cur(void) { return s_ld.rel ? list_top() + (s16)rd16(s_ld.idx) - 1 : (s32)rd32(s_ld.idx); }
/* list item under a display position, -1 none */
static int list_item_at(double px, double py) {
    const ListDesc *L = &s_ld;
    int top = list_top();
    if (px < L->x || px >= L->x + L->w || py < L->y) return -1;
    int k = (int)floor((py - L->y) / L->pitch);
    return k >= 0 && k < L->vis && top + k < L->count ? top + k : -1;
}
/* select item k, scrolling it into view */
static void list_select(int k) {
    const ListDesc *L = &s_ld;
    if (k < 0) k = 0;
    if (k > L->count - 1) k = L->count - 1;
    int top = list_top(), nt = top;
    if (k < nt) nt = k;
    if (k > nt + L->vis - 1) nt = k - L->vis + 1;
    if (L->rel) {
        if (nt != top) wr16(L->top, (u16)nt);
        wr16(L->idx, (u16)(k - nt + 1));
        s_list_wr = k;
        return;
    }
    if (nt != top && L->top) { wr32(L->top, (u32)nt); if (L->redraw) wr32(L->redraw, 2); }
    if ((s32)rd32(L->idx) != k) wr32(L->idx, (u32)k);
    s_list_wr = k;
}

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
/* icon menu item k: top-left of its box (the game slides the items in from the right) */
static void menu_item_box(int k, double *x, double *y) {
    *x = ((s32)rd32(MENU_POS + 8 * k) >> 8) - 8; *y = ((s32)rd32(MENU_POS + 8 * k + 4) >> 8) - 3;
}
static int menu_item_at(double px, double py) {   /* icon menu item under the pointer, -1 none */
    int last = (s32)rd32(RAM_MENU_LAST);
    for (int k = 0; k <= last && k < 16; k++) {
        double x, y; menu_item_box(k, &x, &y);
        if (px >= x && px < x + MENU_W && py >= y && py < y + MENU_H) return k;
    }
    return -1;
}
/* captured pointer: the item whose row holds display y (clamped to the first / last row) */
static int menu_item_row(double py) {
    int last = (s32)rd32(RAM_MENU_LAST), best = 0;
    for (int k = 0; k <= last && k < 16; k++) {
        double x, y; menu_item_box(k, &x, &y);
        if (py >= y) best = k;
    }
    return best;
}

/* Strategy map (overlay 5): the map is bent to fake the horizon, so it is not a plane. With
 * u = x - camX / 32, v = z - camZ / 32 (world in 8192 units, camera from RAM), the game draws its
 * cursor (the quill's tip) at screen x = 160 + u * s(v), y = f(v); fitted to the quill sprite
 * (< 1 px). The camera keeps the cursor inside u in [-3.22, 3.22], v in [-2.59, 2.19]. */
#define SMAP_U  3.22
#define SMAP_V0 -2.59
#define SMAP_V1 2.19
static double smap_s(double v) { return (0.26788293 * v + 3.09757407) * v + 32.06222178; }
static double smap_y(double v) { return ((0.33245762 * v + 2.67249574) * v + 26.38856192) * v + 119.82098294; }
static void smap_cam(double *cu, double *cv) { *cu = (s32)rd32(RAM_CAM_X) / 32.0; *cv = (s32)rd32(RAM_CAM_Z) / 32.0; }
static void smap_to_screen(double wx, double wz, double *sx, double *sy) {
    double cu, cv; smap_cam(&cu, &cv);
    double u = wx - cu, v = wz - cv;
    *sx = 160 + u * smap_s(v); *sy = smap_y(v);
}
/* the game's castle hit test (overlay func 800B89D0), run after the mouse moves the cursor: which
 * castle is under it decides the name shown and what Circle picks */
static void smap_castle_hit(void) {
    s32 gx = (s32)rd32(RAM_CUR_WX), gz = (s32)rd32(RAM_CUR_WZ);
    int n = (rd32(RAM_CASTLE_EXTRA) & 0x20) ? 42 : 41;
    u32 flags = rd32(RAM_UI_FLAGS) & ~0x100u;
    for (int i = 0; i < n; i++) {
        s32 cx = (s32)rd16(CASTLE_TABLE + 28 * i + 4) << 8, cz = (s32)rd16(CASTLE_TABLE + 28 * i + 6) << 8;
        if (gx > cx - 4608 && gx < cx + 4608 && gz > cz - 3072 && gz < cz + 5120) {
            wr32(RAM_CASTLE_CUR, (u32)i); flags |= 0x100; break;
        }
    }
    wr32(RAM_UI_FLAGS, flags);
}
static void smap_to_world(double sx, double sy, double *wx, double *wz) {
    double v = 0;                                     /* f is strictly increasing: Newton */
    for (int i = 0; i < 20; i++) {
        double d = (0.99737286 * v + 5.34499148) * v + 26.38856192;
        v -= (smap_y(v) - sy) / d;
    }
    double cu, cv; smap_cam(&cu, &cv);
    *wx = (sx - 160) / smap_s(v) + cu; *wz = v + cv;
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
        if (e->button.button == SDL_BUTTON_LEFT) {
            s_left++;
            s_left_mod = (SDL_GetModState() & (KMOD_CTRL | KMOD_SHIFT)) != 0;
            s_left_held = 1; s_left_down = s_frame;
        }
        else if (e->button.button == SDL_BUTTON_RIGHT) s_right++;
        else if (e->button.button == SDL_BUTTON_MIDDLE) s_mid++;
        else if (e->button.button == SDL_BUTTON_X1 || e->button.button == SDL_BUTTON_X2) s_side++;
        return 1;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) s_left_held = 0;
        return 1;
    case SDL_MOUSEWHEEL:
        if (e->wheel.y > 0) s_wheel_up++;
        else if (e->wheel.y < 0) s_wheel_down++;
        return 1;
    }
    return 0;
}

void mouse_cancel(void) { s_confirm_wait = 0; }

static void start_pulse_n(u16 bits, int frames) { s_pulse_bits = bits; s_pulse_frames = frames; s_gap_frames = 2; }
static void start_pulse(u16 bits) { start_pulse_n(bits, 6); }

/* a deliberate sideways push of the captured mouse (not a wobble): -1 / +1 = Left / Right, 0 none */
static int page_push(double mvx, double mvy, int abs_move) {
    if (s_moved && !abs_move && fabs(mvx) > fabs(mvy)) {
        s_hpush += mvx;
        if (fabs(s_hpush) >= 64) return s_hpush > 0 ? 1 : -1;
    } else if (s_moved && fabs(mvy) > fabs(mvx)) s_hpush = 0;
    return 0;
}

/* maps: pushing the pointer outwards against an edge holds that D-pad direction, so the game moves its
 * cursor and camera at its own speed (Cursor Speed option) for as long as the pointer stays there;
 * moving the other way lets go */
static void edge_release(double mvx, double mvy) {
    if (mvx < -0.5) s_edge_bits &= ~B_RIGHT;
    if (mvx > 0.5) s_edge_bits &= ~B_LEFT;
    if (mvy < -0.5) s_edge_bits &= ~B_DOWN;
    if (mvy > 0.5) s_edge_bits &= ~B_UP;
}
static void edge_push(int l, int r, int u, int d, double mvx, double mvy, double min) {
    if (l && mvx < -min) s_edge_bits |= B_LEFT;
    if (r && mvx > min) s_edge_bits |= B_RIGHT;
    if (u && mvy < -min) s_edge_bits |= B_UP;
    if (d && mvy > min) s_edge_bits |= B_DOWN;
}

/* pointer capture follows focus and the settings window */
static void update_capture(void) {
    if (getenv("BRIG_FAKE_CAPTURE")) { s_captured = 1; return; }   /* tests: behave as captured */
    SDL_Window *win = SDL_GetKeyboardFocus();
    if (!win || ui_is_open()) s_left_held = 0;   /* the button-up may never arrive: never fast-forward on our own */
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
    s_frame++;
    if (!s_left_held || !g_settings.mouse) s_ff_bits = 0;
    /* movement is taken every frame; whatever arrives while busy is dropped, never replayed later */
    double mvx = s_acc_x, mvy = s_acc_y;
    s_acc_x = s_acc_y = 0;
    int abs_move = s_abs; s_abs = 0;
    if (s_cool > 0) { s_cool--; s_moved = 0; mvx = mvy = 0; }
    if (s_pulse_frames > 0) { if (--s_pulse_frames == 0) s_pulse_bits = 0; s_moved = 0; return; }
    if (s_gap_frames > 0) { s_gap_frames--; s_moved = 0; return; }

    u32 ovl = rd32(OVL_ID_ADDR);
    int battle = ovl == 6 && rd32(RAM_SCREEN) == 2;
    int mode = (int)rd32(RAM_INPUT_MODE);
    u32 flags = rd32(RAM_UI_FLAGS);
    /* icon menu taking input: its state stays 2 after it closes and under the screens it opens
     * (SUMMON, STATUS...), so the game must also have run its menu input (hook) just now */
    int menu = (s32)rd32(RAM_MENU_LAST) >= 0 && rd32(RAM_MENU_STATE) == 2 && !(flags & 1) && s_frame - s_menu_seen <= 2;
    int smap = ovl == 5 && (flags & 0x9) == 0x9;
    int ctx = yn_active() ? 7 : sp_active() ? 8 : dom_active() ? 9 : ruler_active() ? 10 : it_active() ? 11 : nm_active() ? 12 : list_active() ? 4 : grid_active() ? 6 : menu ? 2 : smap ? 5
            : battle && (mode == 0 || mode == 2) && rd8(RAM_CUR_COL) != 0xFF ? 1 : battle ? 3 : 0;   /* 0xFF: cursor hidden */
    if (!battle && !smap && ctx != 2 && ctx != 4 && ctx != 7 && ctx != 8 && ctx != 9 && ctx != 10 && ctx != 11 && ctx != 12) mode = ctx == 6 ? 100 + (int)(rd32(GRID_MODE) != 0) : -1;
    /* events: holding the left button holds Square, the game's fast-forward (the click itself is Circle) */
    /* the same on the battle map while the other side moves */
    int other_turn = ovl == 6 && rd8(RAM_BATTLE_SIDE) != 0;
    s_ff_bits = s_left_held && (rd32(RAM_EVENT) == 1 || other_turn) && s_frame - s_left_down >= FF_DELAY
                && (ctx == 0 || ctx == 1 || ctx == 3 || ctx == 5) ? B_SQUARE : 0;
    /* edge hold: maps with the captured pointer only; a click, the OS pointer or another screen lets go */
    if (!((ctx == 1 || ctx == 5) && s_captured && !abs_move) || s_left || s_right || s_mid) s_edge_bits = 0;
    else edge_release(mvx, mvy);
    if (ctx != s_last_ctx || mode != s_last_mode) {   /* screen / mode changed: re-anchor the pointer */
        s_confirm_wait = 0; s_map_valid = 0; s_menu_valid = 0; s_list_valid = 0; s_grid_valid = 0; s_yn_valid = 0; s_sp_valid = 0; s_dom_valid = 0; s_it_valid = 0; s_nm_valid = 0;
        if (getenv("BRIG_MOUSE_TRACE")) rt_log("[mouse] frame %u ctx %d mode %d", s_frame, ctx, mode);
        s_last_ctx = ctx; s_last_mode = mode;
    }
    /* a click placed something: confirm once the game has taken it */
    if (s_confirm_wait > 0) {
        if (--s_confirm_wait == 0) {
            if (s_confirm_menu >= 0) { if (!s_confirm_addr || (s32)rd32(s_confirm_addr) == s_confirm_menu) start_pulse(B_CIRCLE); }
            else if (rd8(RAM_CUR_COL) == s_confirm_col && rd8(RAM_CUR_ROW) == s_confirm_row) start_pulse(B_CIRCLE);
        }
        s_left = 0; s_moved = 0;
        return;
    }

    if (s_side) { s_side = 0; s_left = 0; start_pulse(B_START); return; }
    /* right click = Cross (back); on a map with the free cursor (Cross does nothing there) = Start */
    /* title screen, PUSH START BUTTON: the game only takes Start there */
    if (s_left && rd32(OVL_ID_ADDR) == TITLE_OVL && rd32(RAM_TITLE_STATE) == 100) { s_left = 0; start_pulse(B_START); s_cool = 20; return; }
    if (s_right) { s_right = 0; s_left = 0; start_pulse((ctx == 1 || ctx == 5) && mode == 0 ? B_START : B_CROSS); return; }
    int troop = ctx == 4 && s_list_fixed;              /* troop picker: Square = mark */
    if (s_mid && !troop) { s_mid = 0; start_pulse(B_TRI); return; }

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
            /* the camera keeps the cursor within x 52..268, y 81..137: a push (not a drift) past that holds */
            if (!abs_move) edge_push(sx < BM_FOLLOW_X0, sx > BM_FOLLOW_X1, sy < BM_FOLLOW_Y0, sy > BM_FOLLOW_Y1, mvx, mvy, 3.0);
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
        if (!s_menu_valid) { double x, y; menu_item_box(cur, &x, &y); s_menu_y = y + MENU_H / 2; s_menu_valid = 1; }
        if (s_moved) {
            double x0, y0, x1, y1; menu_item_box(0, &x0, &y0); menu_item_box(last, &x1, &y1);
            if (abs_move) s_menu_y = s_vy; else s_menu_y += mvy;
            if (s_menu_y < y0 + 0.5) s_menu_y = y0 + 0.5;
            if (s_menu_y > y1 + MENU_H - 0.5) s_menu_y = y1 + MENU_H - 0.5;
            int k = menu_item_row(s_menu_y);
            if (k != cur) wr32(RAM_MENU_INDEX, (u32)k);
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
            s_confirm_menu = k; s_confirm_addr = RAM_MENU_INDEX; s_confirm_wait = 2; s_cool = 20;
            return;
        }
    } else if (ctx == 5) {                            /* strategy map: the quill follows the pointer */
        u32 gx = rd32(RAM_CUR_WX), gz = rd32(RAM_CUR_WZ);
        if (!s_map_valid || gx != s_wr_x || gz != s_wr_z) {   /* the game moved it: continue from there */
            s_wx = (s32)gx / 8192.0; s_wz = (s32)gz / 8192.0;
            s_wr_x = gx; s_wr_z = gz; s_map_valid = 1;
            double sx, sy; smap_to_screen(s_wx, s_wz, &sx, &sy); warp_os_pointer(sx, sy);
            s_moved = 0;
        }
        if (s_moved) {
            double sx, sy, wx, wz;
            if (abs_move) { sx = s_vx; sy = s_vy; }
            else { smap_to_screen(s_wx, s_wz, &sx, &sy); sx += mvx; sy += mvy; }
            if (sy < 0) sy = 0;
            if (sy > disp_h() - 1) sy = disp_h() - 1;
            smap_to_world(sx, sy, &wx, &wz);
            /* a little past the camera box: the game's camera follows, so pushing on scrolls */
            double cu, cv; smap_cam(&cu, &cv);
            if (!abs_move) edge_push(wx <= cu - SMAP_U - 0.4, wx >= cu + SMAP_U + 0.4,
                                     wz <= cv + SMAP_V0 - 0.3, wz >= cv + SMAP_V1 + 0.3, mvx, mvy, 0.5);
            if (wx < cu - SMAP_U - 0.4) wx = cu - SMAP_U - 0.4;
            if (wx > cu + SMAP_U + 0.4) wx = cu + SMAP_U + 0.4;
            if (wz < cv + SMAP_V0 - 0.3) wz = cv + SMAP_V0 - 0.3;
            if (wz > cv + SMAP_V1 + 0.3) wz = cv + SMAP_V1 + 0.3;
            if (wx < 0) wx = 0;
            if (wz < 0) wz = 0;
            s_wx = wx; s_wz = wz;
            s_wr_x = (u32)(s32)lround(wx * 8192.0); s_wr_z = (u32)(s32)lround(wz * 8192.0);
            wr32(RAM_CUR_WX, s_wr_x); wr32(RAM_CUR_WZ, s_wr_z);
            smap_castle_hit();
        }
        s_moved = 0;
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); s_cool = 20; return; }
    } else if (ctx == 6) {                            /* Organize squad grid / stock */
        GridLayout g = grid_layout();
        u32 gx = rd32(GRID_X), gy = rd32(GRID_Y);
        double scroll = (s32)rd32(g.scroll_addr);
        if (!s_grid_valid || gx != s_grid_wx || gy != s_grid_wy) {   /* new screen, or the pad moved it */
            s_grid_sx = (s32)gx + 33 - 12; s_grid_sy = (s32)gy - scroll + g.top - 6;
            s_grid_wx = gx; s_grid_wy = gy; s_grid_valid = 1; s_grid_push_x = s_grid_push_y = 0;
            warp_os_pointer(s_grid_sx, s_grid_sy);
            s_moved = 0;
        }
        if (s_moved && (g.scroll_addr != GRID_SCROLL || rd32(GRID_SCROLLV) == 0)) {
            double dx = abs_move ? s_vx - s_grid_sx : mvx, dy = abs_move ? s_vy - s_grid_sy : mvy;
            s_grid_sx += dx; s_grid_sy += dy;
            int top = (int)lround(scroll / g.pitch);
            double y0 = g.top - 6 - g.pitch / 2, y1 = y0 + 3 * g.pitch - 1;   /* the 3 visible rows */
            double x1 = 7 * 42 - 1;
            /* pushing past the top / bottom row: one row per row-height of push, like the pad */
            if (s_grid_sy < y0) { s_grid_push_y += s_grid_sy - y0; s_grid_sy = y0; }
            else if (s_grid_sy > y1) { s_grid_push_y += s_grid_sy - y1; s_grid_sy = y1; }
            else s_grid_push_y = 0;
            /* pushing past the right end of the squads (or the left end of the stock) turns the page */
            if (s_grid_sx < 0) { s_grid_push_x += s_grid_sx; s_grid_sx = 0; }
            else if (s_grid_sx > x1) { s_grid_push_x += s_grid_sx - x1; s_grid_sx = x1; }
            else s_grid_push_x = 0;
            int col = (int)floor(s_grid_sx / 42.0);
            int row = top + (int)floor((s_grid_sy - y0) / g.pitch);
            if (col > 6) col = 6;
            if (s_grid_push_y <= -g.pitch && top > 0) { row = top - 1; s_grid_push_y = 0; }
            else if (s_grid_push_y >= g.pitch && top + 3 < g.rows) { row = top + 3; s_grid_push_y = 0; }
            if (row < 0) row = 0;
            if (row > g.rows - 1) row = g.rows - 1;
            if (row < top || row > top + 2) {             /* scroll one row */
                int up = row < top;
                if (g.scroll_addr == GRID_SCROLL) {       /* squads: the game's own smooth scroll */
                    /* like the pad: first 4 px now, then 4 px a frame for 10 frames = one row */
                    wr32(GRID_SCROLLV, (u32)(up ? -4 : 4)); wr32(GRID_SCROLLN, 11);
                    wr32(GRID_SCROLL, (u32)((s32)scroll + (up ? -4 : 4)));
                } else wr32(g.scroll_addr, (u32)(s32)((up ? top - 1 : top + 1) * g.pitch));
            }
            u32 nx = (u32)(col * 42), ny = (u32)(s32)(row * g.pitch);
            if (nx != gx) wr32(GRID_X, nx);
            if (ny != gy) wr32(GRID_Y, ny);
            s_grid_wx = nx; s_grid_wy = ny;
            int stock = g.scroll_addr != GRID_SCROLL;
            if ((!stock && col == 6 && s_grid_push_x >= 42) || (stock && col == 0 && s_grid_push_x <= -42)) {
                s_grid_push_x = 0; s_moved = 0;
                start_pulse(stock ? B_LEFT : B_RIGHT);     /* the game slides to the other page */
                return;
            }
        }
        s_moved = 0;
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); s_cool = 20; return; }
    } else if (ctx == 7) {                            /* Yes / No: the pointer's x picks */
        double bx, by, bw, bh; yn_box(&bx, &by, &bw, &bh);
        double cx = bx + bw / 2;
        int cur = (s32)rd32(RAM_YN_INDEX);
        if (!s_yn_valid || cur != s_yn_wr) { s_yn_x = cur ? cx + 28 : cx - 32; s_yn_wr = cur; s_yn_valid = 1; }
        if (s_wheel_up || s_wheel_down) {             /* wheel: up = Yes, down = No */
            s_yn_x = s_wheel_down > s_wheel_up ? cx + 28 : cx - 32; s_wheel_up = s_wheel_down = 0;
            s_moved = 1; abs_move = 0; mvx = 0;
        }
        if (s_moved) {
            s_yn_x = abs_move ? s_vx : s_yn_x + mvx;
            if (s_yn_x < bx + 8) s_yn_x = bx + 8;
            if (s_yn_x > bx + bw - 8) s_yn_x = bx + bw - 8;
            int k = s_yn_x >= cx;
            if (k != cur) wr32(RAM_YN_INDEX, (u32)k);
            s_yn_wr = k;
        }
        s_moved = 0;
        if (s_left) {
            s_left = 0;
            int k = (s32)rd32(RAM_YN_INDEX);
            if (abs_move || !s_captured) {            /* OS pointer: outside the box = cancel */
                if (s_vx < bx || s_vx >= bx + bw || s_vy < by || s_vy >= by + bh) { start_pulse(B_CROSS); return; }
                k = s_vx >= cx; wr32(RAM_YN_INDEX, (u32)k);
            }
            s_confirm_menu = k; s_confirm_addr = RAM_YN_INDEX; s_confirm_wait = 2; s_cool = 20;
            return;
        }
        return;
    } else if (ctx == 12) {                           /* NAME: the pointer picks the letter */
        int col = (s32)rd32(NM_COL), row = (s32)rd32(NM_ROW);
        u32 cur = (u32)(col << 8 | row);
        if (!s_nm_valid || cur != s_nm_wr) {          /* the pad moved it: continue from its cell */
            s_nm_x = col == 25 ? 22 + 250 + 13 : 22 + 10 * col + 5; s_nm_y = 98 + 20 * row + 8;
            s_nm_wr = cur; s_nm_valid = 1;
            warp_os_pointer(s_nm_x, s_nm_y);
            s_moved = 0;
        }
        int inside = 1;
        if (s_moved) {
            if (abs_move) { s_nm_x = s_vx; s_nm_y = s_vy; } else { s_nm_x += mvx; s_nm_y += mvy; }
            inside = s_nm_x >= 22 && s_nm_x < 22 + 250 + 27 && s_nm_y >= 98 && s_nm_y < 98 + 100;
            if (!abs_move) {                          /* captured: stay on the grid */
                if (s_nm_x < 22) s_nm_x = 22;
                if (s_nm_x > 22 + 250 + 26) s_nm_x = 22 + 250 + 26;
                if (s_nm_y < 98) s_nm_y = 98;
                if (s_nm_y > 98 + 99) s_nm_y = 98 + 99;
                inside = 1;
            }
            if (inside) {
                int c = (int)floor((s_nm_x - 22) / 10), r = (int)floor((s_nm_y - 98) / 20);
                if (c > 25) c = 25;
                if (c < 0) c = 0;
                if (r > 4) r = 4;
                if (r < 0) r = 0;
                if (c < 25 && nm_empty(c, r)) {       /* empty cell: the nearest letter in the row */
                    int best = -1;
                    for (int d = 1; d < 25 && best < 0; d++) {
                        if (c - d >= 0 && !nm_empty(c - d, r)) best = c - d;
                        else if (c + d < 25 && !nm_empty(c + d, r)) best = c + d;
                    }
                    if (best >= 0) c = best;
                }
                if (c != col || r != row) { wr32(NM_COL, (u32)c); wr32(NM_ROW, (u32)r); }
                s_nm_wr = (u32)(c << 8 | r);
            }
        }
        s_moved = 0;
        if (s_left) {
            s_left = 0;
            if ((abs_move || !s_captured) && !(s_vx >= 22 && s_vx < 22 + 250 + 27 && s_vy >= 98 && s_vy < 98 + 100)) return;
            start_pulse(B_CIRCLE); s_cool = 12; return;
        }
        return;
    } else if (ctx == 11) {                           /* item grid: the pointer picks the cell */
        int n = (s32)rd32(IT_COUNT), rows = (n + 1) / 2;
        int top = (s32)rd32(RAM_LIST_TOP), row = (s32)rd32(RAM_LIST_INDEX), col = (s32)rd32(IT_COL);
        u32 cur = (u32)(top << 20 | row << 8 | col);
        if (!s_it_valid || cur != s_it_wr) {          /* the pad moved it: continue from its cell */
            s_it_x = IT_X + IT_W * col + IT_W / 2; s_it_y = IT_Y + IT_H * (row - top) + IT_H / 2;
            s_it_wr = cur; s_it_valid = 1; s_it_push = 0;
            warp_os_pointer(s_it_x, s_it_y);
            s_moved = 0;
        }
        if (s_wheel_up || s_wheel_down) {             /* wheel: one row per notch (the game scrolls) */
            int d = s_wheel_down > s_wheel_up ? 1 : -1; s_wheel_up = s_wheel_down = 0;
            s_moved = 0; start_pulse(d > 0 ? B_DOWN : B_UP); return;
        }
        if (s_moved) {
            if (abs_move) { s_it_x = s_vx; s_it_y = s_vy; } else { s_it_x += mvx; s_it_y += mvy; }
            int inside = s_it_x >= IT_X && s_it_x < IT_X + 2 * IT_W && s_it_y >= IT_Y && s_it_y < IT_Y + 4 * IT_H;
            if (!abs_move) {                          /* captured: stay on the grid, push past it to scroll */
                if (s_it_x < IT_X) s_it_x = IT_X;
                if (s_it_x > IT_X + 2 * IT_W - 1) s_it_x = IT_X + 2 * IT_W - 1;
                if (s_it_y < IT_Y) { if (top > 0) s_it_push += s_it_y - IT_Y; s_it_y = IT_Y; }
                else if (s_it_y > IT_Y + 4 * IT_H - 1) { if (top + 4 < rows) s_it_push += s_it_y - (IT_Y + 4 * IT_H - 1); s_it_y = IT_Y + 4 * IT_H - 1; }
                else s_it_push = 0;
                if (s_it_push <= -IT_H && top > 0) { top--; s_it_push = 0; }
                if (s_it_push >= IT_H && top + 4 < rows) { top++; s_it_push = 0; }
                inside = 1;
            }
            if (inside) {
                int r = top + (int)floor((s_it_y - IT_Y) / IT_H), c = s_it_x >= IT_X + IT_W;
                if (r > rows - 1) r = rows - 1;
                if (r * 2 + c >= n) c = 0;            /* odd count: the last row has one item */
                wr32(RAM_LIST_TOP, (u32)top); wr32(RAM_LIST_INDEX, (u32)r); wr32(IT_COL, (u32)c);
                s_it_wr = (u32)(top << 20 | r << 8 | c);
            }
        }
        s_moved = 0;
        if (s_left) {
            s_left = 0;
            if ((abs_move || !s_captured) && !(s_vx >= IT_X && s_vx < IT_X + 2 * IT_W && s_vy >= IT_Y && s_vy < IT_Y + 4 * IT_H)) return;
            start_pulse(B_CIRCLE); s_cool = 20; return;
        }
        return;
    } else if (ctx == 10) {                           /* SELECT RULER: sideways push / wheel = Left / Right */
        int dir = page_push(mvx, mvy, abs_move);
        if (s_wheel_up || s_wheel_down) { dir = s_wheel_down > s_wheel_up ? 1 : -1; s_wheel_up = s_wheel_down = 0; }
        if (!dir && s_left && (abs_move || !s_captured) && s_vy >= 180 && s_vy < 205 && (s_vx < 40 || (s_vx >= 200 && s_vx < 240))) {
            dir = s_vx < 40 ? -1 : 1;                 /* OS pointer: the red arrows beside the text box */
            s_left = 0;
        }
        s_moved = 0;
        if (dir) { s_hpush = 0; start_pulse(dir > 0 ? B_RIGHT : B_LEFT); return; }
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); s_cool = 20; return; }
        return;
    } else if (ctx == 9) {                            /* DOMAIN map: the box follows the pointer */
        u32 gx = rd32(RAM_DOM_X), gy = rd32(RAM_DOM_Y);
        if (!s_dom_valid || gx != s_dom_wx || gy != s_dom_wy) {   /* the pad moved it: continue from there */
            s_dom_x = (s32)gx / 256.0 + 35; s_dom_y = (s32)gy / 256.0 - 5;
            s_dom_wx = gx; s_dom_wy = gy; s_dom_valid = 1;
            warp_os_pointer(s_dom_x, s_dom_y);
            s_moved = 0;
        }
        if (s_moved) {
            if (abs_move) { s_dom_x = s_vx; s_dom_y = s_vy; } else { s_dom_x += mvx; s_dom_y += mvy; }
            if (s_dom_x < 6144 / 256.0 + 35) s_dom_x = 6144 / 256.0 + 35;
            if (s_dom_x > 0xE700 / 256.0 + 35) s_dom_x = 0xE700 / 256.0 + 35;
            if (s_dom_y < 8192 / 256.0 - 5) s_dom_y = 8192 / 256.0 - 5;
            if (s_dom_y > 0xD700 / 256.0 - 5) s_dom_y = 0xD700 / 256.0 - 5;
            s_dom_wx = (u32)(s32)lround((s_dom_x - 35) * 256); s_dom_wy = (u32)(s32)lround((s_dom_y + 5) * 256);
            wr32(RAM_DOM_X, s_dom_wx); wr32(RAM_DOM_Y, s_dom_wy);
        }
        s_moved = 0;
        if (s_left) {
            /* the game only jumps if its cursor was moved by the pad since the map opened: put the box
             * 1 px to the right and let a one-frame Left bring it back, then Circle */
            s_left = 0;
            wr32(RAM_DOM_X, s_dom_wx + 256);
            start_pulse_n(B_LEFT, 1);
            s_confirm_menu = 0; s_confirm_addr = 0; s_confirm_wait = 2; s_cool = 20;
            return;
        }
    } else if (ctx == 8) {                            /* unit status page 2: the pointer picks the item described */
        int sect = (s32)rd32(RAM_SP_SECT), idx = (s32)rd32(RAM_SP_INDEX);
        u32 key = (u32)(sect << 16 | (idx & 0xFFFF));
        if (!s_sp_valid || key != s_sp_wr) {          /* new page, or the pad moved it: start on the selection */
            SpSect t = sp_sect(sect);
            s_sp_x = t.x + 40; s_sp_y = t.y + 16 * (idx - (t.top ? (s32)rd32(t.top) : 0)) + 8;
            s_sp_wr = key; s_sp_valid = 1; s_sp_push = 0;
        }
        if (s_moved) {
            if (abs_move) { s_sp_x = s_vx; s_sp_y = s_vy; }
            else { s_sp_x += mvx; s_sp_y += mvy; }
            if (s_sp_x < 14) s_sp_x = 14;
            if (s_sp_x > 305) s_sp_x = 305;
            for (int k = 0; k < 3; k++) {
                SpSect t = sp_sect(k);
                if (t.n <= 0 || s_sp_x < t.x || s_sp_x >= t.x + 138) continue;
                int top = t.top ? (s32)rd32(t.top) : 0;
                double y1 = t.y + 16 * t.vis;
                if (s_sp_y < t.y - 8 || s_sp_y >= y1 + 8) continue;
                /* captured: pushing past the first / last visible row scrolls, one row per 16 px */
                if (!abs_move && t.top && s_sp_y >= y1 && top + t.vis < t.n) {
                    s_sp_push += s_sp_y - (y1 - 1); s_sp_y = y1 - 1;
                    if (s_sp_push >= 16) { s_sp_push = 0; wr32(t.top, (u32)++top); }
                } else if (!abs_move && t.top && s_sp_y < t.y && top > 0) {
                    s_sp_push += s_sp_y - t.y; s_sp_y = t.y;
                    if (s_sp_push <= -16) { s_sp_push = 0; wr32(t.top, (u32)--top); }
                } else s_sp_push = 0;
                int r = (int)floor((s_sp_y - t.y) / 16);
                if (r < 0 || r >= t.vis) break;
                if (k != sect) wr32(RAM_SP_SECT, (u32)k);
                if (top + r != idx) wr32(RAM_SP_INDEX, (u32)(top + r));
                s_sp_wr = (u32)(k << 16 | ((top + r) & 0xFFFF));
                break;
            }
            if (!abs_move) {                          /* captured: stay within the lists */
                if (s_sp_y < 79) s_sp_y = 79;
                if (s_sp_y > 206) s_sp_y = 206;
            }
        }
        s_moved = 0;
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); s_cool = 20; return; }
    } else if (ctx == 4) {                            /* scrolling list: the pointer walks the items */
        int cur = list_cur();
        if (s_ld.kind == 4) {                         /* unit list: Left / Right show other columns */
            int row = (s16)rd16(RAM_UL_ROW);
            if (row == 0 && !s_ulh) {                 /* the keyboard put it on the header */
                double x, w; s_ulh = 1; s_ulh_push = 0;
                s_ulh_x = ul_hcol_box((s8)rd8(RAM_UL_HCOL), &x, &w) ? x + w / 2 : 60;
            }
            if (row != 0) s_ulh = 0;
            if (!s_ulh && s_moved && list_top() == 0 && cur == 0) {   /* above the first row: the header */
                int up = abs_move ? (s_vy >= 22 && s_vy < s_ld.y) : (s_list_pos + mvy / s_ld.pitch < 0);
                if (!abs_move && up) { s_ulh_push += mvy; up = s_ulh_push <= -12; } else if (!abs_move) s_ulh_push = 0;
                if (up) {
                    s_ulh = 1; s_ulh_push = 0; s_ulh_x = abs_move ? s_vx : 60;
                    wr16(RAM_UL_ROW, 0);
                }
            }
            if (s_ulh) {                              /* header: x picks the sort column, a click sorts */
                int leave = 0;
                if (s_wheel_down) { s_wheel_down = 0; leave = 1; }
                s_wheel_up = 0;
                if (s_moved) {
                    if (abs_move) { if (s_vy >= s_ld.y) leave = 1; else s_ulh_x = s_vx; }
                    else {
                        s_ulh_x += mvx;
                        if (s_ulh_x < 32) s_ulh_x = 32;
                        if (s_ulh_x > 300) s_ulh_x = 300;
                        s_ulh_push = mvy > 0 ? s_ulh_push + mvy : 0;
                        if (s_ulh_push >= 12) leave = 1;
                    }
                }
                if (leave) {                          /* back to the knights (the OS pointer: the one under it) */
                    s_ulh = 0; s_ulh_push = 0;
                    wr16(RAM_UL_ROW, 1); s_list_pos = list_top() + 0.5; s_list_wr = cur = list_top();
                    if (!abs_move) { s_moved = 0; return; }
                } else s_moved = 0;
            }
            if (s_ulh) {
                int c = ul_hcol_at(s_ulh_x);
                if ((s8)rd8(RAM_UL_HCOL) != c) wr8(RAM_UL_HCOL, (u8)c);
                if (s_left) {
                    s_left = 0;
                    if ((abs_move || !s_captured) && !(s_vy >= 22 && s_vy < s_ld.y)) return;
                    start_pulse(B_CIRCLE); s_cool = 20;
                }
                return;
            }
            int dir = page_push(mvx, mvy, abs_move);
            if (!dir && s_left && (abs_move || !s_captured) && s_vy >= 0 && s_vy < 22 && (s_vx < 60 || s_vx >= 260)) {
                dir = s_vx < 60 ? -1 : 1;             /* OS pointer: the page arrows in the top corners */
                s_left = 0;
            }
            if (dir) { s_hpush = 0; s_moved = 0; start_pulse(dir > 0 ? B_RIGHT : B_LEFT); return; }
        }
        if (!s_list_valid || cur != s_list_wr) {      /* new list, or the game / keyboard moved it */
            s_list_pos = cur + 0.5; s_list_wr = cur; s_list_valid = 1;
        }
        if (s_ld.pad_scroll) {                        /* the game scrolls: Up / Down past the visible rows */
            int top = list_top(), dir = 0;
            if (s_wheel_up || s_wheel_down) {         /* one row per notch, scrolling at the ends */
                int d = s_wheel_down > s_wheel_up ? 1 : -1; s_wheel_up = s_wheel_down = 0;
                if (cur + d >= top && cur + d < top + s_ld.vis) { list_select(cur + d); s_list_pos = cur + d + 0.5; s_moved = 0; }
                else dir = d;
            }
            else if (s_moved && !abs_move) {
                double p = s_list_pos + mvy / s_ld.pitch;
                if (p < top) { s_list_push += p - top; s_list_pos = top + 0.01; }
                else if (p >= top + s_ld.vis) { s_list_push += p - (top + s_ld.vis); s_list_pos = top + s_ld.vis - 0.01; }
                else { s_list_push = 0; s_list_pos = p; }
                if (s_list_push <= -1) dir = -1; else if (s_list_push >= 1) dir = 1;
                if (dir) s_list_push = 0;
                mvy = 0;
                int k = (int)floor(s_list_pos);
                if (k != cur && k >= top && k < top + s_ld.vis) list_select(k);
                if (!dir) s_moved = 0;
            }
            if (dir) {
                s_moved = 0;
                if (dir > 0 ? cur < top + s_ld.vis - 1 : cur > top) list_select(dir > 0 ? top + s_ld.vis - 1 : top);
                start_pulse(dir > 0 ? B_DOWN : B_UP);
                return;
            }
        }
        if (s_wheel_up || s_wheel_down) {             /* wheel: one item per notch */
            s_list_pos += s_wheel_down - s_wheel_up; s_wheel_up = s_wheel_down = 0;
            s_moved = 1; abs_move = 0; mvy = 0;
        }
        if (s_moved) {
            if (abs_move) {
                int k = list_item_at(s_vx, s_vy);
                if (k >= 0) s_list_pos = k + 0.5;
            } else s_list_pos += mvy / s_ld.pitch;
            /* pushing past the first / last visible row scrolls the list */
            if (s_list_pos < 0.01) s_list_pos = 0.01;
            if (s_list_pos > s_ld.count - 0.01) s_list_pos = s_ld.count - 0.01;
            int k = (int)floor(s_list_pos);
            if (k != cur) list_select(k);
        }
        s_moved = 0;
        if (troop && ((s_left && s_left_mod) || s_mid)) {   /* troop picker: Ctrl / Shift click or middle = Square */
            int k = list_cur();
            if (s_left && (abs_move || !s_captured)) {
                int kk = s_vx >= 0 ? list_item_at(s_vx, s_vy) : -1;
                if (kk >= 0) { k = kk; list_select(k); }
            }
            s_left = 0; s_mid = 0;
            start_pulse(B_SQUARE);   /* like the pad: marks, and the game moves the highlight down */
            return;
        }
        if (s_left) {
            s_left = 0;
            int k = list_cur();
            if (abs_move || !s_captured) {            /* OS pointer: the item under it, or close when outside */
                int kk = s_vx >= 0 ? list_item_at(s_vx, s_vy) : -1;
                if (kk < 0) { start_pulse(B_CROSS); return; }
                k = kk; list_select(k);
            }
            s_confirm_menu = k; s_confirm_addr = s_ld.rel ? 0 : s_ld.idx; s_confirm_wait = 2; s_cool = 20;
            return;
        }
        return;
    } else {                                          /* anywhere else: buttons */
        s_moved = 0;
        if (s_left) { s_left = 0; start_pulse(B_CIRCLE); return; }
    }
    if (s_wheel_up) { s_wheel_up--; start_pulse(B_UP); }
    else if (s_wheel_down) { s_wheel_down--; start_pulse(B_DOWN); }
}

u16 mouse_pad_bits(void) { return s_pulse_bits | s_edge_bits | s_ff_bits; }
