#pragma once

typedef struct {
    int bgm, sfx, movie;          /* percent, 0..100 */
    int vsync;                    /* 1 = present on vsync */
    int frameskip;                /* 0 off, 1 auto, 2..4 = draw 1 of N frames */
    int show_perf;                /* performance overlay */
    int fast_load;                /* 0 = original CD timing, 1 = instant loading */
    int autosave;                 /* save states at each phase change and battle start */
    int slot;                     /* save slot used by F5 / F9 (1..10) */
} Settings;
extern Settings g_settings;

void settings_load(void);    /* file + env overrides, then apply */
void settings_apply(void);   /* push g_settings into the audio mixer */
void settings_save(void);
const char *settings_path(void);
