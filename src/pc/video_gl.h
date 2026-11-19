/* OpenGL presentation + shaders (video_gl.c). C interface for platform.c and ui.cpp. */
#pragma once
#include <SDL.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

int  vgl_init(SDL_Window *win);                  /* create the GL context; 0 on failure */
void *vgl_context(void);
void vgl_set_vsync(int on);
void vgl_drawable_size(int *w, int *h);
void vgl_upload(const uint32_t *argb, int w, int h);   /* the PS1 display image (ARGB8888) */
void vgl_draw(int dx, int dy, int dw, int dh);   /* clear the window, draw the image into this rect (top-left origin) */
int  vgl_read_pixels(uint32_t *argb, int w, int h);    /* window contents, top row first (screenshots) */
void vgl_swap(void);

/* shaders: built-ins first, then *.glsl files from the shader folders */
typedef struct { char name[48], desc[96]; float def, min, max, step, value; int loc; } VglParam;
int  vgl_shader_count(void);
const char *vgl_shader_name(int i);
int  vgl_shader_is_builtin(int i);
int  vgl_shader_current(void);
int  vgl_select_shader(int i);                   /* 1 ok, 0 failed to compile (falls back to the default) */
void vgl_rescan_shaders(void);                   /* re-read the folders and recompile the current shader */
const char *vgl_shader_dir(void);                /* user shader folder (created on demand) */
int  vgl_param_count(void);
VglParam *vgl_param(int i);
void vgl_param_changed(int i);
void vgl_reset_params(void);
int  vgl_filter_linear(void);
void vgl_set_filter_linear(int on);
void vgl_save_config(void);

/* small RGB textures for the UI (save thumbnails) */
unsigned vgl_create_rgb_texture(int w, int h, const uint8_t *rgb);
void vgl_update_rgb_texture(unsigned tex, int w, int h, const uint8_t *rgb);

#ifdef __cplusplus
}
#endif
