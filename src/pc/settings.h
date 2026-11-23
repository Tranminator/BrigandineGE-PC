#pragma once

typedef struct {
    int bgm, sfx, movie;          /* percent, 0..100 */
    int vsync;                    /* 1 = present on vsync */
    int frameskip;                /* 0 off, 1 auto, 2..4 = draw 1 of N frames */
    int show_perf;                /* performance overlay */
    int fast_load;                /* 0 = original CD timing, 1 = instant loading */
    int autosave;                 /* save states at each phase change and battle start */
    int slot;                     /* save slot used by F5 / F9 (1..10) */
    int mouse;                    /* mouse control (1 = on) */
    int mouse_speed;              /* game pointer speed, percent of the OS pointer */
    int mouse_capture;            /* 1 = game pointer (mouse captured), 0 = OS pointer */
    int render_scale;             /* 1 = native software renderer, 2..8 = hardware renderer at N x */
    int disc;                     /* disc in the drive at the last session (1 / 2): the next start boots it */
    int screen_fit;               /* 0 = original 4:3 (fit), 1 = integer scale, 2 = stretch, 3 = native 1x (320x240) */
    int fullscreen;               /* 0 = window, 1 = fullscreen (desktop resolution) */
    int window_scale;             /* window size: 1..4 x 320x240 */
} Settings;
extern Settings g_settings;

void settings_load(void);    /* file + env overrides, then apply */
void settings_apply(void);   /* push g_settings into the audio mixer */
void settings_save(void);
const char *settings_path(void);
