#pragma once

typedef struct { int bgm, sfx, movie; } Settings;   /* percent, 0..100 */
extern Settings g_settings;

void settings_load(void);    /* file + env overrides, then apply */
void settings_apply(void);   /* push g_settings into the audio mixer */
void settings_save(void);
const char *settings_path(void);
