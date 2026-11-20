/* Settings UI (Dear ImGui), toggled with F2. C interface for the platform layer. */
#pragma once
#include <SDL.h>
#ifdef __cplusplus
extern "C" {
#endif

void ui_init(SDL_Window *win, void *gl_context);
int  ui_event(const SDL_Event *e);   /* returns 1 when the UI consumed the event */
int  ui_is_open(void);
void ui_toast(const char *msg);      /* short message, bottom right */
void ui_frame(void);                 /* draw the UI (if visible) into the current frame */

typedef struct { double fps, busy_avg_ms, busy_max_ms; int skipped_per_s; } PlatformStats;
extern PlatformStats g_stats;

#ifdef __cplusplus
}
#endif
