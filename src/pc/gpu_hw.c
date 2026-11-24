/* Hardware (OpenGL) PS1 renderer with internal upscaling. See gpu_hw.h.
 *
 * GPU-side resources:
 *   native  - R16UI 1024x512 mirror of g_vram (texture pages and CLUTs are read from it,
 *             so 4/8-bit palettised textures decode exactly like on the PS1)
 *   scaled  - RGBA8 (1024*N)x(512*N) render target; RGB = colour, A = mask bit (bit 15)
 *   display - the display area cut out of `scaled`, handed to the presentation shader
 *
 * Primitives are batched. Ordering against the software VRAM is kept with two rect lists:
 *   dirty - areas of g_vram changed since the native texture was last updated
 *   reads - texture/CLUT areas the queued primitives sample
 * Any VRAM write that overlaps `reads` flushes the batch first, so queued primitives always
 * see VRAM as it was when they were submitted (this keeps render-to-texture effects right).
 *
 * Semi-transparency: modes 0 (B/2+F/2), 1 (B+F) and 3 (B+F/4) use dual-source blending,
 * so opaque and semi-transparent texels mix in one draw; mode 2 (B-F) draws its opaque
 * texels first, then subtracts the semi-transparent ones. Colours are kept at 8 bits per
 * channel (no 15-bit truncation or dithering), so gradients are smooth at any scale. */
#include "gpu_hw.h"
#include "gpu.h"
#include "gl_funcs.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void rt_log(const char *fmt, ...);

int g_hw_on;
static int s_scale = 1, s_max_scale = 8, s_ready, s_need_resync = 1;
static GLuint s_native, s_scaled, s_fbo, s_tmp, s_tmp_fbo, s_disp, s_disp_fbo;
static int s_disp_w, s_disp_h, s_tmp_w, s_tmp_h;
static GLuint s_prog, s_copy_prog, s_vao, s_vbo;
static GLint u_vram, u_mode, u_pass, u_hscale, uc_vram, uc_scale, u_xf, uc_xf, u_wide, uc_wide;

/* Widescreen ("wide canvas", like bsnes-hd): the two 320x240 display buffers (VRAM 0,0 and 0,240)
 * are mirrored into a separate, wider target (320 + 2E) x 480, everything drawn into them shifted E
 * pixels right and not clipped at x 0 / 319. The game's picture stays the centre; on screens where
 * the game draws past its 320 pixels (see widescreen.c) the sides show more. VRAM itself is
 * unchanged (it is full of textures right of the buffers), so readbacks and effects keep working. */
int g_ws_extra;                     /* E: extra native pixels on each side, 0 = off */
static GLuint s_wide, s_wide_fbo;
static int s_wide_e, s_wide_s;      /* E and scale the wide target was made for */
static int s_wide_stale;            /* 4:3 screens draw to the scaled VRAM only: the wide canvas's centre is
                                     * old until a wide screen starts (then it is refreshed from VRAM once) */
static int s_last_wide;             /* the last hw_display_texture returned the wide picture */
static int s_vram_stale;            /* buffer batches went to the wide canvas only: the scaled VRAM's
                                       display buffers are out of date (refreshed before they are read) */
#define WS_BUF_W 320
#define WS_BUF_H 480                /* both buffers, stacked as in VRAM */

/* ------------------------------------------------------------------ shaders */
static const char VS[] =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec4 a_col;\n"
    "layout(location = 2) in vec2 a_uv;\n"
    "layout(location = 3) in uvec4 a_info;\n"
    "flat out uvec4 v_info; noperspective out vec4 v_col; noperspective out vec2 v_uv;\n"
    "uniform vec4 u_xf;\n"   /* target: (x offset, y offset, width, height) in native pixels */
    "uniform float u_wide;\n" /* wide canvas pass: E, else 0 */
    "void main() {\n"
    "  vec2 q = a_pos;\n"
    /* screen-wide untextured primitives (gradients, fades): their x 0 / 320 edges reach the canvas edges */
    "  if (u_wide > 0.0 && ((a_info.x >> 16) & 16u) != 0u) { if (q.x <= 0.0) q.x -= u_wide; else if (q.x >= 320.0) q.x += u_wide; }\n"
    "  vec2 p = (q + u_xf.xy) / u_xf.zw * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_col = a_col; v_uv = a_uv; v_info = a_info;\n"
    "}\n";

/* info.x: tpage (9 bits) | flags << 16 (1 textured, 2 raw, 4 semi, 8 set mask)
 * info.y: clut   info.z: texture window (mw | mh<<5 | ox<<10 | oy<<15)
 * info.w: UV limits u0 | v0<<8 | u1<<16 | v1<<24 */
static const char FS[] =
    "#version 330 core\n"
    "flat in uvec4 v_info; noperspective in vec4 v_col; noperspective in vec2 v_uv;\n"
    "uniform usampler2D u_vram; uniform int u_mode; uniform int u_pass; uniform float u_scale;\n"
    "layout(location = 0, index = 0) out vec4 o_col;\n"
    "layout(location = 0, index = 1) out vec4 o_fac;\n"
    "uint vram(int x, int y) { return texelFetch(u_vram, ivec2(x & 1023, y & 511), 0).r; }\n"
    /* one PS1 texel: UV limits, texture window, 4/8/15-bit decode through the CLUT */
    "uint texel(int u, int v) {\n"
    "  uint tp = v_info.x & 0x1FFu, clut = v_info.y, tw = v_info.z, lim = v_info.w;\n"
    "  u = clamp(u, int(lim & 255u), int((lim >> 16) & 255u)) & 255;\n"
    "  v = clamp(v, int((lim >> 8) & 255u), int(lim >> 24)) & 255;\n"
    "  int mw = int(tw & 31u), mh = int((tw >> 5) & 31u), ox = int((tw >> 10) & 31u), oy = int((tw >> 15) & 31u);\n"
    "  u = (u & ~(mw * 8)) | ((ox & mw) * 8);\n"
    "  v = (v & ~(mh * 8)) | ((oy & mh) * 8);\n"
    "  int bx = int(tp & 15u) * 64, by = int((tp >> 4) & 1u) * 256, depth = int((tp >> 7) & 3u);\n"
    "  int cx = int(clut & 63u) * 16, cy = int((clut >> 6) & 511u);\n"
    "  if (depth == 0) { uint w = vram(bx + u / 4, by + v); return vram(cx + int((w >> uint((u & 3) * 4)) & 15u), cy); }\n"
    "  if (depth == 1) { uint w = vram(bx + u / 2, by + v); return vram(cx + int((w >> uint((u & 1) * 8)) & 255u), cy); }\n"
    "  return vram(bx + u, by + v);\n"
    "}\n"
    "vec3 rgb8(uint t) { return vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u)) * 8.0; }\n"
    "void main() {\n"
    /* geometry is not shifted (edges line up with sprites), but the PS1 evaluates UV and colour
     * at the pixel's top-left corner: take the value half a native pixel up-left */
    "  vec2 gx = dFdx(v_uv) * u_scale, gy = dFdy(v_uv) * u_scale;\n"   /* UV change per native pixel */
    "  vec2 uv = v_uv - 0.5 * (gx + gy);\n"
    /* axis-aligned one-texel-per-pixel mappings (UI, text, sprites drawn as polygons): round, so
     * texel edges stay on native pixel edges; everything else (skewed terrain, 3D): floor */
    "  bvec2 unit = bvec2(abs(abs(gx.x) + abs(gy.x) - 1.0) < 0.002 && min(abs(gx.x), abs(gy.x)) < 0.002,\n"
    "                     abs(abs(gx.y) + abs(gy.y) - 1.0) < 0.002 && min(abs(gx.y), abs(gy.y)) < 0.002);\n"
    "  uv = floor(uv + mix(vec2(0.0), vec2(0.5), unit));\n"
    "  vec3 vc = v_col.rgb - (dFdx(v_col.rgb) + dFdy(v_col.rgb)) * (0.5 * u_scale);\n"
    "  uint flags = v_info.x >> 16;\n"
    "  bool semi = (flags & 4u) != 0u, mbit = (flags & 8u) != 0u, st;\n"
    "  vec3 c;\n"
    "  if ((flags & 1u) != 0u) {\n"
    "    uint t = texel(int(uv.x), int(uv.y));\n"
    "    if (t == 0u) discard;\n"
    "    vec3 tc = rgb8(t);\n"
    "    c = (flags & 2u) != 0u ? tc : min(tc * floor(clamp(vc, 0.0, 1.0) * 255.0 + 0.5) / 128.0, vec3(255.0));\n"
    "    st = semi && (t & 0x8000u) != 0u;\n"
    "    mbit = mbit || (t & 0x8000u) != 0u;\n"
    "  } else {\n"
    "    c = floor(clamp(vc, 0.0, 1.0) * 255.0 + 0.5);\n"
    "    st = semi;\n"
    "  }\n"
    /* the PS1 truncates computed (non-raw) colours to 5 bits per channel, 3.5/255 lost on
     * average: keep 8-bit smoothness but match its average brightness */
    "  if ((flags & 3u) != 3u) c = max(c - BIAS, vec3(0.0));\n"
    "  if (u_pass == 1 && st) discard;\n"
    "  if (u_pass == 2 && !st) discard;\n"
    "  c /= 255.0;\n"
    "  float a = mbit ? 1.0 : 0.0;\n"
    "  o_col = vec4(c, a); o_fac = vec4(0.0);\n"
    "  if (st && u_pass == 0) {\n"
    "    if (u_mode == 0) { o_col.rgb = c * 0.5; o_fac = vec4(0.5); }\n"
    "    else if (u_mode == 1) { o_fac = vec4(1.0); }\n"
    "    else { o_col.rgb = c * 0.25; o_fac = vec4(1.0); }\n"
    "  }\n"
    "}\n";

/* native VRAM -> scaled target (uploads, resync): nearest upscale */
static const char FS_COPY[] =
    "#version 330 core\n"
    "uniform usampler2D u_vram; uniform float u_scale;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(floor(gl_FragCoord.xy / u_scale));\n"
    "  uint t = texelFetch(u_vram, ivec2(p.x & 1023, p.y & 511), 0).r;\n"
    "  o = vec4(vec3(float(t & 31u), float((t >> 5) & 31u), float((t >> 10) & 31u)) * 8.0 / 255.0,\n"
    "           (t & 0x8000u) != 0u ? 1.0 : 0.0);\n"
    "}\n";

static GLuint build(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(), sh[2];
    /* fs starts with its #version line: insert the defines after it */
    static char fsbuf[16384];
    const char *nl = strchr(fs, '\n');
    const char *bias = getenv("BRIG_HW_BIAS");
    snprintf(fsbuf, sizeof fsbuf, "%.*s#define BIAS %s\n%s", (int)(nl - fs + 1), fs, bias ? bias : "3.5", nl + 1);
    const char *src[2] = { vs, fsbuf };
    GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    for (int i = 0; i < 2; i++) {
        sh[i] = glCreateShader(type[i]);
        glShaderSource(sh[i], 1, &src[i], NULL);
        glCompileShader(sh[i]);
        GLint ok = 0; glGetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(sh[i], sizeof log, NULL, log); rt_log("[hw] shader: %s", log); return 0; }
        glAttachShader(p, sh[i]);
    }
    glLinkProgram(p);
    glDeleteShader(sh[0]); glDeleteShader(sh[1]);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); rt_log("[hw] link: %s", log); return 0; }
    return p;
}

/* ------------------------------------------------------------------ rect lists */
typedef struct { int x0, y0, x1, y1; } Rc;          /* [x0,x1) x [y0,y1) */
#define MAX_RC 16
typedef struct { Rc r[MAX_RC]; int n; } RcList;
static RcList s_dirty, s_reads;

static Rc rc_make(int x, int y, int w, int h) {
    Rc r = { x, y, x + w, y + h };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > VRAM_W) r.x1 = VRAM_W;
    if (r.y1 > VRAM_H) r.y1 = VRAM_H;
    return r;
}
static int rc_empty(Rc a) { return a.x0 >= a.x1 || a.y0 >= a.y1; }
static int rc_hit(Rc a, Rc b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
static Rc rc_union(Rc a, Rc b) {
    Rc r = { a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1 };
    return r;
}
static void list_add(RcList *l, Rc r) {
    if (rc_empty(r)) return;
    for (int i = 0; i < l->n; i++)
        if (r.x0 >= l->r[i].x0 && r.y0 >= l->r[i].y0 && r.x1 <= l->r[i].x1 && r.y1 <= l->r[i].y1) return;
    if (l->n == MAX_RC) {                             /* full: collapse into one box */
        Rc u = l->r[0];
        for (int i = 1; i < l->n; i++) u = rc_union(u, l->r[i]);
        l->r[0] = u; l->n = 1;
    }
    l->r[l->n++] = r;
}
static int list_hit(const RcList *l, Rc r) {
    for (int i = 0; i < l->n; i++) if (rc_hit(l->r[i], r)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ batch */
typedef struct { float x, y; u8 r, g, b, a; float u, v; u32 info[4]; } Vert;
static Vert *s_verts;
static int s_nverts, s_cap;
static int s_batch_mode = -1;                       /* -1 opaque only, else the semi mode in use */
static int s_ax0, s_ay0, s_ax1, s_ay1;              /* draw area of the batch */

static void sync_native(void) {
    if (!s_dirty.n) return;
    glBindTexture(GL_TEXTURE_2D, s_native);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, VRAM_W);
    for (int i = 0; i < s_dirty.n; i++) {
        Rc r = s_dirty.r[i];
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                        &g_vram[r.y0 * VRAM_W + r.x0]);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    s_dirty.n = 0;
}

static void bind_target(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, VRAM_W * s_scale, VRAM_H * s_scale);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_native);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
}

static void draw_verts(const Vert *v, int n) {
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * sizeof(Vert)), v, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, n);
}

static GLuint make_rgba_target(int w, int h, GLuint *fbo, int linear);
/* ---- wide canvas */
static int wide_on(void) { return g_ws_extra > 0 && s_wide && s_wide_e == g_ws_extra && s_wide_s == s_scale; }
static int wide_ensure(void) {
    if (g_ws_extra <= 0) return 0;
    if (s_wide && s_wide_e == g_ws_extra && s_wide_s == s_scale) return 1;
    if (s_wide_fbo) glDeleteFramebuffers(1, &s_wide_fbo);
    if (s_wide) glDeleteTextures(1, &s_wide);
    s_wide = make_rgba_target((WS_BUF_W + 2 * g_ws_extra) * s_scale, WS_BUF_H * s_scale, &s_wide_fbo, 0);
    s_wide_e = g_ws_extra; s_wide_s = s_scale;
    if (!s_wide) return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, s_wide_fbo);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_wide_stale = 1;
    return 1;
}
/* a draw area that is one display buffer */
static int is_buffer_area(int x0, int y0, int x1, int y1) {
    return x0 == 0 && x1 == WS_BUF_W - 1 && (y0 == 0 || y0 == 240) && y1 == y0 + 239;
}
/* copy a scaled-VRAM rect inside the buffers into the wide target's centre */
static void wide_from_vram(Rc r) {
    if (!wide_on() || s_wide_stale) return;
    if (r.x1 > WS_BUF_W) r.x1 = WS_BUF_W;
    if (r.y1 > WS_BUF_H) r.y1 = WS_BUF_H;
    if (rc_empty(r)) return;
    int S = s_scale, E = s_wide_e;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_wide_fbo);
    glBlitFramebuffer(r.x0 * S, r.y0 * S, r.x1 * S, r.y1 * S, (r.x0 + E) * S, r.y0 * S, (r.x1 + E) * S, r.y1 * S,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* a wide screen starts: bring the canvas's centre up to date with the scaled VRAM */
static void wide_refresh(void) {
    if (!s_wide_stale || !wide_on()) return;
    s_wide_stale = 0;
    wide_from_vram(rc_make(0, 0, WS_BUF_W, WS_BUF_H));
}

/* the reverse, for the rect of the buffers about to be read from the scaled VRAM */
static void vram_from_wide(Rc r) {
    if (!s_vram_stale || !s_wide) return;
    if (r.x1 > WS_BUF_W) r.x1 = WS_BUF_W;
    if (r.y1 > WS_BUF_H) r.y1 = WS_BUF_H;
    if (rc_empty(r)) return;
    int S = s_scale, E = s_wide_e;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_wide_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    glBlitFramebuffer((r.x0 + E) * S, r.y0 * S, (r.x1 + E) * S, r.y1 * S, r.x0 * S, r.y0 * S, r.x1 * S, r.y1 * S,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void flush(void) {
    if (!s_nverts) { s_reads.n = 0; s_batch_mode = -1; return; }
    int to_wide = wide_ensure() && is_buffer_area(s_ax0, s_ay0, s_ax1, s_ay1);
    /* each batch is drawn once: a screen shown wide into the wide canvas only, a 4:3 screen into the
     * scaled VRAM only (drawing both halved the frame rate at high scales) */
    extern int g_ws_screen;
    if (to_wide && !g_ws_screen) { to_wide = 0; s_wide_stale = 1; }
    if (to_wide) wide_refresh();
    sync_native();
    bind_target();
    glEnable(GL_SCISSOR_TEST);
    glScissor(s_ax0 * s_scale, s_ay0 * s_scale, (s_ax1 - s_ax0 + 1) * s_scale, (s_ay1 - s_ay0 + 1) * s_scale);
    glUseProgram(s_prog);
    glUniform1i(u_vram, 0);
    glUniform1i(u_mode, s_batch_mode < 0 ? 0 : s_batch_mode);
    glUniform1f(u_hscale, (float)s_scale);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_nverts * sizeof(Vert)), s_verts, GL_STREAM_DRAW);
    int first = to_wide ? 1 : 0;
    if (first) s_vram_stale = 1;
    for (int pass = first; pass < 1 + to_wide; pass++) {
    if (pass == 0) { glUniform4f(u_xf, 0, 0, VRAM_W, VRAM_H); glUniform1f(u_wide, 0); }
    else {                                         /* the same batch into the wide canvas, sides unclipped */
        int E = s_wide_e, S = s_scale;
        glBindFramebuffer(GL_FRAMEBUFFER, s_wide_fbo);
        glViewport(0, 0, (WS_BUF_W + 2 * E) * S, WS_BUF_H * S);
        glScissor(0, s_ay0 * S, (WS_BUF_W + 2 * E) * S, (s_ay1 - s_ay0 + 1) * S);
        glUniform4f(u_xf, (float)E, 0, (float)(WS_BUF_W + 2 * E), (float)WS_BUF_H);
        glUniform1f(u_wide, (float)E);
    }
    if (s_batch_mode == 2) {
        glDisable(GL_BLEND);
        glUniform1i(u_pass, 1);
        glDrawArrays(GL_TRIANGLES, 0, s_nverts);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        glUniform1i(u_pass, 2);
        glDrawArrays(GL_TRIANGLES, 0, s_nverts);
    } else if (s_batch_mode >= 0) {
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_SRC1_ALPHA, GL_ONE, GL_ZERO);
        glUniform1i(u_pass, 0);
        glDrawArrays(GL_TRIANGLES, 0, s_nverts);
    } else {
        glDisable(GL_BLEND);
        glUniform1i(u_pass, 0);
        glDrawArrays(GL_TRIANGLES, 0, s_nverts);
    }
    glDisable(GL_BLEND);
    }
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_nverts = 0;
    s_reads.n = 0;
    s_batch_mode = -1;
}

/* copy native VRAM rect into the scaled target (after a CPU upload, or a full resync) */
static void copy_native_to_scaled(Rc r) {
    sync_native();
    bind_target();
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
    glUseProgram(s_copy_prog);
    glUniform1i(uc_vram, 0);
    glUniform1f(uc_scale, (float)s_scale);
    glUniform4f(uc_xf, 0, 0, VRAM_W, VRAM_H);
    glUniform1f(uc_wide, 0);
    Vert q[6]; memset(q, 0, sizeof q);
    float x0 = (float)r.x0, y0 = (float)r.y0, x1 = (float)r.x1, y1 = (float)r.y1;
    float xy[6][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    for (int i = 0; i < 6; i++) { q[i].x = xy[i][0]; q[i].y = xy[i][1]; }
    draw_verts(q, 6);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    wide_from_vram(r);
}

static void resync(void) {
    s_need_resync = 0;
    s_nverts = 0; s_reads.n = 0; s_batch_mode = -1;
    s_dirty.n = 0;
    list_add(&s_dirty, rc_make(0, 0, VRAM_W, VRAM_H));
    copy_native_to_scaled(rc_make(0, 0, VRAM_W, VRAM_H));
}

static inline int ready(void) {
    if (!g_hw_on) return 0;
    if (s_need_resync) resync();
    return 1;
}

/* ------------------------------------------------------------------ setup */
static void free_targets(void) {
    if (s_fbo) glDeleteFramebuffers(1, &s_fbo);
    if (s_scaled) glDeleteTextures(1, &s_scaled);
    if (s_disp_fbo) glDeleteFramebuffers(1, &s_disp_fbo);
    if (s_disp) glDeleteTextures(1, &s_disp);
    if (s_tmp_fbo) glDeleteFramebuffers(1, &s_tmp_fbo);
    if (s_tmp) glDeleteTextures(1, &s_tmp);
    if (s_wide_fbo) glDeleteFramebuffers(1, &s_wide_fbo);
    if (s_wide) glDeleteTextures(1, &s_wide);
    s_wide = s_wide_fbo = 0; s_wide_e = s_wide_s = 0; s_vram_stale = 0; s_wide_stale = 0;
    s_fbo = s_scaled = s_disp_fbo = s_disp = s_tmp_fbo = s_tmp = 0;
    s_disp_w = s_disp_h = s_tmp_w = s_tmp_h = 0;
}

static GLuint make_rgba_target(int w, int h, GLuint *fbo, int linear) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) { rt_log("[hw] framebuffer %dx%d incomplete (%x)", w, h, st); return 0; }
    return t;
}

static int init_once(void) {
    if (s_ready) return s_ready > 0;
    s_ready = -1;
    GLint maxtex = 0;
    glGetIntegerv(0x0D33 /* GL_MAX_TEXTURE_SIZE */, &maxtex);
    s_max_scale = maxtex >= 8192 ? 8 : maxtex / VRAM_W;
    if (s_max_scale < 2) { rt_log("[hw] max texture size %d too small", maxtex); return 0; }
    s_prog = build(VS, FS);
    s_copy_prog = build(VS, FS_COPY);
    if (!s_prog || !s_copy_prog) return 0;
    u_vram = glGetUniformLocation(s_prog, "u_vram");
    u_mode = glGetUniformLocation(s_prog, "u_mode");
    u_pass = glGetUniformLocation(s_prog, "u_pass");
    u_hscale = glGetUniformLocation(s_prog, "u_scale");
    uc_vram = glGetUniformLocation(s_copy_prog, "u_vram");
    uc_scale = glGetUniformLocation(s_copy_prog, "u_scale");
    u_xf = glGetUniformLocation(s_prog, "u_xf");
    uc_xf = glGetUniformLocation(s_copy_prog, "u_xf");
    u_wide = glGetUniformLocation(s_prog, "u_wide");
    uc_wide = glGetUniformLocation(s_copy_prog, "u_wide");

    glGenTextures(1, &s_native);
    glBindTexture(GL_TEXTURE_2D, s_native);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, VRAM_W, VRAM_H, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, 0, sizeof(Vert), (const void *)offsetof(Vert, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vert), (const void *)offsetof(Vert, r));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, 0, sizeof(Vert), (const void *)offsetof(Vert, u));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(3, 4, GL_UNSIGNED_INT, sizeof(Vert), (const void *)offsetof(Vert, info));
    glBindVertexArray(0);
    s_ready = 1;
    return 1;
}

int hw_set_scale(int scale) {
    if (scale < 2) {
        if (g_hw_on) { free_targets(); g_hw_on = 0; }
        s_scale = 1;
        return 1;
    }
    if (!init_once()) { g_hw_on = 0; s_scale = 1; return 1; }
    if (scale > s_max_scale) scale = s_max_scale;
    if (g_hw_on && scale == s_scale) return s_scale;
    free_targets();
    s_scale = scale;
    s_scaled = make_rgba_target(VRAM_W * scale, VRAM_H * scale, &s_fbo, 0);
    if (!s_scaled) { free_targets(); g_hw_on = 0; s_scale = 1; return 1; }
    g_hw_on = 1;
    s_need_resync = 1;
    rt_log("[hw] hardware renderer on, %dx (%dx%d)", scale, VRAM_W * scale, VRAM_H * scale);
    return s_scale;
}

int hw_scale(void) { return g_hw_on ? s_scale : 1; }
int hw_max_scale(void) { init_once(); return s_max_scale; }
void hw_invalidate(void) { s_need_resync = 1; s_nverts = 0; s_reads.n = 0; s_batch_mode = -1; }

/* ------------------------------------------------------------------ primitive intake */
static void begin_prim(int semi_mode_needed) {
    /* draw area change ends the batch */
    if (s_nverts && (s_ax0 != g_gpu.area_x0 || s_ay0 != g_gpu.area_y0 || s_ax1 != g_gpu.area_x1 || s_ay1 != g_gpu.area_y1))
        flush();
    /* blend mode compatibility (see file comment) */
    if (s_nverts) {
        if (s_batch_mode == 2 && semi_mode_needed != 2) flush();
        else if (semi_mode_needed >= 0 && s_batch_mode >= 0 && semi_mode_needed != s_batch_mode) flush();
    }
    if (!s_nverts) { s_ax0 = g_gpu.area_x0; s_ay0 = g_gpu.area_y0; s_ax1 = g_gpu.area_x1; s_ay1 = g_gpu.area_y1; }
    if (semi_mode_needed >= 0) s_batch_mode = semi_mode_needed;
}

static Vert *alloc_verts(int n) {
    if (s_nverts + n > s_cap) {
        s_cap = s_cap ? s_cap * 2 : 8192;
        while (s_cap < s_nverts + n) s_cap *= 2;
        s_verts = (Vert *)realloc(s_verts, (size_t)s_cap * sizeof(Vert));
    }
    Vert *v = s_verts + s_nverts;
    s_nverts += n;
    return v;
}

static u32 tw_bits(void) {
    return (u32)g_gpu.tw_mw | ((u32)g_gpu.tw_mh << 5) | ((u32)g_gpu.tw_ox << 10) | ((u32)g_gpu.tw_oy << 15);
}

/* texture page + CLUT area a primitive samples, narrowed to its UV box */
static void add_reads(u32 tp, u32 clut, int umin, int vmin, int umax, int vmax) {
    int bx = (tp & 15) * 64, by = ((tp >> 4) & 1) * 256, depth = (tp >> 7) & 3;
    if (g_gpu.tw_mw || g_gpu.tw_mh) { umin = vmin = 0; umax = vmax = 255; }
    if (depth == 0) list_add(&s_reads, rc_make(bx + umin / 4, by + vmin, umax / 4 - umin / 4 + 1, vmax - vmin + 1));
    else if (depth == 1) list_add(&s_reads, rc_make(bx + umin / 2, by + vmin, umax / 2 - umin / 2 + 1, vmax - vmin + 1));
    else list_add(&s_reads, rc_make(bx + umin, by + vmin, umax - umin + 1, vmax - vmin + 1));
    if (depth < 2) {
        int cx = (clut & 63) * 16, cy = (clut >> 6) & 511;
        list_add(&s_reads, rc_make(cx, cy, depth == 0 ? 16 : 256, 1));
    }
}

/* the software rasteriser is about to write `r`: flush if queued primitives read it */
static void before_sw_write(Rc r) {
    if (list_hit(&s_reads, r)) flush();
    list_add(&s_dirty, r);
}

static Rc prim_bounds(const HwV *v, int n) {
    int x0 = v[0].x, y0 = v[0].y, x1 = v[0].x, y1 = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
    }
    if (x0 < g_gpu.area_x0) x0 = g_gpu.area_x0;
    if (y0 < g_gpu.area_y0) y0 = g_gpu.area_y0;
    if (x1 > g_gpu.area_x1) x1 = g_gpu.area_x1;
    if (y1 > g_gpu.area_y1) y1 = g_gpu.area_y1;
    return rc_make(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

static void put_vert(Vert *o, float x, float y, int r, int g, int b, float u, float v, const u32 info[4]) {
    o->x = x; o->y = y; o->r = (u8)r; o->g = (u8)g; o->b = (u8)b; o->a = 255;
    o->u = u; o->v = v;
    memcpy(o->info, info, sizeof o->info);
}

void hw_poly(const HwV *v, int nv, int gouraud, int textured, int raw, int semi, u32 tpage, u32 clut) {
    if (!ready()) return;
    begin_prim(semi ? g_gpu.semi_mode : -1);
    int umin = 255, vmin = 255, umax = 0, vmax = 0;
    for (int i = 0; i < nv; i++) {
        if (v[i].u < umin) umin = v[i].u;
        if (v[i].u > umax) umax = v[i].u;
        if (v[i].v < vmin) vmin = v[i].v;
        if (v[i].v > vmax) vmax = v[i].v;
    }
    u32 flags = (textured ? 1u : 0) | (raw ? 2u : 0) | (semi ? 4u : 0) | (g_gpu.mask_set ? 8u : 0);
    if (!textured) {                               /* spans the whole 320 width: widened on the wide canvas */
        int mnx = v[0].x, mxx = v[0].x;
        for (int i = 1; i < nv; i++) { if (v[i].x < mnx) mnx = v[i].x; if (v[i].x > mxx) mxx = v[i].x; }
        if (mnx <= 0 && mxx >= 320) flags |= 16u;
    }
    u32 info[4] = { (tpage & 0x1FF) | (flags << 16), clut & 0xFFFF, tw_bits(),
                    (u32)umin | ((u32)vmin << 8) | ((u32)umax << 16) | ((u32)vmax << 24) };
    static const int tris[2][3] = { { 0, 1, 2 }, { 1, 2, 3 } };
    for (int t = 0; t < (nv == 4 ? 2 : 1); t++) {
        const HwV *a = &v[tris[t][0]], *b = &v[tris[t][1]], *c = &v[tris[t][2]];
        int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
        const HwV *p3[3] = { a, b, c };
        for (int k = 1; k < 3; k++) {
            if (p3[k]->x < minx) minx = p3[k]->x;
            if (p3[k]->x > maxx) maxx = p3[k]->x;
            if (p3[k]->y < miny) miny = p3[k]->y;
            if (p3[k]->y > maxy) maxy = p3[k]->y;
        }
        if (maxx - minx > 1023 || maxy - miny > 511) continue;    /* hardware rejects huge polys */
        Vert *o = alloc_verts(3);
        for (int k = 0; k < 3; k++) {
            const HwV *p = p3[k];
            /* no half-pixel shift: polygons must line up exactly with sprites at high resolution
             * (a shifted edge leaves a half-native-pixel gap next to a sprite) */
            put_vert(&o[k], (float)p->x, (float)p->y, gouraud ? p->r : v[0].r, gouraud ? p->g : v[0].g,
                     gouraud ? p->b : v[0].b, (float)p->u, (float)p->v, info);
        }
    }
    if (textured) add_reads(tpage, clut, umin, vmin, umax, vmax);
    before_sw_write(prim_bounds(v, nv));
}

void hw_rect(int x, int y, int w, int h, int r, int g, int b, int textured, int raw, int semi, int u0, int v0, u32 clut) {
    if (!ready() || w <= 0 || h <= 0) return;
    begin_prim(semi ? g_gpu.semi_mode : -1);
    /* UV runs one texel per pixel; flipped sprites run backwards from u0 */
    float ul = (float)u0, ur = (float)(u0 + w), vt = (float)v0, vb = (float)(v0 + h);
    int umin = u0, umax = u0 + w - 1, vmin = v0, vmax = v0 + h - 1;
    if (g_gpu.flip_x) { ul = (float)(u0 + 1); ur = (float)(u0 + 1 - w); umin = u0 - w + 1; umax = u0; }
    if (g_gpu.flip_y) { vt = (float)(v0 + 1); vb = (float)(v0 + 1 - h); vmin = v0 - h + 1; vmax = v0; }
    int lu0 = umin < 0 ? 0 : umin > 255 ? 255 : umin, lu1 = umax < 0 ? 0 : umax > 255 ? 255 : umax;
    int lv0 = vmin < 0 ? 0 : vmin > 255 ? 255 : vmin, lv1 = vmax < 0 ? 0 : vmax > 255 ? 255 : vmax;
    u32 tp = g_gpu.texpage;
    u32 flags = (textured ? 1u : 0) | (raw ? 2u : 0) | (semi ? 4u : 0) | (g_gpu.mask_set ? 8u : 0);
    if (!textured && x <= 0 && x + w >= 320) flags |= 16u;   /* screen-wide: widened on the wide canvas */
    u32 info[4] = { (tp & 0x1FF) | (flags << 16), clut & 0xFFFF, tw_bits(),
                    (u32)lu0 | ((u32)lv0 << 8) | ((u32)lu1 << 16) | ((u32)lv1 << 24) };
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    Vert *o = alloc_verts(6);
    put_vert(&o[0], x0, y0, r, g, b, ul, vt, info);
    put_vert(&o[1], x1, y0, r, g, b, ur, vt, info);
    put_vert(&o[2], x0, y1, r, g, b, ul, vb, info);
    put_vert(&o[3], x1, y0, r, g, b, ur, vt, info);
    put_vert(&o[4], x1, y1, r, g, b, ur, vb, info);
    put_vert(&o[5], x0, y1, r, g, b, ul, vb, info);
    if (textured) add_reads(tp, clut, lu0, lv0, lu1, lv1);
    HwV c[2] = { { x, y, 0, 0, 0, 0, 0 }, { x + w - 1, y + h - 1, 0, 0, 0, 0, 0 } };
    before_sw_write(prim_bounds(c, 2));
}

void hw_line(HwV a, HwV b, int gouraud, int semi) {
    if (!ready()) return;
    int dx = abs(b.x - a.x), dy = abs(b.y - a.y);
    int n = dx > dy ? dx : dy;
    if (n > 1023) return;
    begin_prim(semi ? g_gpu.semi_mode : -1);
    u32 flags = (semi ? 4u : 0) | (g_gpu.mask_set ? 8u : 0);
    u32 info[4] = { flags << 16, 0, 0, 0 };
    /* exactly the pixels the PS1 (and gpu.c) draws, one native-pixel quad each: a thinner or
     * wider line would leave slivers when the game later redraws just those pixels */
    Vert *o = alloc_verts((n + 1) * 6);
    for (int i = 0; i <= n; i++) {
        int x = n ? a.x + (b.x - a.x) * i / n : a.x;
        int y = n ? a.y + (b.y - a.y) * i / n : a.y;
        int r = a.r, g = a.g, bl = a.b;
        if (gouraud && n) { r += (b.r - a.r) * i / n; g += (b.g - a.g) * i / n; bl += (b.b - a.b) * i / n; }
        float x0 = (float)x, y0 = (float)y, x1 = x0 + 1, y1 = y0 + 1;
        put_vert(&o[0], x0, y0, r, g, bl, 0, 0, info);
        put_vert(&o[1], x1, y0, r, g, bl, 0, 0, info);
        put_vert(&o[2], x0, y1, r, g, bl, 0, 0, info);
        put_vert(&o[3], x1, y0, r, g, bl, 0, 0, info);
        put_vert(&o[4], x1, y1, r, g, bl, 0, 0, info);
        put_vert(&o[5], x0, y1, r, g, bl, 0, 0, info);
        o += 6;
    }
    HwV c[2] = { a, b };
    before_sw_write(prim_bounds(c, 2));
}

/* ------------------------------------------------------------------ VRAM transfers */
void hw_pre_write(int x, int y, int w, int h) {
    if (!ready()) return;
    Rc r = rc_make(x, y, w, h);
    if (list_hit(&s_reads, r)) flush();
}

void hw_upload(int x, int y, int w, int h) {
    if (!ready()) return;
    flush();
    Rc r = rc_make(x, y, w, h);
    list_add(&s_dirty, r);
    copy_native_to_scaled(r);
    /* writes that wrap past the VRAM edge: resync the wrapped part too */
    if (x + w > VRAM_W) { Rc r2 = rc_make(0, y, x + w - VRAM_W, h); list_add(&s_dirty, r2); copy_native_to_scaled(r2); }
    if (y + h > VRAM_H) { Rc r2 = rc_make(x, 0, w, y + h - VRAM_H); list_add(&s_dirty, r2); copy_native_to_scaled(r2); }
}

void hw_fill(int x, int y, int w, int h, u16 c) {
    if (!ready()) return;
    flush();
    Rc r = rc_make(x, y, w, h);
    list_add(&s_dirty, r);
    if (rc_empty(r)) return;
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x0 * s_scale, r.y0 * s_scale, (r.x1 - r.x0) * s_scale, (r.y1 - r.y0) * s_scale);
    glColorMask(1, 1, 1, 1);
    glClearColor((c & 31) * 8 / 255.0f, ((c >> 5) & 31) * 8 / 255.0f, ((c >> 10) & 31) * 8 / 255.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (wide_ensure() && r.x0 < WS_BUF_W && r.y0 < WS_BUF_H) {   /* the wide canvas: a full-width clear covers the sides */
        int E = s_wide_e, S = s_scale;
        int x0 = r.x0 == 0 && r.x1 >= WS_BUF_W ? 0 : r.x0 + E, x1 = r.x0 == 0 && r.x1 >= WS_BUF_W ? WS_BUF_W + 2 * E : r.x1 + E;
        int y1 = r.y1 > WS_BUF_H ? WS_BUF_H : r.y1;
        glBindFramebuffer(GL_FRAMEBUFFER, s_wide_fbo);
        glScissor(x0 * S, r.y0 * S, (x1 - x0) * S, (y1 - r.y0) * S);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    if (!ready()) return;
    flush();
    Rc d = rc_make(dx, dy, w, h);
    list_add(&s_dirty, d);
    if (sx + w > VRAM_W || sy + h > VRAM_H || dx + w > VRAM_W || dy + h > VRAM_H) {   /* wrapping: take native */
        copy_native_to_scaled(rc_make(0, 0, VRAM_W, VRAM_H));
        return;
    }
    if (sx < WS_BUF_W && sy < WS_BUF_H) vram_from_wide(rc_make(sx, sy, w, h));
    int S = s_scale, tw = w * S, th = h * S;
    if (tw > s_tmp_w || th > s_tmp_h) {             /* temp target for overlapping copies */
        if (s_tmp_fbo) glDeleteFramebuffers(1, &s_tmp_fbo);
        if (s_tmp) glDeleteTextures(1, &s_tmp);
        s_tmp_w = tw > s_tmp_w ? tw : s_tmp_w; s_tmp_h = th > s_tmp_h ? th : s_tmp_h;
        s_tmp = make_rgba_target(s_tmp_w, s_tmp_h, &s_tmp_fbo, 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_tmp_fbo);
    glBlitFramebuffer(sx * S, sy * S, (sx + w) * S, (sy + h) * S, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_tmp_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo);
    glBlitFramebuffer(0, 0, tw, th, dx * S, dy * S, (dx + w) * S, (dy + h) * S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (dx < WS_BUF_W && dy < WS_BUF_H) wide_from_vram(d);   /* (the wide sides are not copied) */
}

/* ------------------------------------------------------------------ debug */
/* Write the scaled VRAM (sampled at each native pixel's centre), the software VRAM and a
 * difference image as BMPs, to find where the two renderers disagree. */
int hw_debug_dump(const char *dir) {
    if (!ready()) return 0;
    flush();
    int S = s_scale, W = VRAM_W * S;
    u8 *hi = (u8 *)malloc((size_t)W * 4 * S);      /* S rows at a time */
    SDL_Surface *a = SDL_CreateRGBSurfaceWithFormat(0, VRAM_W, VRAM_H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *b = SDL_CreateRGBSurfaceWithFormat(0, VRAM_W, VRAM_H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Surface *d = SDL_CreateRGBSurfaceWithFormat(0, VRAM_W, VRAM_H, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!hi || !a || !b || !d) { free(hi); return 0; }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    int ndiff = 0;
    for (int y = 0; y < VRAM_H; y++) {
        glReadPixels(0, y * S, W, S, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, hi);
        u32 *ra = (u32 *)((u8 *)a->pixels + y * a->pitch), *rb = (u32 *)((u8 *)b->pixels + y * b->pitch);
        u32 *rd = (u32 *)((u8 *)d->pixels + y * d->pitch);
        for (int x = 0; x < VRAM_W; x++) {
            u32 h = ((u32 *)hi)[(S / 2) * W + x * S + S / 2];
            u16 v = g_vram[y * VRAM_W + x];
            u32 n = 0xFF000000u | ((u32)((v & 31) << 3) << 16) | ((u32)(((v >> 5) & 31) << 3) << 8) | (u32)(((v >> 10) & 31) << 3);
            ra[x] = h | 0xFF000000u; rb[x] = n;
            int dr = abs((int)((h >> 16) & 255) - (int)((n >> 16) & 255)), dg = abs((int)((h >> 8) & 255) - (int)((n >> 8) & 255)),
                db = abs((int)(h & 255) - (int)(n & 255));
            int m = dr > dg ? dr : dg; m = m > db ? m : db;
            rd[x] = m > 40 ? 0xFFFF0000u : (0xFF000000u | (u32)(m * 3) << 8);
            ndiff += m > 40;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    char p[1100];
    snprintf(p, sizeof p, "%svram_hw.bmp", dir); SDL_SaveBMP(a, p);
    snprintf(p, sizeof p, "%svram_sw.bmp", dir); SDL_SaveBMP(b, p);
    snprintf(p, sizeof p, "%svram_diff.bmp", dir); SDL_SaveBMP(d, p);
    rt_log("[hw] debug dump to %s: %d native pixels differ strongly; display %d,%d %dx%d; area %d,%d-%d,%d",
           dir, ndiff, g_gpu.disp_x, g_gpu.disp_y, g_gpu.disp_w, g_gpu.disp_h,
           g_gpu.area_x0, g_gpu.area_y0, g_gpu.area_x1, g_gpu.area_y1);
    SDL_FreeSurface(a); SDL_FreeSurface(b); SDL_FreeSurface(d); free(hi);
    return 1;
}

/* ------------------------------------------------------------------ display */
int hw_display_is_wide(void) { return s_last_wide; }

int hw_display_texture(unsigned *tex, int *w, int *h, int show_vram) {
    if (!ready()) return 0;
    flush();
    s_last_wide = 0;
    extern int g_ws_screen;   /* widescreen.c: this screen draws past its 320 pixels */
    if (!show_vram && g_ws_screen && wide_on() && g_gpu.disp_x == 0 && g_gpu.disp_w == WS_BUF_W && g_gpu.disp_h == 240 &&
        (g_gpu.disp_y == 0 || g_gpu.disp_y == 240) && g_gpu.disp_enabled) {
        wide_refresh();
        int S = s_scale, E = s_wide_e, tw = (WS_BUF_W + 2 * E) * S, th = 240 * S;
        if (tw != s_disp_w || th != s_disp_h) {
            if (s_disp_fbo) glDeleteFramebuffers(1, &s_disp_fbo);
            if (s_disp) glDeleteTextures(1, &s_disp);
            s_disp = make_rgba_target(tw, th, &s_disp_fbo, 0);
            s_disp_w = tw; s_disp_h = th;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_wide_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_disp_fbo);
        glBlitFramebuffer(0, g_gpu.disp_y * S, tw, (g_gpu.disp_y + 240) * S, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        extern int g_ws_mask_y;   /* the sides beside a 320-wide panel: black */
        if (g_ws_mask_y > 0 && g_ws_mask_y < 240) {
            glBindFramebuffer(GL_FRAMEBUFFER, s_disp_fbo);
            glEnable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 1);
            glScissor(0, g_ws_mask_y * S, E * S, (240 - g_ws_mask_y) * S);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor((E + WS_BUF_W) * S, g_ws_mask_y * S, E * S, (240 - g_ws_mask_y) * S);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        *tex = s_disp; *w = tw; *h = th;
        s_last_wide = 1;
        return 1;
    }
    if (s_vram_stale) { vram_from_wide(rc_make(0, 0, WS_BUF_W, WS_BUF_H)); s_vram_stale = 0; }
    int x0 = show_vram ? 0 : g_gpu.disp_x, y0 = show_vram ? 0 : g_gpu.disp_y;
    int dw = show_vram ? VRAM_W : g_gpu.disp_w, dh = show_vram ? VRAM_H : g_gpu.disp_h;
    if (x0 + dw > VRAM_W) dw = VRAM_W - x0;
    if (y0 + dh > VRAM_H) dh = VRAM_H - y0;
    if (dw <= 0 || dh <= 0) return 0;
    int S = s_scale, tw = dw * S, th = dh * S;
    if (tw != s_disp_w || th != s_disp_h) {
        if (s_disp_fbo) glDeleteFramebuffers(1, &s_disp_fbo);
        if (s_disp) glDeleteTextures(1, &s_disp);
        s_disp = make_rgba_target(tw, th, &s_disp_fbo, 0);
        s_disp_w = tw; s_disp_h = th;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_disp_fbo);
    glBlitFramebuffer(x0 * S, y0 * S, (x0 + dw) * S, (y0 + dh) * S, 0, 0, tw, th, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!g_gpu.disp_enabled && !show_vram) {        /* display blanked: show black */
        glBindFramebuffer(GL_FRAMEBUFFER, s_disp_fbo);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    *tex = s_disp; *w = tw; *h = th;
    return 1;
}
