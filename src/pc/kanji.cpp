// Replacement for the BIOS kanji/full-width font (B0:51 Krom2RawAdd).
//
// The English patch draws some text through the BIOS font: it converts ASCII to
// Shift-JIS full-width codes and asks Krom2RawAdd for the 16x15 1bpp glyph in ROM.
// We have no BIOS (Sony's font is copyrighted), so glyphs are rasterised from a
// Japanese Gothic system font into a fake ROM area and Krom2RawAdd returns
// addresses there. Glyph format (as read by the game at 0x8001D004): 15 rows of
// 2 bytes; byte 0 = pixels 0-7, byte 1 = pixels 8-15, MSB = leftmost pixel.
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "rt.h"
}

#define ROM_BASE   0xBFC66000u   // where the real BIOS keeps its font
#define MAX_GLYPHS 512
#define GLYPH_SZ   30

static unsigned char s_rom[MAX_GLYPHS * GLYPH_SZ];
static unsigned short s_code[MAX_GLYPHS];
static int s_count;
static stbtt_fontinfo s_font;
static unsigned char *s_ttf;
static int s_font_state;   // 0 untried, 1 loaded, -1 none
static bool s_ascii_only;  // fallback font without full-width forms

// Bundled first: DotGothic16 (SIL OFL, Google Fonts) is a Japanese dot font drawn on a
// 16 px grid, the same style as the PS1 BIOS font. System fonts are fallbacks.
static const char *BUNDLED = "assets/fonts/DotGothic16-Regular.ttf";
static bool s_dot_font;      // bundled pixel font: keep its own pixel grid and spacing

static const char *FONTS[] = {
    "/System/Library/Fonts/ヒラギノ角ゴシック W5.ttc",   // macOS Hiragino Kaku Gothic
    "/System/Library/Fonts/ヒラギノ角ゴシック W4.ttc",
    "C:\\Windows\\Fonts\\msgothic.ttc",                  // Windows MS Gothic
    "C:\\Windows\\Fonts\\YuGothM.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   // Latin-only fallback
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
};

static bool try_font(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    s_ttf = (unsigned char *)malloc((size_t)n);
    if (fread(s_ttf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s_ttf); s_ttf = nullptr; return false; }
    fclose(f);
    if (!stbtt_InitFont(&s_font, s_ttf, stbtt_GetFontOffsetForIndex(s_ttf, 0))) { free(s_ttf); s_ttf = nullptr; return false; }
    s_font_state = 1;
    s_ascii_only = stbtt_FindGlyphIndex(&s_font, 0xFF21) == 0;   // no full-width 'Ａ'
    rt_log("[kanji] BIOS font replacement: %s%s", path, s_ascii_only ? " (Latin only)" : "");
    return true;
}

extern "C" const char *SDL_GetBasePath(void);
extern "C" void SDL_free(void *);

static void load_font(void) {
    s_font_state = -1;
    char path[1024];
    const char *force = getenv("BRIG_KANJI_FONT");     // debug: use this font file instead
    if (force && try_font(force)) { s_dot_font = strstr(force, "DotGothic") != nullptr; return; }
    const char *base = SDL_GetBasePath();             // folder of the executable
    if (base) {
        snprintf(path, sizeof path, "%s%s", base, BUNDLED);
        SDL_free((void *)base);
        if (try_font(path)) { s_dot_font = true; return; }
    }
    if (try_font(BUNDLED)) { s_dot_font = true; return; }   // run from the project folder
    for (const char *path : FONTS) {
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        s_ttf = (unsigned char *)malloc((size_t)n);
        if (fread(s_ttf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(s_ttf); s_ttf = nullptr; continue; }
        fclose(f);
        if (stbtt_InitFont(&s_font, s_ttf, stbtt_GetFontOffsetForIndex(s_ttf, 0))) {
            s_font_state = 1;
            s_ascii_only = stbtt_FindGlyphIndex(&s_font, 0xFF21) == 0;   // no full-width 'Ａ'
            rt_log("[kanji] BIOS font replacement: %s%s", path, s_ascii_only ? " (Latin only)" : "");
            return;
        }
        free(s_ttf); s_ttf = nullptr;
    }
    rt_log("[kanji] no usable system font: BIOS-font text will be blank");
}

// Shift-JIS (the codes the patch produces + common punctuation) -> Unicode. 0 = unsupported.
static unsigned sjis_to_unicode(unsigned c) {
    if (c == 0x8140) return 0x3000;                                   // ideographic space
    if (c >= 0x824F && c <= 0x8258) return 0xFF10 + (c - 0x824F);     // ０-９
    if (c >= 0x8260 && c <= 0x8279) return 0xFF21 + (c - 0x8260);     // Ａ-Ｚ
    if (c >= 0x8281 && c <= 0x829A) return 0xFF41 + (c - 0x8281);     // ａ-ｚ
    // The patch maps ASCII 0x21-0x2F to 0x8240-0x824E, which is unassigned in Shift-JIS:
    // draw the ASCII punctuation it stands for.
    if (c >= 0x8240 && c <= 0x824E) return 0xFF01 + (c - 0x8240);     // ！..／ (full-width forms)
    static const struct { unsigned short s, u; } P[] = {
        {0x8141,0x3001},{0x8142,0x3002},{0x8143,0xFF0C},{0x8144,0xFF0E},{0x8146,0xFF1A},{0x8147,0xFF1B},
        {0x8148,0xFF1F},{0x8149,0xFF01},{0x815B,0x30FC},{0x815E,0xFF0F},{0x8160,0xFF5E},{0x8165,0x2018},
        {0x8166,0x2019},{0x8167,0x201C},{0x8168,0x201D},{0x8169,0xFF08},{0x816A,0xFF09},{0x8175,0x300C},
        {0x8176,0x300D},{0x817B,0xFF0B},{0x817C,0xFF0D},{0x8181,0xFF1D},{0x8183,0xFF1C},{0x8184,0xFF1E},
        {0x8190,0xFF04},{0x8193,0xFF05},{0x8195,0xFF06},{0x8196,0xFF0A},{0x8197,0xFF20},
    };
    for (auto &p : P) if (p.s == c) return p.u;
    return 0;
}

static void render(unsigned unicode, unsigned char *out) {
    memset(out, 0, GLYPH_SZ);
    if (s_font_state != 1 || unicode == 0x3000) return;
    unsigned cp = unicode;
    if (s_ascii_only && cp >= 0xFF01 && cp <= 0xFF5E) cp -= 0xFEE0;    // full-width -> ASCII
    int gi = stbtt_FindGlyphIndex(&s_font, (int)cp);
    if (!gi) return;
    // dot font: em box = 16 px so every font dot lands on one pixel; others: fit 16 px tall
    float sc = s_dot_font ? stbtt_ScaleForMappingEmToPixels(&s_font, 16.0f) : stbtt_ScaleForPixelHeight(&s_font, 16.0f);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&s_font, &ascent, &descent, &gap);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&s_font, gi, sc, sc, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    unsigned char bmp[32 * 32] = {0};
    if (w > 32) w = 32;
    if (h > 32) h = 32;
    stbtt_MakeGlyphBitmap(&s_font, bmp, w, h, 32, sc, sc, gi);
    // baseline chosen once per font so that capitals ('Ａ' top) and descenders ('ｇ' bottom)
    // both fit inside the 15 rows of the cell
    static int s_baseline = -1000;
    if (s_baseline == -1000) {
        int ax0, ay0, ax1, ay1, gx0, gy0, gx1, gy1;
        int ga = stbtt_FindGlyphIndex(&s_font, s_ascii_only ? 'A' : 0xFF21);
        int gg = stbtt_FindGlyphIndex(&s_font, s_ascii_only ? 'g' : 0xFF47);
        stbtt_GetGlyphBitmapBox(&s_font, ga, sc, sc, &ax0, &ay0, &ax1, &ay1);
        stbtt_GetGlyphBitmapBox(&s_font, gg, sc, sc, &gx0, &gy0, &gx1, &gy1);
        s_baseline = (14 - gy1 - ay0) / 2;
        if (s_baseline + ay0 < 0) s_baseline = -ay0;
        (void)ascent; (void)descent; (void)gap;
    }
    int baseline = s_baseline;
    int ox = s_dot_font ? x0 : (16 - w) / 2;               // dot font: designed position in its 16 px advance
    for (int y = 0; y < h; y++) {
        int ry = baseline + y0 + y;
        if (ry < 0 || ry >= 15) continue;
        for (int x = 0; x < w; x++) {
            int rx = ox + x;
            if (rx < 0 || rx >= 16 || bmp[y * 32 + x] < (s_dot_font ? 128 : 110)) continue;
            out[ry * 2 + (rx >> 3)] |= (unsigned char)(0x80 >> (rx & 7));
        }
    }
}

// B0:51 Krom2RawAdd(sjis): address of the glyph, or -1 if the code has no glyph.
extern "C" u32 kanji_krom2rawadd(u32 sjis) {
    sjis &= 0xFFFF;
    unsigned u = sjis_to_unicode(sjis);
    if (!u) return 0xFFFFFFFFu;
    for (int i = 0; i < s_count; i++) if (s_code[i] == sjis) return ROM_BASE + (u32)i * GLYPH_SZ;
    if (s_count >= MAX_GLYPHS) return 0xFFFFFFFFu;
    if (!s_font_state) load_font();
    render(u, s_rom + s_count * GLYPH_SZ);
    s_code[s_count] = (unsigned short)sjis;
    return ROM_BASE + (u32)(s_count++) * GLYPH_SZ;
}

// Reads from the fake font ROM (physical address). Returns 1 if handled.
extern "C" int kanji_rom_read(u32 phys, int size, u32 *out) {
    u32 base = ROM_BASE & 0x1FFFFFFFu;
    if (phys < base || phys + size > base + sizeof s_rom) return 0;
    u32 v = 0;
    memcpy(&v, s_rom + (phys - base), (size_t)size);
    *out = v;
    return 1;
}

// debug: BRIG_KANJI_PREVIEW=<file.pgm> writes a sample line rendered with the glyphs
static void preview(const char *path) {
    const char *text = "Calmary Castle  Attack! 1234";
    int n = (int)strlen(text), W = n * 16, H = 15;
    unsigned char *img = (unsigned char *)calloc((size_t)W * H, 1);
    for (int i = 0; i < n; i++) {
        unsigned ch = (unsigned char)text[i];
        unsigned sj = ch < 0x21 ? ch + 0x8120 : ch < 0x5B ? ch + 0x821F : ch + 0x8220;   // the patch's mapping
        u32 a = kanji_krom2rawadd(sj);
        if (a == 0xFFFFFFFFu) continue;
        for (int y = 0; y < 15; y++) for (int x = 0; x < 16; x++) {
            u32 v; kanji_rom_read((a & 0x1FFFFFFFu) + y * 2 + (x >> 3), 1, &v);
            if (v & (0x80 >> (x & 7))) img[y * W + i * 16 + x] = 255;
        }
    }
    FILE *f = fopen(path, "wb");
    if (f) { fprintf(f, "P5 %d %d 255\n", W, H); fwrite(img, 1, (size_t)W * H, f); fclose(f); }
    free(img);
}

// debug: BRIG_KANJI_TEST=1 prints a few glyphs as ASCII art and exits
extern "C" void kanji_selftest(void) {
    if (getenv("BRIG_KANJI_PREVIEW")) { preview(getenv("BRIG_KANJI_PREVIEW")); return; }
    const unsigned codes[] = { 0x8260, 0x8287, 0x8250, 0x8243 };
    for (unsigned c : codes) {
        u32 a = kanji_krom2rawadd(c);
        printf("sjis %04X -> %08X\n", c, a);
        for (int y = 0; y < 15; y++) {
            for (int x = 0; x < 16; x++) {
                u32 v; kanji_rom_read((a & 0x1FFFFFFFu) + y * 2 + (x >> 3), 1, &v);
                putchar((v & (0x80 >> (x & 7))) ? '#' : '.');
            }
            putchar('\n');
        }
    }
}
