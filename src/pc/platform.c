/* SDL2 platform layer: window, presentation of the PS1 display area (OpenGL, video_gl.c), input, pacing,
 * frame skipping, and the settings UI hook (ui.cpp, F2). */
#include "hle.h"
#include "gpu.h"
#include "settings.h"
#include "ui.h"
#include "state.h"
#include "video_gl.h"
#include "gpu_hw.h"
#include "mouse.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static SDL_Window *g_win;
static SDL_GameController *g_pad;
/* controller: Back held + D-pad Left / Right changes the save slot (like Left Shift + arrows), Back + LB
 * quick saves (F5), Back + RB quick loads (F9). While Back is held the game gets neither Select nor
 * Left / Right / L1 / R1; a Back tap without a combo is Select. */
static int g_back_combo;      /* a slot change happened during this Back press */
static int g_select_pulse;    /* frames of Select still to send after a plain Back tap */
static SDL_Joystick *g_vjoy; /* tests (BRIG_VPAD): virtual controller */
static int g_quit, g_show_vram;
static u32 g_frame[VRAM_W * VRAM_H];
static int g_tex_w, g_tex_h;          /* size of the image currently in the GL texture */
static int g_gl_ok;
static Uint64 g_next_frame, g_ideal;
static int g_vsync_clock;            /* 1: display vsync (~60 Hz) is the frame clock */
static int g_refresh_hz;
static const u8 *g_movie_rgb;        /* non-NULL while a movie frame overrides VRAM output */
static int g_movie_w, g_movie_h;

/* frame skip state */
static int g_skip_this_frame;        /* drawing of the game frame that ends at the next vblank is skipped */
static int g_skip_run;               /* consecutive skipped frames */
static int g_fixed_phase;

PlatformStats g_stats;

void platform_set_movie_frame(const u8 *rgb, int w, int h) { g_movie_rgb = rgb; g_movie_w = w; g_movie_h = h; }

/* Apply VSync setting. With a ~60 Hz display and VSync on, presenting is the frame clock;
 * otherwise we sleep to 59.94 Hz ourselves (VSync off = best for VRR displays). */
/* Window / fullscreen and the window size (F2 > Graphics). Fullscreen uses the desktop resolution; the
 * picture inside follows the screen fit. Leaving fullscreen restores the chosen window size. */
static int g_ui_grown;        /* the window was enlarged for the settings window (1x / 2x sizes) */
void platform_ui_window(int open);
void platform_apply_display(void) {
    if (!g_win) return;
    if (g_settings.fullscreen) { SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP); return; }
    SDL_SetWindowFullscreen(g_win, 0);
    g_ui_grown = 0;
    SDL_SetWindowSize(g_win, 320 * g_settings.window_scale, 240 * g_settings.window_scale);
    SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (ui_is_open()) platform_ui_window(1);          /* (shrinks back to the new size when it closes) */          /* changed from the settings window: keep it usable */
}
/* the settings window needs about 660x600: small windows grow while it is open and shrink back to the
 * size they had when it closes, unless the player resized the window meanwhile (that size is kept) */
static int g_pre_w, g_pre_h, g_grown_w, g_grown_h;
void platform_ui_window(int open) {
    if (!g_win || g_settings.fullscreen) return;
    int w, h; SDL_GetWindowSize(g_win, &w, &h);
    if (open && (w < 660 || h < 600)) {
        g_pre_w = w; g_pre_h = h;
        g_grown_w = w < 660 ? 660 : w; g_grown_h = h < 600 ? 600 : h;
        SDL_SetWindowSize(g_win, g_grown_w, g_grown_h);
        g_ui_grown = 1;
    } else if (!open && g_ui_grown) {
        g_ui_grown = 0;
        if (w == g_grown_w && h == g_grown_h) SDL_SetWindowSize(g_win, g_pre_w, g_pre_h);
    }
}

void platform_apply_video(void) {
    if (!g_gl_ok) return;
    vgl_set_vsync(g_settings.vsync);
    g_settings.render_scale = hw_set_scale(g_settings.render_scale);
    g_vsync_clock = g_settings.vsync && g_refresh_hz >= 59 && g_refresh_hz <= 61;
    g_next_frame = g_ideal = SDL_GetPerformanceCounter();
}

int platform_init(const char *title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) return 0;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    int win_w = 320 * g_settings.window_scale, win_h = 240 * g_settings.window_scale;   /* tests: BRIG_WINDOW=WxH */
    if (getenv("BRIG_WINDOW")) sscanf(getenv("BRIG_WINDOW"), "%dx%d", &win_w, &win_h);
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL);
    if (!g_win) { rt_log("[video] window: %s", SDL_GetError()); return 0; }
    if (g_settings.fullscreen) SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    g_gl_ok = vgl_init(g_win);
    if (!g_gl_ok) { rt_log("[video] OpenGL 3.3 is required: %s", SDL_GetError()); return 0; }
    SDL_DisplayMode dm;
    g_refresh_hz = SDL_GetCurrentDisplayMode(SDL_GetWindowDisplayIndex(g_win), &dm) == 0 ? dm.refresh_rate : 0;
    if (getenv("BRIG_VPAD")) {                        /* tests: a virtual controller driven by BRIG_KEYS PadDown= / PadUp= */
        int vi = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, SDL_CONTROLLER_BUTTON_MAX, 0);
        if (vi >= 0) { g_pad = SDL_GameControllerOpen(vi); g_vjoy = SDL_JoystickOpen(vi); }
    }
    for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++)
        if (SDL_IsGameController(i)) { g_pad = SDL_GameControllerOpen(i); break; }
    if (getenv("BRIG_SHOW_VRAM")) g_show_vram = 1;
    platform_apply_video();
    if (getenv("BRIG_TRACE")) rt_log("[video] display %d Hz, vsync=%d, vsync clock=%d, frameskip=%d",
                                     g_refresh_hz, g_settings.vsync, g_vsync_clock, g_settings.frameskip);
    ui_init(g_win, vgl_context());
    return 1;
}

int platform_quit_requested(void) { return g_quit; }

static inline u32 c15(u16 p) {
    u32 r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static int build_frame(void);

/* 160x120 RGB snapshot of the image currently on screen (for save-state thumbnails). */
void platform_thumbnail(u8 *rgb, int w, int h) {
    if (g_hw_on) build_frame();           /* the hardware renderer does not fill g_frame */
    if (!g_tex_w || !g_tex_h) { memset(rgb, 0, (size_t)w * h * 3); return; }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            u32 p = g_frame[(y * g_tex_h / h) * g_tex_w + x * g_tex_w / w];
            u8 *o = rgb + (y * w + x) * 3;
            o[0] = (u8)(p >> 16); o[1] = (u8)(p >> 8); o[2] = (u8)p;
        }
}

/* Convert the current PS1 display (or movie frame) into g_frame (g_tex_w x g_tex_h). */
static int build_frame(void) {
    int x0, y0, w, h;
    if (g_show_vram) { x0 = 0; y0 = 0; w = VRAM_W; h = VRAM_H; }
    else { x0 = g_gpu.disp_x; y0 = g_gpu.disp_y; w = g_gpu.disp_w; h = g_gpu.disp_h; }
    /* movies: letterbox into a 240-line screen like the PS1 display (e.g. 320x192 -> bars top/bottom) */
    int top = 0;
    if (g_movie_rgb && !g_show_vram) { w = g_movie_w; h = g_movie_h < 240 ? 240 : g_movie_h; top = (h - g_movie_h) / 2; }
    if (w <= 0 || h <= 0) return 0;
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
    g_tex_w = w; g_tex_h = h;
    return 1;
}

/* ... and upload it as the image to present */
static void upload_frame(void) {
    if (build_frame()) vgl_upload(g_frame, g_tex_w, g_tex_h);
}

static void present(int skipped) {
    /* hardware renderer: the upscaled display area (movies and 24-bit screens use the software image) */
    unsigned htex; int hw_w, hw_h;
    if (g_hw_on && !g_movie_rgb && !g_gpu.disp_24bit && hw_display_texture(&htex, &hw_w, &hw_h, g_show_vram))
        vgl_use_texture(htex, hw_w, hw_h);
    /* a skipped game frame left a stale buffer on screen: keep showing the previous image */
    else if (!skipped || g_movie_rgb || g_show_vram || !g_tex_w || g_hw_on) upload_frame();
    int ow, oh;
    vgl_drawable_size(&ow, &oh);
    /* screen fit (F3): original 4:3 letterboxed in the window, integer multiples of 320x240 (sharp,
     * even pixels; falls back to 4:3 when the window is smaller than 320x240), or stretched over it */
    double aspect = g_show_vram ? 2.0 : 4.0 / 3.0;
    int dw = ow, dh = (int)(ow / aspect);
    if (dh > oh) { dh = oh; dw = (int)(oh * aspect); }
    if (!g_show_vram && g_settings.screen_fit == 1) {
        int k = ow / 320 < oh / 240 ? ow / 320 : oh / 240;
        if (k >= 1) { dw = 320 * k; dh = 240 * k; }
    } else if (!g_show_vram && g_settings.screen_fit == 2) { dw = ow; dh = oh; }
    vgl_draw((ow - dw) / 2, (oh - dh) / 2, dw, dh);
    {   int ww, wh; SDL_GetWindowSize(g_win, &ww, &wh);
        mouse_set_view((ow - dw) / 2, (oh - dh) / 2, dw, dh, ww ? (float)ow / ww : 1.0f); }
    ui_frame();
    /* debug: BRIG_SHOT_DIR=<dir> BRIG_SHOT_EVERY=<frames> dumps BMP snapshots of the final window image */
    static int frame_no;
    const char *dir = getenv("BRIG_SHOT_DIR"), *ev = getenv("BRIG_SHOT_EVERY");
    const char *sf_ = getenv("BRIG_SHOT_FROM"), *st_ = getenv("BRIG_SHOT_TO");
    ++frame_no;
    {   /* debug: BRIG_GPUDUMP=<n> logs the GP0 packets that build the image shown at snapshot frame n */
        static int dump = -2, dump_to = -1;   /* BRIG_GPUDUMP=<n> or <a>-<b> */
        if (dump == -2) {
            const char *e2 = getenv("BRIG_GPUDUMP");
            dump = e2 ? atoi(e2) : -1;
            const char *dash = e2 ? strchr(e2, '-') : NULL;
            dump_to = dash ? atoi(dash + 1) : dump;
        }
        g_gpu_dump = (frame_no + 1 >= dump && frame_no + 1 <= dump_to);
        if (g_gpu_dump) rt_log("[gp0] ---- frame %d cursor col=%u row=%u cam %d %d ofs %d %d", frame_no + 1, rd8(0x800E7C48), rd8(0x800E7C50), (int)rd32(0x800A6AB4), (int)rd32(0x800A6AB8), g_gpu.ofs_x, g_gpu.ofs_y);
    }
    int in_range = (!sf_ || frame_no >= atoi(sf_)) && (!st_ || frame_no <= atoi(st_));
    if (dir && ev && atoi(ev) > 0 && in_range && (frame_no % atoi(ev)) == 0) {
        SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        if (sf && sf->pitch == ow * 4 && vgl_read_pixels((u32 *)sf->pixels, ow, oh)) {
            char path[512];
            snprintf(path, sizeof path, "%s/shot_%05d.bmp", dir, frame_no);
            SDL_SaveBMP(sf, path);
        }
        if (sf) SDL_FreeSurface(sf);
    }
    vgl_swap();
}

/* debug: BRIG_KEYS="frame:KeyName,..." injects key presses (SDL key names, e.g. F2, Down, Right, Escape) */
static void inject_keys(void) {
    static int frame;
    const char *s = getenv("BRIG_KEYS");
    static int mod_frames;
    frame++;
    if (mod_frames > 0 && --mod_frames == 0) SDL_SetModState(KMOD_NONE);
    if (!s) return;
    while (*s) {
        int f = atoi(s);
        const char *colon = strchr(s, ':');
        if (!colon) break;
        char name[32]; int n = 0;
        const char *p = colon + 1;
        while (*p && *p != ',' && n < 31) name[n++] = *p++;
        name[n] = 0;
        if (f == frame && !strncmp(name, "Rel=", 4)) {   /* scripted relative mouse motion (points) */
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = SDL_MOUSEMOTION; ev.motion.xrel = atoi(name + 4);
            ev.motion.yrel = strchr(name, ';') ? atoi(strchr(name, ';') + 1) : 0;
            ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
        } else if (f == frame && (!strncmp(name, "PadDown=", 8) || !strncmp(name, "PadUp=", 6))) {
            /* tests (BRIG_VPAD): virtual controller button, by SDL_GameControllerButton number */
            if (g_vjoy) SDL_JoystickSetVirtualButton(g_vjoy, atoi(strchr(name, '=') + 1), name[3] == 'D');
        } else if (f == frame && (!strncmp(name, "LDown", 5) || !strncmp(name, "LUp", 3))) {
            /* scripted left button held / released (LClick is a press and release) */
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = name[1] == 'D' ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
        } else if (f == frame && (!strncmp(name, "LClick", 6) || !strncmp(name, "RBtn", 4) ||
                                  !strncmp(name, "MBtn", 4) || !strncmp(name, "CClick", 6) ||
                                  !strncmp(name, "XBtn", 4))) {
            /* button only: LClick, RBtn, MBtn (middle), CClick (left with Ctrl held), XBtn (side) */
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            if (name[0] == 'C') { SDL_SetModState(KMOD_LCTRL); mod_frames = 2; }
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = name[0] == 'R' ? SDL_BUTTON_RIGHT : name[0] == 'M' ? SDL_BUTTON_MIDDLE
                             : name[0] == 'X' ? SDL_BUTTON_X1 : SDL_BUTTON_LEFT;
            ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
            ev.type = SDL_MOUSEBUTTONUP; ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
        } else if (f == frame && (!strncmp(name, "Move=", 5) || !strncmp(name, "Click=", 6) || !strncmp(name, "RClick=", 7))) {
            /* scripted mouse (window points): Move=x;y, Click=x;y, RClick=x;y */
            const char *xy = strchr(name, '=') + 1;
            int mx = atoi(xy), my = strchr(xy, ';') ? atoi(strchr(xy, ';') + 1) : 0;
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            ev.type = SDL_MOUSEMOTION; ev.motion.x = mx; ev.motion.y = my;
            ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
            if (name[0] != 'M') {
                memset(&ev, 0, sizeof ev);
                ev.type = SDL_MOUSEBUTTONDOWN; ev.button.x = mx; ev.button.y = my;
                ev.button.button = name[0] == 'R' ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
                ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
                ev.type = SDL_MOUSEBUTTONUP; ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
            }
        } else if (f == frame) {
            SDL_Event ev; memset(&ev, 0, sizeof ev);
            int shift = !strncmp(name, "Shift+", 6);   /* "Shift+Right" = with Left Shift held */
            ev.type = SDL_KEYDOWN; ev.key.keysym.sym = SDL_GetKeyFromName(shift ? name + 6 : name);
            if (shift) ev.key.keysym.mod = KMOD_LSHIFT;
            ev.key.keysym.scancode = SDL_GetScancodeFromKey(ev.key.keysym.sym);
            ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
            ev.type = SDL_KEYUP;
            ev.button.windowID = g_win ? SDL_GetWindowID(g_win) : 0; SDL_PushEvent(&ev);
        }
        if (!*p) break;
        s = p + 1;
    }
}

/* Decide whether the game frame that starts now should skip rasterising. */
static void decide_frameskip(Uint64 now, Uint64 period) {
    int mode = g_settings.frameskip, skip = 0;
    if (mode == 1) {                                  /* auto: skip while more than a frame late */
        Sint64 late = (Sint64)(now - g_ideal);
        if (late > (Sint64)period && g_skip_run < 3) skip = 1;
    } else if (mode >= 2) {                           /* fixed: draw 1, skip (mode-1) */
        g_fixed_phase = (g_fixed_phase + 1) % mode;
        skip = g_fixed_phase != 0;
    }
    if (g_movie_rgb) skip = 0;
    g_skip_run = skip ? g_skip_run + 1 : 0;
    g_skip_this_frame = skip;
    g_gpu_skip_draw = skip;
}

/* debug: BRIG_POKE="frame:addr=value,..." writes 32-bit words into PS1 RAM at a frame (hex addr/value) */
static void poke_ram(void) {
    static int frame;
    const char *s = getenv("BRIG_POKE");
    frame++;
    if (!s) return;
    while (*s) {
        int f = atoi(s);
        const char *colon = strchr(s, ':'), *eq = colon ? strchr(colon, '=') : NULL;
        if (!colon || !eq) break;
        if (f == frame) wr32((u32)strtoul(colon + 1, NULL, 16), (u32)strtoul(eq + 1, NULL, 16));
        const char *comma = strchr(eq, ',');
        if (!comma) break;
        s = comma + 1;
    }
}

/* debug: BRIG_RAMDUMP="frame,frame,..." writes the 2 MB of PS1 RAM to <BRIG_DEBUG_DIR or .>/ram_<frame>.bin */
static void dump_ram(void) {
    static int frame;
    const char *s = getenv("BRIG_RAMDUMP");
    frame++;
    if (!s) return;
    for (const char *p = s; *p; ) {
        if (atoi(p) == frame) {
            const char *dir = getenv("BRIG_DEBUG_DIR");
            char path[1024]; snprintf(path, sizeof path, "%s/ram_%05d.bin", dir ? dir : ".", frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(g_ram, 1, RAM_SIZE, f); fclose(f); }
        }
        const char *comma = strchr(p, ',');
        if (!comma) break;
        p = comma + 1;
    }
}

void platform_vblank(void) {
    SDL_Event e;
    inject_keys();
    poke_ram();
    dump_ram();
    if (g_select_pulse > 0) g_select_pulse--;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g_quit = 1;
        if (ui_event(&e)) continue;                   /* settings UI (F2) consumed it */
        if (mouse_event(&e)) continue;
        if (e.type == SDL_KEYDOWN) {                  /* keyboard d-pad takes over from the mouse */
            SDL_Keycode kc = e.key.keysym.sym;
            if (kc == SDLK_UP || kc == SDLK_DOWN || kc == SDLK_LEFT || kc == SDLK_RIGHT) mouse_cancel();
        }
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F1) g_show_vram ^= 1;
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F3) {   /* screen fit, like RetroArch */
            static const char *names[] = { "Screen: original 4:3", "Screen: integer scale", "Screen: stretch" };
            g_settings.screen_fit = (g_settings.screen_fit + 1) % 3;
            if (!getenv("BRIG_STATE_DIR")) settings_save();
            ui_toast(names[g_settings.screen_fit]);
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F5) state_request_save(state_slot());
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F9) state_request_load(state_slot());
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_F10 && g_hw_on) {
            /* diagnostics: VRAM images into <pref>/debug/ + states/debug.state */
            char *pref = SDL_GetPrefPath("PortGamePC", "Brigandine"), dir[1100];
            const char *env = getenv("BRIG_DEBUG_DIR");                 /* tests */
            if (env) snprintf(dir, sizeof dir, "%s/", env);
            else snprintf(dir, sizeof dir, "%sdebug/", pref ? pref : "");
            if (pref) SDL_free(pref);
#ifdef _WIN32
            _mkdir(dir);
#else
            mkdir(dir, 0755);
#endif
            if (hw_debug_dump(dir)) { state_request_save(STATE_DEBUG_SLOT); ui_toast("Renderer debug dump written"); }
        }
        if (e.type == SDL_KEYDOWN && (e.key.keysym.mod & KMOD_LSHIFT) &&
            (e.key.keysym.sym == SDLK_LEFT || e.key.keysym.sym == SDLK_RIGHT))
            state_cycle_slot(e.key.keysym.sym == SDLK_LEFT ? -1 : 1);   /* Left Shift + arrows: save slot */
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat && getenv("BRIG_TRACE")) {
            SDL_Keycode kc = e.key.keysym.sym;
            if (kc == SDLK_UP || kc == SDLK_DOWN || kc == SDLK_LEFT || kc == SDLK_RIGHT)
                rt_log("[input] t=%u ms (event t=%u) %s %s", SDL_GetTicks(), e.key.timestamp,
                       SDL_GetKeyName(kc), e.type == SDL_KEYDOWN ? "DOWN" : "UP");
        }
        if (e.type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e.cdevice.which);
        if (e.type == SDL_CONTROLLERBUTTONDOWN && g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
            (e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
            state_cycle_slot(e.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);   /* Back + D-pad: save slot */
            g_back_combo = 1;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
            (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) state_request_save(state_slot());   /* Back + LB = F5 */
            else state_request_load(state_slot());                                                        /* Back + RB = F9 */
            g_back_combo = 1;
        }
        if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) g_back_combo = 0;
        if (e.type == SDL_CONTROLLERBUTTONUP && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK && !g_back_combo)
            g_select_pulse = 6;                                       /* a plain tap: Select for the game */
    }
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    Uint64 freq = SDL_GetPerformanceFrequency(), period = freq * 1001 / 60000;   /* NTSC 59.94 Hz */

    /* stats: game-thread busy time (since last present returned) and frame rate */
    static Uint64 last_wake, stat_t0; static double busy_sum, busy_max; static int nfr, nskip;
    Uint64 t = SDL_GetPerformanceCounter();
    if (last_wake) { double b = (double)(t - last_wake) * 1000.0 / freq; busy_sum += b; if (b > busy_max) busy_max = b; nfr++; }
    nskip += g_skip_this_frame;
    if (!stat_t0) stat_t0 = t;
    if (t - stat_t0 >= freq) {
        g_stats.fps = nfr * (double)freq / (double)(t - stat_t0);
        g_stats.busy_avg_ms = nfr ? busy_sum / nfr : 0;
        g_stats.busy_max_ms = busy_max;
        g_stats.skipped_per_s = nskip;
        if (getenv("BRIG_TRACE")) {
            extern volatile u32 g_stat_xa_underruns, g_stat_clipped;
            rt_log("[perf] %.1f fps | busy avg %.1f ms max %.1f ms | skipped %d | xa underruns %u | clipped samples %u",
                   g_stats.fps, g_stats.busy_avg_ms, busy_max, nskip, g_stat_xa_underruns, g_stat_clipped);
            g_stat_xa_underruns = 0; g_stat_clipped = 0;
        }
        busy_sum = busy_max = 0; nfr = nskip = 0; stat_t0 = t;
    }

    /* pacing. Hold TAB to fast-forward (or BRIG_FAST=1 for tests). */
    g_next_frame += period;
    g_ideal += period;
    Uint64 now = SDL_GetPerformanceCounter();
    static int ff, fast_env = -1;
    if (fast_env < 0) fast_env = getenv("BRIG_FAST") != NULL;
    int want_ff = k[SDL_SCANCODE_TAB] || fast_env;
    if (want_ff != ff) { ff = want_ff; vgl_set_vsync(ff ? 0 : g_settings.vsync); }
    if (ff) {
        g_next_frame = g_ideal = now;
    } else if (!g_vsync_clock) {
        if (g_next_frame > now) SDL_Delay((Uint32)((g_next_frame - now) * 1000 / freq));
        else if (now - g_next_frame > freq / 4) g_next_frame = now;
    }
    if ((Sint64)(now - g_ideal) > (Sint64)(freq / 4) || (Sint64)(g_ideal - now) > (Sint64)(freq / 4)) g_ideal = now;
    mouse_update();
    int skipped = g_skip_this_frame;
    present(skipped);
    last_wake = SDL_GetPerformanceCounter();
    decide_frameskip(last_wake, period);
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
    if (ui_is_open()) return 0;   /* settings UI has the keyboard and pad */
    if (g_select_pulse > 0) b |= 1 << 0;
    b |= mouse_pad_bits();
    if (k[SDL_SCANCODE_BACKSPACE] || k[SDL_SCANCODE_RSHIFT]) b |= 1 << 0;
    if (k[SDL_SCANCODE_RETURN]) b |= 1 << 3;
    if (!k[SDL_SCANCODE_LSHIFT]) {   /* Left Shift + arrows changes the save slot instead */
        if (k[SDL_SCANCODE_UP]) b |= 1 << 4;
        if (k[SDL_SCANCODE_RIGHT]) b |= 1 << 5;
        if (k[SDL_SCANCODE_DOWN]) b |= 1 << 6;
        if (k[SDL_SCANCODE_LEFT]) b |= 1 << 7;
    }
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
        int back = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK);
        for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) {
            if (back && (map[i].bit == 0 || map[i].bit == 5 || map[i].bit == 7 || map[i].bit == 10 || map[i].bit == 11))
                continue;                                 /* Back combos (see g_back_combo) */
            if (SDL_GameControllerGetButton(g_pad, map[i].btn)) b |= 1 << map[i].bit;
        }
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) b |= 1 << 8;
        if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) b |= 1 << 9;
    }
    return b;
}
