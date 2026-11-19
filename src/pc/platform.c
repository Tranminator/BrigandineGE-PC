/* SDL2 platform layer: window, presentation of the PS1 display area, input, pacing. */
#include "hle.h"
#include "gpu.h"
#include "settings.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static SDL_GameController *g_pad;
static int g_quit, g_show_vram;
static u32 g_frame[VRAM_W * VRAM_H];
static Uint64 g_next_frame;
static Uint64 *g_perf_mark;
static int g_vsync_clock;
static const u8 *g_movie_rgb;        /* non-NULL while a movie frame overrides VRAM output */
static int g_movie_w, g_movie_h;

void platform_set_movie_frame(const u8 *rgb, int w, int h) { g_movie_rgb = rgb; g_movie_w = w; g_movie_h = h; }

int platform_init(const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) return 0;
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    /* With a ~60 Hz display, presenting with vsync is the frame clock (no tearing, no drift).
     * Other refresh rates (e.g. 120 Hz ProMotion): sleep to 59.94 Hz first, then present on vsync. */
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_win), &dm) == 0 && dm.refresh_rate >= 59 && dm.refresh_rate <= 61)
        g_vsync_clock = 1;
    if (getenv("BRIG_TRACE")) rt_log("[video] display %d Hz, vsync clock=%d", dm.refresh_rate, g_vsync_clock);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VRAM_W, VRAM_H);
    SDL_RenderSetLogicalSize(g_ren, 640, 480);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
    g_next_frame = SDL_GetPerformanceCounter();
    if (getenv("BRIG_SHOW_VRAM")) g_show_vram = 1;
    return g_win && g_ren && g_tex;
}

int platform_quit_requested(void) { return g_quit; }

static inline u32 c15(u16 p) {
    u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}


/* ------------------------------------------------------------------ sound settings overlay (F2)
 * Up/Down select, Left/Right -/+5%, F2 or Esc closes and saves. Game input is blocked while open. */
static int g_ovl_open, g_ovl_sel;

static const struct { char c; const char *rows[7]; } FONT[] = {
    {'S',{".####","#....","#....",".###.","....#","....#","####."}}, {'O',{".###.","#...#","#...#","#...#","#...#","#...#",".###."}},
    {'U',{"#...#","#...#","#...#","#...#","#...#","#...#",".###."}}, {'N',{"#...#","##..#","#.#.#","#..##","#...#","#...#","#...#"}},
    {'D',{"####.","#...#","#...#","#...#","#...#","#...#","####."}}, {'B',{"####.","#...#","#...#","####.","#...#","#...#","####."}},
    {'G',{".###.","#...#","#....","#.###","#...#","#...#",".###."}}, {'M',{"#...#","##.##","#.#.#","#.#.#","#...#","#...#","#...#"}},
    {'F',{"#####","#....","#....","####.","#....","#....","#...."}}, {'X',{"#...#","#...#",".#.#.","..#..",".#.#.","#...#","#...#"}},
    {'V',{"#...#","#...#","#...#","#...#","#...#",".#.#.","..#.."}}, {'I',{".###.","..#..","..#..","..#..","..#..","..#..",".###."}},
    {'E',{"#####","#....","#....","####.","#....","#....","#####"}}, {'0',{".###.","#...#","#..##","#.#.#","##..#","#...#",".###."}},
    {'1',{"..#..",".##..","..#..","..#..","..#..","..#..",".###."}}, {'2',{".###.","#...#","....#","...#.","..#..",".#...","#####"}},
    {'3',{"#####","...#.","..#..","...#.","....#","#...#",".###."}}, {'4',{"...#.","..##.",".#.#.","#..#.","#####","...#.","...#."}},
    {'5',{"#####","#....","####.","....#","....#","#...#",".###."}}, {'6',{"..##.",".#...","#....","####.","#...#","#...#",".###."}},
    {'7',{"#####","....#","...#.","..#..",".#...",".#...",".#..."}}, {'8',{".###.","#...#","#...#",".###.","#...#","#...#",".###."}},
    {'9',{".###.","#...#","#...#",".####","....#","...#.",".##.."}}, {'%',{"##..#","##..#","...#.","..#..",".#...","#..##","#..##"}},
    {'>',{".#...","..#..","...#.","....#","...#.","..#..",".#..."}},
};

static void draw_text(const char *t, int x, int y, int sc) {
    for (; *t; t++, x += 6 * sc) {
        for (unsigned g = 0; g < sizeof FONT / sizeof FONT[0]; g++) {
            if (FONT[g].c != *t) continue;
            for (int r = 0; r < 7; r++)
                for (int c = 0; c < 5; c++)
                    if (FONT[g].rows[r][c] == '#') { SDL_Rect px = { x + c * sc, y + r * sc, sc, sc }; SDL_RenderFillRect(g_ren, &px); }
        }
    }
}

static void draw_overlay(void) {
    static const char *names[3] = { "BGM", "SFX", "MOVIE" };
    int vals[3] = { g_settings.bgm, g_settings.sfx, g_settings.movie };
    SDL_RenderSetLogicalSize(g_ren, 640, 480);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 200);
    SDL_Rect bg = { 120, 130, 400, 220 };
    SDL_RenderFillRect(g_ren, &bg);
    SDL_SetRenderDrawColor(g_ren, 200, 170, 90, 255);
    SDL_RenderDrawRect(g_ren, &bg);
    draw_text("SOUND", 260, 145, 4);
    for (int i = 0; i < 3; i++) {
        int y = 205 + i * 45;
        SDL_SetRenderDrawColor(g_ren, i == g_ovl_sel ? 255 : 170, i == g_ovl_sel ? 220 : 170, i == g_ovl_sel ? 120 : 170, 255);
        if (i == g_ovl_sel) draw_text(">", 135, y, 3);
        draw_text(names[i], 160, y, 3);
        SDL_Rect frame = { 270, y, 150, 21 }, fill = { 272, y + 2, 146 * vals[i] / 100, 17 };
        SDL_RenderDrawRect(g_ren, &frame);
        SDL_RenderFillRect(g_ren, &fill);
        char num[8]; snprintf(num, sizeof num, "%d%%", vals[i]);
        draw_text(num, 435, y, 3);
    }
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
}

static void overlay_key(SDL_Keycode k) {
    int *v[3] = { &g_settings.bgm, &g_settings.sfx, &g_settings.movie };
    if (k == SDLK_UP) g_ovl_sel = (g_ovl_sel + 2) % 3;
    else if (k == SDLK_DOWN) g_ovl_sel = (g_ovl_sel + 1) % 3;
    else if (k == SDLK_LEFT || k == SDLK_RIGHT) {
        int nv = *v[g_ovl_sel] + (k == SDLK_RIGHT ? 5 : -5);
        *v[g_ovl_sel] = nv < 0 ? 0 : nv > 100 ? 100 : nv;
        settings_apply();
    } else if (k == SDLK_ESCAPE || k == SDLK_F2) {
        g_ovl_open = 0;
        settings_save();
    }
}

static void present(void) {
    int x0, y0, w, h;
    if (g_show_vram) { x0 = 0; y0 = 0; w = VRAM_W; h = VRAM_H; }
    else { x0 = g_gpu.disp_x; y0 = g_gpu.disp_y; w = g_gpu.disp_w; h = g_gpu.disp_h; }
    /* movies: letterbox into a 240-line screen like the PS1 display (e.g. 320x192 -> bars top/bottom) */
    int top = 0;
    if (g_movie_rgb && !g_show_vram) { w = g_movie_w; h = g_movie_h < 240 ? 240 : g_movie_h; top = (h - g_movie_h) / 2; }
    if (w <= 0 || h <= 0) return;
    if (g_movie_rgb && !g_show_vram) {
        for (int i = 0; i < w * h; i++) g_frame[i] = 0xFF000000u;
        for (int y = 0; y < g_movie_h; y++)
            for (int x = 0; x < w; x++) {
                const u8 *p = g_movie_rgb + (y * w + x) * 3;
                g_frame[(y + top) * w + x] = 0xFF000000u | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
    } else if (!g_show_vram && g_gpu.disp_24bit) {
        const u8 *v = (const u8 *)g_vram;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const u8 *p = v + (((y0 + y) & 511) * VRAM_W * 2) + ((x0 * 2 + x * 3) % (VRAM_W * 2));
                g_frame[y * w + x] = 0xFF000000u | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
            }
    } else {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                g_frame[y * w + x] = (g_gpu.disp_enabled || g_show_vram)
                    ? c15(g_vram[((y0 + y) & 511) * VRAM_W + ((x0 + x) & 1023)]) : 0xFF000000u;
    }
    SDL_Rect src = { 0, 0, w, h };
    SDL_UpdateTexture(g_tex, &src, g_frame, w * 4);
    SDL_RenderSetLogicalSize(g_ren, g_show_vram ? VRAM_W : 640, g_show_vram ? VRAM_H : 480);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, &src, NULL);
    if (g_ovl_open) draw_overlay();
    /* debug: BRIG_SHOT_DIR=<dir> BRIG_SHOT_EVERY=<frames> dumps BMP snapshots of the final window image */
    static int frame_no;
    const char *dir = getenv("BRIG_SHOT_DIR"), *ev = getenv("BRIG_SHOT_EVERY");
    const char *sf_ = getenv("BRIG_SHOT_FROM"), *st_ = getenv("BRIG_SHOT_TO");
    ++frame_no;
    int in_range = (!sf_ || frame_no >= atoi(sf_)) && (!st_ || frame_no <= atoi(st_));
    if (dir && ev && atoi(ev) > 0 && in_range && (frame_no % atoi(ev)) == 0) {
        int ow, oh;
        SDL_GetRendererOutputSize(g_ren, &ow, &oh);
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (sf && SDL_RenderReadPixels(g_ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels, sf->pitch) == 0) {
            char path[512];
            snprintf(path, sizeof path, "%s/shot_%05d.bmp", dir, frame_no);
            SDL_SaveBMP(sf, path);
        }
        if (sf) SDL_FreeSurface(sf);
    }
    SDL_RenderPresent(g_ren);
}

/* debug: BRIG_KEYS="frame:KeyName,..." injects key presses (SDL key names, e.g. F2, Down, Right, Escape) */
static void inject_keys(void) {
    static int frame;
    const char *s = getenv("BRIG_KEYS");
    frame++;
    if (!s) return;
    while (*s) {
        int f = atoi(s);
        const char *colon = strchr(s, ':');
        if (!colon) break;
        char name[32]; int n = 0;
        const char *p = colon + 1;
        while (*p && *p != ',' && n < 31) name[n++] = *p++;
        name[n] = 0;
        if (f == frame) {
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = SDL_KEYDOWN; ev.key.keysym.sym = SDL_GetKeyFromName(name);
            SDL_PushEvent(&ev);
        }
        if (!*p) break;
        s = p + 1;
    }
}

void platform_vblank(void) {
    SDL_Event e;
    inject_keys();
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = 1;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F1) g_show_vram ^= 1;
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat && getenv("BRIG_TRACE")) {
            SDL_Keycode kc = e.key.keysym.sym;
            if (kc == SDLK_UP || kc == SDLK_DOWN || kc == SDLK_LEFT || kc == SDLK_RIGHT)
                rt_log("[input] t=%u ms (event t=%u) %s %s", SDL_GetTicks(), e.key.timestamp,
                       SDL_GetKeyName(kc), e.type == SDL_KEYDOWN ? "DOWN" : "UP");
        }
        if (e.type == SDL_KEYDOWN) {
            if (g_ovl_open) overlay_key(e.key.keysym.sym);
            else if (e.key.keysym.sym == SDLK_F2) { g_ovl_open = 1; g_ovl_sel = 0; }
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
    }
    /* NTSC pacing: 59.94 Hz. Hold TAB to fast-forward. Present happens after the sleep so the
     * vsync wait starts right at the frame deadline. */
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    Uint64 freq = SDL_GetPerformanceFrequency();
    {   /* debug stats: game-thread busy time per frame (excludes our sleep) */
        static Uint64 last_wake; static double busy_sum, busy_max; static int nfr;
        Uint64 t = SDL_GetPerformanceCounter();
        if (last_wake) { double b = (double)(t - last_wake) * 1000.0 / freq; busy_sum += b; if (b > busy_max) busy_max = b; nfr++; }
        if (nfr == 60 && getenv("BRIG_TRACE")) {
            extern volatile u32 g_stat_xa_underruns, g_stat_clipped;
            rt_log("[perf] busy avg %.1f ms max %.1f ms | xa underruns %u | clipped samples %u",
                   busy_sum / nfr, busy_max, g_stat_xa_underruns, g_stat_clipped);
            busy_sum = busy_max = 0; nfr = 0; g_stat_xa_underruns = 0; g_stat_clipped = 0;
        }
        g_perf_mark = &last_wake;
    }
    g_next_frame += freq * 1001 / 60000;
    Uint64 now = SDL_GetPerformanceCounter();
    static int ff, fast_env = -1;   /* fast-forward: drop vsync while TAB is held (or always with BRIG_FAST=1) */
    if (fast_env < 0) fast_env = getenv("BRIG_FAST") != NULL;
    int want_ff = k[SDL_SCANCODE_TAB] || fast_env;
    if (want_ff != ff) { ff = want_ff; SDL_RenderSetVSync(g_ren, !ff); }
    if (ff) {
        g_next_frame = now;
    } else if (!g_vsync_clock) {
        if (g_next_frame > now) SDL_Delay((Uint32)((g_next_frame - now) * 1000 / freq));
        else if (now - g_next_frame > freq / 4) g_next_frame = now;
    }
    present();
    if (g_perf_mark) *g_perf_mark = SDL_GetPerformanceCounter();
}

/* PS1 bits: 0 Select 3 Start 4 Up 5 Right 6 Down 7 Left 8 L2 9 R2 10 L1 11 R1
 *           12 Triangle 13 Circle 14 Cross 15 Square */
/* debug: BRIG_INPUT="frame:hexmask,frame:hexmask,..." holds mask for 8 frames at each frame */
static u16 scripted_buttons(void) {
    static int frame;
    const char *s = getenv("BRIG_INPUT");
    frame++;
    if (!s) return 0;
    while (*s) {
        int f = atoi(s);
        const char *colon = strchr(s, ':');
        if (!colon) break;
        unsigned m = (unsigned)strtoul(colon + 1, NULL, 16);
        if (frame >= f && frame < f + 8) return (u16)m;
        const char *comma = strchr(colon, ',');
        if (!comma) break;
        s = comma + 1;
    }
    return 0;
}

u16 platform_pad_buttons(void) {
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    u16 b = scripted_buttons();
    if (g_ovl_open) return 0;   /* settings overlay has the keyboard */
    if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) b |= 1 << 0;
    if (k[SDL_SCANCODE_RETURN]) b |= 1 << 3;
    if (k[SDL_SCANCODE_UP]) b |= 1 << 4;
    if (k[SDL_SCANCODE_RIGHT]) b |= 1 << 5;
    if (k[SDL_SCANCODE_DOWN]) b |= 1 << 6;
    if (k[SDL_SCANCODE_LEFT]) b |= 1 << 7;
    if (k[SDL_SCANCODE_1]) b |= 1 << 8;
    if (k[SDL_SCANCODE_3]) b |= 1 << 9;
    if (k[SDL_SCANCODE_Q]) b |= 1 << 10;
    if (k[SDL_SCANCODE_E]) b |= 1 << 11;
    if (k[SDL_SCANCODE_S]) b |= 1 << 12;  /* Triangle */
    if (k[SDL_SCANCODE_X]) b |= 1 << 13;  /* Circle */
    if (k[SDL_SCANCODE_Z]) b |= 1 << 14;  /* Cross */
    if (k[SDL_SCANCODE_A]) b |= 1 << 15;  /* Square */
    if (g_pad) {
        static const struct { SDL_GameControllerButton btn; int bit; } map[] = {
            { SDL_CONTROLLER_BUTTON_BACK, 0 }, { SDL_CONTROLLER_BUTTON_START, 3 },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, 4 }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 5 },
            { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 6 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 7 },
            { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 10 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 11 },
            { SDL_CONTROLLER_BUTTON_Y, 12 }, { SDL_CONTROLLER_BUTTON_B, 13 },
            { SDL_CONTROLLER_BUTTON_A, 14 }, { SDL_CONTROLLER_BUTTON_X, 15 },
        };
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GameControllerGetButton(g_pad, map[i].btn)) b |= 1 << map[i].bit;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= 1 << 8;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= 1 << 9;
    }
    return b;
}
