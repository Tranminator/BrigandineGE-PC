/* OpenGL presentation with shaders.
 *
 * The software GPU (gpu.c) still produces the PS1 display image; this module uploads it as
 * a texture and draws it into the window through a fragment shader. Shaders use the
 * RetroArch single-pass .glsl convention (one file, VERTEX / FRAGMENT sections, uniforms
 * MVPMatrix, Texture, TextureSize, InputSize, OutputSize, FrameCount, FrameDirection;
 * attributes VertexCoord, TexCoord, COLOR; "#pragma parameter" lines become sliders), so
 * many existing single-pass shaders can be dropped into the shader folder unchanged.
 * The built-in shaders below are written for this project.
 *
 * GL functions are loaded through SDL_GL_GetProcAddress (no GL headers or loader library
 * needed); the context is OpenGL 3.2 core, which macOS and Windows both provide. */
#include "video_gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
#define GLCC __stdcall
#else
#include <dirent.h>
#include <sys/stat.h>
#define GLCC
#endif

void rt_log(const char *fmt, ...);
void ui_toast(const char *msg);

/* ------------------------------------------------------------------ GL loader */
typedef unsigned int GLuint, GLenum, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef ptrdiff_t GLsizeiptr;

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGBA8 0x8058
#define GL_RGB8 0x8051
#define GL_RGB 0x1907
#define GL_BGRA 0x80E1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_TEXTURE0 0x84C0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAMEBUFFER 0x8D40
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44

#define GL_FUNCS \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, Clear, (GLbitfield)) \
    X(void, Disable, (GLenum)) \
    X(void, GenTextures, (GLsizei, GLuint *)) \
    X(void, BindTexture, (GLenum, GLuint)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint)) \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, PixelStorei, (GLenum, GLint)) \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(void, ActiveTexture, (GLenum)) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, DeleteProgram, (GLuint)) \
    X(void, UseProgram, (GLuint)) \
    X(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
    X(void, Uniform1i, (GLint, GLint)) \
    X(void, Uniform1f, (GLint, GLfloat)) \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
    X(void, GenVertexArrays, (GLsizei, GLuint *)) \
    X(void, BindVertexArray, (GLuint)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, VertexAttrib4f, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, BindFramebuffer, (GLenum, GLuint))

#define X(ret, name, args) typedef ret (GLCC *PFN_##name) args; static PFN_##name gl##name;
GL_FUNCS
#undef X

static int load_gl(void) {
    int ok = 1;
#define X(ret, name, args) \
    gl##name = (PFN_##name)SDL_GL_GetProcAddress("gl" #name); \
    if (!gl##name) { rt_log("[gl] missing gl" #name); ok = 0; }
    GL_FUNCS
#undef X
    return ok;
}

/* ------------------------------------------------------------------ built-in shaders */
#define BUILTIN_HEAD \
    "#if defined(VERTEX)\n" \
    "in vec4 VertexCoord; in vec4 TexCoord; out vec2 vTex;\n" \
    "uniform mat4 MVPMatrix;\n" \
    "void main() { gl_Position = MVPMatrix * VertexCoord; vTex = TexCoord.xy; }\n" \
    "#elif defined(FRAGMENT)\n" \
    "in vec2 vTex; out vec4 FragColor;\n" \
    "uniform sampler2D Texture; uniform vec2 TextureSize, InputSize, OutputSize; uniform int FrameCount;\n"
#define BUILTIN_TAIL "#endif\n"

/* Sharp: nearest-neighbour look, but pixel edges are blended over one output pixel so
 * non-integer scales do not give uneven pixel widths. Needs bilinear filtering. */
static const char SH_SHARP[] = BUILTIN_HEAD
    "vec3 sharp(vec2 uv) {\n"
    "  vec2 texel = uv * TextureSize;\n"
    "  vec2 scale = max(floor(OutputSize / InputSize), vec2(1.0));\n"
    "  vec2 c = fract(texel) - 0.5, r = 0.5 - 0.5 / scale;\n"
    "  vec2 f = (c - clamp(c, -r, r)) * scale + 0.5;\n"
    "  return texture(Texture, (floor(texel) + f) / TextureSize).rgb;\n"
    "}\n"
    "void main() { FragColor = vec4(sharp(vTex), 1.0); }\n" BUILTIN_TAIL;

/* Pixel / Smooth: plain nearest or bilinear (the filter does the work). */
static const char SH_PLAIN[] = BUILTIN_HEAD
    "void main() { FragColor = vec4(texture(Texture, vTex).rgb, 1.0); }\n" BUILTIN_TAIL;

/* Scanlines: dark gaps between the 240 source lines, brighter lines get wider. */
static const char SH_SCANLINES[] =
    "#pragma parameter SCAN_STRENGTH \"Scanline strength\" 0.55 0.0 1.0 0.05\n"
    "#pragma parameter SCAN_BRIGHT \"Brightness\" 1.15 0.8 1.6 0.05\n"
    BUILTIN_HEAD
    "uniform float SCAN_STRENGTH, SCAN_BRIGHT;\n"
    "void main() {\n"
    "  vec2 texel = vTex * TextureSize;\n"
    "  float sx = max(floor(OutputSize.x / InputSize.x), 1.0);\n"
    "  float cx = fract(texel.x) - 0.5, rx = 0.5 - 0.5 / sx;\n"
    "  float fx = (cx - clamp(cx, -rx, rx)) * sx + 0.5;\n"
    "  vec2 uv = vec2(floor(texel.x) + fx, floor(texel.y) + 0.5) / TextureSize;\n"
    "  vec3 c = texture(Texture, uv).rgb;\n"
    "  float l = max(max(c.r, c.g), c.b);\n"
    "  float d = fract(texel.y) - 0.5;\n"
    "  float w = mix(0.22, 0.36, l);\n"
    "  float beam = exp(-0.5 * d * d / (w * w));\n"
    "  c *= mix(1.0, beam * 1.2, SCAN_STRENGTH) * SCAN_BRIGHT;\n"
    "  FragColor = vec4(min(c, vec3(1.0)), 1.0);\n"
    "}\n" BUILTIN_TAIL;

/* CRT: curved screen, gaussian scanline beams whose width follows brightness,
 * aperture-grille mask, glow and vignette. Works in linear light. */
static const char SH_CRT[] =
    "#pragma parameter CRT_CURVATURE \"Curvature\" 0.06 0.0 0.25 0.01\n"
    "#pragma parameter CRT_SCANLINES \"Scanlines\" 0.7 0.0 1.0 0.05\n"
    "#pragma parameter CRT_MASK \"Shadow mask\" 0.25 0.0 1.0 0.05\n"
    "#pragma parameter CRT_GLOW \"Glow\" 0.12 0.0 0.6 0.02\n"
    "#pragma parameter CRT_BRIGHT \"Brightness\" 1.25 0.8 2.0 0.05\n"
    "#pragma parameter CRT_SHARP \"Horizontal sharpness\" 0.5 0.0 1.0 0.05\n"
    BUILTIN_HEAD
    "uniform float CRT_CURVATURE, CRT_SCANLINES, CRT_MASK, CRT_GLOW, CRT_BRIGHT, CRT_SHARP;\n"
    "vec3 lin(vec3 c) { return pow(c, vec3(2.2)); }\n"
    "vec3 fetch(vec2 p) {\n"   /* p in source pixels; x softened between nearest and linear */
    "  float x0 = floor(p.x - 0.5) + 0.5, f = p.x - x0;\n"
    "  f = mix(f, smoothstep(0.0, 1.0, f), CRT_SHARP);\n"
    "  f = mix(f, step(0.5, f), CRT_SHARP * 0.5);\n"
    "  vec3 a = lin(texture(Texture, vec2(x0, p.y) / TextureSize).rgb);\n"
    "  vec3 b = lin(texture(Texture, vec2(x0 + 1.0, p.y) / TextureSize).rgb);\n"
    "  return mix(a, b, f);\n"
    "}\n"
    "float beam(float d, vec3 c) {\n"
    "  float l = max(max(c.r, c.g), c.b);\n"
    "  float w = mix(0.20, 0.42, sqrt(l));\n"
    "  return exp(-0.5 * d * d / (w * w));\n"
    "}\n"
    "void main() {\n"
    "  vec2 scr = vTex * TextureSize / InputSize;\n"
    "  vec2 cc = scr * 2.0 - 1.0;\n"
    "  cc *= 1.0 + CRT_CURVATURE * (cc.yx * cc.yx);\n"
    "  vec2 w = cc * 0.5 + 0.5;\n"
    "  if (any(lessThan(w, vec2(0.0))) || any(greaterThan(w, vec2(1.0)))) { FragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "  vec2 p = w * InputSize;\n"
    "  float yl = p.y - 0.5, y0 = floor(yl), fy = yl - y0;\n"
    "  vec3 a = fetch(vec2(p.x, y0 + 0.5)), b = fetch(vec2(p.x, y0 + 1.5));\n"
    "  vec3 flat_ = mix(a, b, fy);\n"
    "  vec3 lines = a * beam(fy, a) + b * beam(1.0 - fy, b);\n"
    "  vec3 col = mix(flat_, lines * 1.15, CRT_SCANLINES);\n"
    "  vec3 glow = vec3(0.0);\n"
    "  for (int i = -2; i <= 2; i++) for (int j = -2; j <= 2; j++)\n"
    "    glow += lin(texture(Texture, (p + vec2(i, j) * 1.5) / TextureSize).rgb);\n"
    "  col += glow / 25.0 * CRT_GLOW;\n"
    "  int m = int(mod(gl_FragCoord.x, 3.0));\n"
    "  vec3 mask = vec3(1.0 - CRT_MASK); mask[m] = 1.0;\n"
    "  col *= mask * (1.0 + CRT_MASK * 0.7);\n"
    "  vec2 v = w * (1.0 - w.yx); col *= clamp(pow(v.x * v.y * 15.0, 0.18), 0.0, 1.0);\n"
    "  col *= CRT_BRIGHT;\n"
    "  FragColor = vec4(pow(clamp(col, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);\n"
    "}\n" BUILTIN_TAIL;

/* ------------------------------------------------------------------ shader list */
#define MAX_SHADERS 128
#define MAX_PARAMS  64
typedef struct { char name[64]; char path[1024]; const char *src; int linear; } ShaderEntry;
static ShaderEntry s_sh[MAX_SHADERS];
static int s_nsh, s_nbuiltin, s_cur = -1;
static VglParam s_par[MAX_PARAMS];
static int s_npar;
static int s_linear;
static char s_user_dir[1024], s_base_dir[1024], s_cfg_path[1024];

/* saved settings: current shader, per-shader filter and parameter values */
#define MAX_SAVED 512
typedef struct { char key[160]; float v; } Saved;
static Saved s_saved[MAX_SAVED];
static int s_nsaved;
static char s_saved_shader[64] = "Sharp";

static Saved *saved_find(const char *key, int create) {
    for (int i = 0; i < s_nsaved; i++) if (!strcmp(s_saved[i].key, key)) return &s_saved[i];
    if (!create || s_nsaved >= MAX_SAVED) return NULL;
    Saved *s = &s_saved[s_nsaved++];
    snprintf(s->key, sizeof s->key, "%s", key); s->v = 0;
    return s;
}

static void add_shader(const char *name, const char *path, const char *src, int linear) {
    if (s_nsh >= MAX_SHADERS) return;
    for (int i = 0; i < s_nsh; i++) if (!strcmp(s_sh[i].name, name)) return;   /* first one wins */
    ShaderEntry *e = &s_sh[s_nsh++];
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->path, sizeof e->path, "%s", path ? path : "");
    e->src = src; e->linear = linear;
}

static int name_cmp(const void *a, const void *b) {
    return strcmp(((const ShaderEntry *)a)->name, ((const ShaderEntry *)b)->name);
}

static void scan_dir(const char *dir) {
    int first = s_nsh;
#ifdef _WIN32
    char pat[1100]; snprintf(pat, sizeof pat, "%s*.glsl", dir);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[1100], name[64];
        snprintf(path, sizeof path, "%s%s", dir, fd.cFileName);
        snprintf(name, sizeof name, "%s", fd.cFileName);
        char *dot = strrchr(name, '.'); if (dot) *dot = 0;
        add_shader(name, path, NULL, 0);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".glsl")) continue;
        char path[1100], name[64];
        snprintf(path, sizeof path, "%s%s", dir, e->d_name);
        snprintf(name, sizeof name, "%s", e->d_name);
        char *dot = strrchr(name, '.'); if (dot) *dot = 0;
        add_shader(name, path, NULL, 0);
    }
    closedir(d);
#endif
    qsort(s_sh + first, s_nsh - first, sizeof s_sh[0], name_cmp);
}

static void build_list(void) {
    s_nsh = 0;
    add_shader("Sharp", NULL, SH_SHARP, 1);
    add_shader("Pixel", NULL, SH_PLAIN, 0);
    add_shader("Smooth", NULL, SH_PLAIN, 1);
    add_shader("Scanlines", NULL, SH_SCANLINES, 1);
    add_shader("CRT", NULL, SH_CRT, 1);
    s_nbuiltin = s_nsh;
    scan_dir(s_user_dir);
    if (s_base_dir[0]) scan_dir(s_base_dir);
}

/* ------------------------------------------------------------------ GL objects */
static SDL_Window *s_win;
static SDL_GLContext s_ctx;
static GLuint s_prog, s_vao, s_vbo, s_tex;
static int s_tex_w, s_tex_h;          /* allocated texture size == image size */
static int s_img_w, s_img_h;
static unsigned s_frame_count;
static GLint u_mvp, u_tex, u_texsize, u_insize, u_outsize, u_fc, u_fd;

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static GLuint compile_stage(GLenum type, const char *src, char *err, size_t errn) {
    const char *parts[3] = { "#version 150\n",
                             type == GL_VERTEX_SHADER ? "#define VERTEX\n#define PARAMETER_UNIFORM\n"
                                                      : "#define FRAGMENT\n#define PARAMETER_UNIFORM\n",
                             src };
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 3, parts, NULL);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(sh, (GLsizei)errn, NULL, err);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void parse_params(const char *src, const char *shader) {
    s_npar = 0;
    for (const char *l = src; l && *l; ) {
        const char *nl = strchr(l, '\n');
        while (*l == ' ' || *l == '\t') l++;
        if (!strncmp(l, "#pragma parameter", 17) && s_npar < MAX_PARAMS) {
            VglParam p; memset(&p, 0, sizeof p);
            p.step = 0;
            int n = sscanf(l, "#pragma parameter %47s \"%95[^\"]\" %f %f %f %f", p.name, p.desc, &p.def, &p.min, &p.max, &p.step);
            if (n >= 5) {
                int dup = 0;
                for (int i = 0; i < s_npar; i++) if (!strcmp(s_par[i].name, p.name)) dup = 1;
                if (!dup) {
                    if (p.step <= 0) p.step = (p.max - p.min) / 100.0f;
                    char key[160]; snprintf(key, sizeof key, "param.%s.%s", shader, p.name);
                    Saved *sv = saved_find(key, 0);
                    p.value = sv ? sv->v : p.def;
                    s_par[s_npar++] = p;
                }
            }
        }
        l = nl ? nl + 1 : NULL;
    }
}

static void apply_filter(void) {
    if (!s_tex) return;
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, s_linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, s_linear ? GL_LINEAR : GL_NEAREST);
}

static int load_shader(int idx, char *err, size_t errn) {
    ShaderEntry *e = &s_sh[idx];
    char *owned = NULL;
    const char *src = e->src;
    if (!src) {
        owned = read_file(e->path);
        if (!owned) { snprintf(err, errn, "cannot read %s", e->path); return 0; }
        /* the version is ours to choose: neutralise any #version line in the file */
        for (char *v = strstr(owned, "#version"); v; v = strstr(v, "#version")) { v[0] = '/'; v[1] = '/'; }
        src = owned;
    }
    GLuint vs = compile_stage(GL_VERTEX_SHADER, src, err, errn);
    GLuint fs = vs ? compile_stage(GL_FRAGMENT_SHADER, src, err, errn) : 0;
    if (!vs || !fs) { if (vs) glDeleteShader(vs); free(owned); return 0; }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "VertexCoord");
    glBindAttribLocation(p, 1, "TexCoord");
    glBindAttribLocation(p, 2, "COLOR");
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { glGetProgramInfoLog(p, (GLsizei)errn, NULL, err); glDeleteProgram(p); free(owned); return 0; }

    if (s_prog) glDeleteProgram(s_prog);
    s_prog = p; s_cur = idx;
    u_mvp = glGetUniformLocation(p, "MVPMatrix");
    u_tex = glGetUniformLocation(p, "Texture");
    u_texsize = glGetUniformLocation(p, "TextureSize");
    u_insize = glGetUniformLocation(p, "InputSize");
    u_outsize = glGetUniformLocation(p, "OutputSize");
    u_fc = glGetUniformLocation(p, "FrameCount");
    u_fd = glGetUniformLocation(p, "FrameDirection");
    parse_params(src, e->name);
    for (int i = 0; i < s_npar; i++) s_par[i].loc = glGetUniformLocation(p, s_par[i].name);
    char key[160]; snprintf(key, sizeof key, "filter.%s", e->name);
    Saved *sv = saved_find(key, 0);
    s_linear = sv ? sv->v != 0 : e->linear;
    apply_filter();
    snprintf(s_saved_shader, sizeof s_saved_shader, "%s", e->name);
    free(owned);
    return 1;
}

int vgl_select_shader(int i) {
    if (i < 0 || i >= s_nsh) return 0;
    char err[2048] = "";
    if (load_shader(i, err, sizeof err)) return 1;
    rt_log("[gl] shader \"%s\" failed:\n%s", s_sh[i].name, err);
    char msg[128]; snprintf(msg, sizeof msg, "Shader \"%s\" failed to compile (details in the log)", s_sh[i].name);
    ui_toast(msg);
    if (s_cur < 0 || s_cur == i) load_shader(0, err, sizeof err);   /* back to Sharp */
    return 0;
}

/* ------------------------------------------------------------------ config */
static void load_config(void) {
    FILE *f = fopen(s_cfg_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "shader=", 7)) { snprintf(s_saved_shader, sizeof s_saved_shader, "%s", line + 7); continue; }
        char *eq = strrchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        Saved *s = saved_find(line, 1);
        if (s) s->v = (float)atof(eq + 1);
    }
    fclose(f);
}

void vgl_save_config(void) {
    if (!s_cfg_path[0]) return;
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) return;
    fprintf(f, "# Brigandine GE native port: display shader settings\nshader=%s\n", s_saved_shader);
    for (int i = 0; i < s_nsaved; i++) fprintf(f, "%s=%g\n", s_saved[i].key, s_saved[i].v);
    fclose(f);
}

/* ------------------------------------------------------------------ public */
int vgl_init(SDL_Window *win) {
    s_win = win;
    s_ctx = SDL_GL_CreateContext(win);
    if (!s_ctx) { rt_log("[gl] context: %s", SDL_GetError()); return 0; }
    SDL_GL_MakeCurrent(win, s_ctx);
    if (!load_gl()) return 0;

    char *pref = SDL_GetPrefPath("PortGamePC", "Brigandine");
    if (pref) {
        snprintf(s_user_dir, sizeof s_user_dir, "%sshaders/", pref);
        snprintf(s_cfg_path, sizeof s_cfg_path, "%sshaders.cfg", pref);
        SDL_free(pref);
    }
    char *base = SDL_GetBasePath();
    if (base) { snprintf(s_base_dir, sizeof s_base_dir, "%sshaders/", base); SDL_free(base); }
    const char *env = getenv("BRIG_SHADER_CFG");     /* tests */
    if (env) snprintf(s_cfg_path, sizeof s_cfg_path, "%s", env);
    load_config();
    if ((env = getenv("BRIG_SHADER"))) snprintf(s_saved_shader, sizeof s_saved_shader, "%s", env);

    /* quad: VertexCoord (x, y, 0, 1) + TexCoord (u, v, 0, 0); bottom-left vertex samples the last image row */
    static const GLfloat quad[] = {
        0, 0, 0, 1,  0, 1, 0, 0,
        1, 0, 0, 1,  1, 1, 0, 0,
        0, 1, 0, 1,  0, 0, 0, 0,
        1, 1, 0, 1,  1, 0, 0, 0,
    };
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    build_list();
    int want = 0;
    for (int i = 0; i < s_nsh; i++) if (!strcmp(s_sh[i].name, s_saved_shader)) want = i;
    if (!vgl_select_shader(want) && s_cur < 0) return 0;
    return 1;
}

void *vgl_context(void) { return s_ctx; }
void vgl_set_vsync(int on) { SDL_GL_SetSwapInterval(on ? 1 : 0); }
void vgl_drawable_size(int *w, int *h) { SDL_GL_GetDrawableSize(s_win, w, h); }

void vgl_upload(const uint32_t *argb, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != s_tex_w || h != s_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
        s_tex_w = w; s_tex_h = h;
        apply_filter();
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
    }
    s_img_w = w; s_img_h = h;
}

void vgl_draw(int dx, int dy, int dw, int dh) {
    int ow, oh;
    SDL_GL_GetDrawableSize(s_win, &ow, &oh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glViewport(0, 0, ow, oh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!s_img_w || !s_prog || dw <= 0 || dh <= 0) return;
    glViewport(dx, oh - dy - dh, dw, dh);
    glUseProgram(s_prog);
    static const GLfloat mvp[16] = { 2, 0, 0, 0,  0, 2, 0, 0,  0, 0, -1, 0,  -1, -1, 0, 1 };
    glUniformMatrix4fv(u_mvp, 1, 0, mvp);
    glUniform1i(u_tex, 0);
    glUniform2f(u_texsize, (GLfloat)s_tex_w, (GLfloat)s_tex_h);
    glUniform2f(u_insize, (GLfloat)s_img_w, (GLfloat)s_img_h);
    glUniform2f(u_outsize, (GLfloat)dw, (GLfloat)dh);
    glUniform1i(u_fc, (GLint)s_frame_count++);
    glUniform1i(u_fd, 1);
    for (int i = 0; i < s_npar; i++) if (s_par[i].loc >= 0) glUniform1f(s_par[i].loc, s_par[i].value);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, 0, 8 * sizeof(GLfloat), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, 0, 8 * sizeof(GLfloat), (const void *)(4 * sizeof(GLfloat)));
    glDisableVertexAttribArray(2);
    glVertexAttrib4f(2, 1, 1, 1, 1);                 /* COLOR */
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glUseProgram(0);
    glViewport(0, 0, ow, oh);
}

int vgl_read_pixels(uint32_t *argb, int w, int h) {
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
    for (int y = 0; y < h / 2; y++)                  /* GL rows are bottom-up */
        for (int x = 0; x < w; x++) {
            uint32_t t = argb[y * w + x];
            argb[y * w + x] = argb[(h - 1 - y) * w + x];
            argb[(h - 1 - y) * w + x] = t;
        }
    return 1;
}

void vgl_swap(void) { SDL_GL_SwapWindow(s_win); }

int vgl_shader_count(void) { return s_nsh; }
const char *vgl_shader_name(int i) { return i >= 0 && i < s_nsh ? s_sh[i].name : ""; }
int vgl_shader_is_builtin(int i) { return i < s_nbuiltin; }
int vgl_shader_current(void) { return s_cur; }
int vgl_param_count(void) { return s_npar; }
VglParam *vgl_param(int i) { return i >= 0 && i < s_npar ? &s_par[i] : NULL; }
int vgl_filter_linear(void) { return s_linear; }

const char *vgl_shader_dir(void) {
#ifdef _WIN32
    CreateDirectoryA(s_user_dir, NULL);
#else
    mkdir(s_user_dir, 0755);
#endif
    return s_user_dir;
}

void vgl_param_changed(int i) {
    if (i < 0 || i >= s_npar || s_cur < 0) return;
    char key[160]; snprintf(key, sizeof key, "param.%s.%s", s_sh[s_cur].name, s_par[i].name);
    Saved *s = saved_find(key, 1);
    if (s) s->v = s_par[i].value;
}

void vgl_reset_params(void) {
    if (s_cur < 0) return;
    for (int i = 0; i < s_npar; i++) {
        s_par[i].value = s_par[i].def;
        char key[160]; snprintf(key, sizeof key, "param.%s.%s", s_sh[s_cur].name, s_par[i].name);
        Saved *s = saved_find(key, 0);
        if (s) s->v = s_par[i].def;
    }
}

void vgl_set_filter_linear(int on) {
    s_linear = on != 0;
    apply_filter();
    if (s_cur < 0) return;
    char key[160]; snprintf(key, sizeof key, "filter.%s", s_sh[s_cur].name);
    Saved *s = saved_find(key, 1);
    if (s) s->v = (float)s_linear;
}

void vgl_rescan_shaders(void) {
    char cur[64]; snprintf(cur, sizeof cur, "%s", s_cur >= 0 ? s_sh[s_cur].name : "Sharp");
    build_list();
    int want = 0;
    for (int i = 0; i < s_nsh; i++) if (!strcmp(s_sh[i].name, cur)) want = i;
    s_cur = -1;
    vgl_select_shader(want);
}

unsigned vgl_create_rgb_texture(int w, int h, const uint8_t *rgb) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return t;
}

void vgl_update_rgb_texture(unsigned tex, int w, int h, const uint8_t *rgb) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}
