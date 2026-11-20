/* User settings (sound volumes), persisted in the OS per-user config folder:
 *   macOS: ~/Library/Application Support/PortGamePC/Brigandine/settings.cfg
 *   Windows: %APPDATA%\PortGamePC\Brigandine\settings.cfg
 * Priority: environment variable (BRIG_BGM_VOLUME / BRIG_SFX_VOLUME / BRIG_MOVIE_VOLUME) > file > default. */
#include "settings.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings = { 30, 100, 50, 1, 0, 0, 0, 1, 1, 1, 50, 1, 1 };   /* BGM 30% matches the emulator balance (user-tested); movies 50% (user choice) */
extern float g_bgm_gain, g_sfx_gain, g_movie_gain;   /* audio.c */

static char g_path[1024];

static int clamp_pct(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
static int clamp_range(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void settings_apply(void) {
    g_bgm_gain = g_settings.bgm / 100.0f;
    g_sfx_gain = g_settings.sfx / 100.0f;
    g_movie_gain = g_settings.movie / 100.0f;
}

void settings_load(void) {
    char *dir = SDL_GetPrefPath("PortGamePC", "Brigandine");
    if (dir) { snprintf(g_path, sizeof g_path, "%ssettings.cfg", dir); SDL_free(dir); }
    FILE *f = g_path[0] ? fopen(g_path, "r") : NULL;
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            int v;
            if (sscanf(line, "bgm=%d", &v) == 1) g_settings.bgm = clamp_pct(v);
            else if (sscanf(line, "sfx=%d", &v) == 1) g_settings.sfx = clamp_pct(v);
            else if (sscanf(line, "movie=%d", &v) == 1) g_settings.movie = clamp_pct(v);
            else if (sscanf(line, "vsync=%d", &v) == 1) g_settings.vsync = v != 0;
            else if (sscanf(line, "frameskip=%d", &v) == 1) g_settings.frameskip = clamp_range(v, 0, 4);
            else if (sscanf(line, "show_perf=%d", &v) == 1) g_settings.show_perf = v != 0;
            else if (sscanf(line, "fast_load=%d", &v) == 1) g_settings.fast_load = v != 0;
            else if (sscanf(line, "autosave=%d", &v) == 1) g_settings.autosave = v != 0;
            else if (sscanf(line, "slot=%d", &v) == 1) g_settings.slot = clamp_range(v, 1, 10);
            else if (sscanf(line, "mouse=%d", &v) == 1) g_settings.mouse = v != 0;
            else if (sscanf(line, "mouse_speed=%d", &v) == 1) g_settings.mouse_speed = clamp_range(v, 10, 200);
            else if (sscanf(line, "mouse_capture=%d", &v) == 1) g_settings.mouse_capture = v != 0;
            else if (sscanf(line, "render_scale=%d", &v) == 1) g_settings.render_scale = clamp_range(v, 1, 8);
        }
        fclose(f);
    }
    const char *e;
    if ((e = getenv("BRIG_BGM_VOLUME"))) g_settings.bgm = atoi(e);
    if ((e = getenv("BRIG_SFX_VOLUME"))) g_settings.sfx = atoi(e);
    if ((e = getenv("BRIG_MOVIE_VOLUME"))) g_settings.movie = atoi(e);
    if ((e = getenv("BRIG_VSYNC"))) g_settings.vsync = atoi(e) != 0;
    if ((e = getenv("BRIG_FRAMESKIP"))) g_settings.frameskip = clamp_range(atoi(e), 0, 4);
    if ((e = getenv("BRIG_FAST_LOAD"))) g_settings.fast_load = atoi(e) != 0;
    if ((e = getenv("BRIG_SCALE"))) g_settings.render_scale = clamp_range(atoi(e), 1, 8);
    settings_apply();
}

void settings_save(void) {
    if (!g_path[0]) return;
    FILE *f = fopen(g_path, "w");
    if (!f) return;
    fprintf(f, "# Brigandine GE native port settings\n# volumes 0-100\nbgm=%d\nsfx=%d\nmovie=%d\n"
               "# video: vsync 0/1, frameskip 0 off / 1 auto / 2-4 draw 1 of N, show_perf 0/1,\n"
               "# render_scale 1 = native (software) / 2-8 = hardware renderer at N x\n"
               "vsync=%d\nframeskip=%d\nshow_perf=%d\nrender_scale=%d\n"
               "# system: fast_load 0 = original CD speed / 1 = instant, autosave 0/1, slot = save slot for F5/F9 (1-10), mouse 0/1, mouse_speed 10-200 (%%), mouse_capture 0/1\nfast_load=%d\nautosave=%d\nslot=%d\nmouse=%d\nmouse_speed=%d\nmouse_capture=%d\n",
            g_settings.bgm, g_settings.sfx, g_settings.movie, g_settings.vsync, g_settings.frameskip, g_settings.show_perf, g_settings.render_scale,
            g_settings.fast_load, g_settings.autosave, g_settings.slot, g_settings.mouse, g_settings.mouse_speed, g_settings.mouse_capture);
    fclose(f);
}

const char *settings_path(void) { return g_path; }
