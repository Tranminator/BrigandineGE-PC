/* PS1 movie frame decoder: BS v2 bitstream -> run/level -> dequant -> IDCT -> RGB24.
 * Software replacement for libpress (DecDCTvlc) + the MDEC chip. */
#include "mdec.h"
#include <math.h>
#include <stdlib.h>

static const u8 ZIGZAG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

/* PS1 default quantisation matrix: the MPEG-1 intra matrix except entry 0 (DC) = 2 */
static const u8 QMAT[64] = {
    2, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83 };

/* MPEG-1 Table B.14 (dct_coeff_next), without sign bit: code string -> run, level */
static const struct { const char *code; u8 run, level; } VLC[] = {
    {"11",0,1},{"011",1,1},{"0100",0,2},{"0101",2,1},{"00101",0,3},{"00111",3,1},{"00110",4,1},
    {"000110",1,2},{"000111",5,1},{"000101",6,1},{"000100",7,1},{"0000110",0,4},{"0000100",2,2},
    {"0000111",8,1},{"0000101",9,1},{"00100110",0,5},{"00100001",0,6},{"00100101",1,3},{"00100100",3,2},
    {"00100111",10,1},{"00100011",11,1},{"00100010",12,1},{"00100000",13,1},{"0000001010",0,7},
    {"0000001100",1,4},{"0000001011",2,3},{"0000001111",4,2},{"0000001001",5,2},{"0000001110",14,1},
    {"0000001101",15,1},{"0000001000",16,1},{"000000011101",0,8},{"000000011000",0,9},{"000000010011",0,10},
    {"000000010000",0,11},{"000000011011",1,5},{"000000010100",2,4},{"000000011100",3,3},{"000000010010",4,3},
    {"000000011110",6,2},{"000000010101",7,2},{"000000010001",8,2},{"000000011111",17,1},{"000000011010",18,1},
    {"000000011001",19,1},{"000000010111",20,1},{"000000010110",21,1},{"0000000011010",0,12},
    {"0000000011001",0,13},{"0000000011000",0,14},{"0000000010111",0,15},{"0000000010110",1,6},
    {"0000000010101",1,7},{"0000000010100",2,5},{"0000000010011",3,4},{"0000000010010",5,3},
    {"0000000010001",9,2},{"0000000010000",10,2},{"0000000011111",22,1},{"0000000011110",23,1},
    {"0000000011101",24,1},{"0000000011100",25,1},{"0000000011011",26,1},{"00000000011111",0,16},
    {"00000000011110",0,17},{"00000000011101",0,18},{"00000000011100",0,19},{"00000000011011",0,20},
    {"00000000011010",0,21},{"00000000011001",0,22},{"00000000011000",0,23},{"00000000010111",0,24},
    {"00000000010110",0,25},{"00000000010101",0,26},{"00000000010100",0,27},{"00000000010011",0,28},
    {"00000000010010",0,29},{"00000000010001",0,30},{"00000000010000",0,31},{"000000000011000",0,32},
    {"000000000010111",0,33},{"000000000010110",0,34},{"000000000010101",0,35},{"000000000010100",0,36},
    {"000000000010011",0,37},{"000000000010010",0,38},{"000000000010001",0,39},{"000000000010000",0,40},
    {"000000000011111",1,8},{"000000000011110",1,9},{"000000000011101",1,10},{"000000000011100",1,11},
    {"000000000011011",1,12},{"000000000011010",1,13},{"000000000011001",1,14},{"0000000000010011",1,15},
    {"0000000000010010",1,16},{"0000000000010001",1,17},{"0000000000010000",1,18},{"0000000000010100",6,3},
    {"0000000000011010",11,2},{"0000000000011001",12,2},{"0000000000011000",13,2},{"0000000000010111",14,2},
    {"0000000000010110",15,2},{"0000000000010101",16,2},{"0000000000011111",27,1},{"0000000000011110",28,1},
    {"0000000000011101",29,1},{"0000000000011100",30,1},{"0000000000011011",31,1},
};

/* Lookup by 16-bit peek: entry = len | run<<8 | level<<16 ; len 0 = invalid */
static u32 g_lut[65536];
static float g_cos[8][8];
static int g_init;

static void init_tables(void) {
    for (unsigned i = 0; i < sizeof VLC / sizeof VLC[0]; i++) {
        int len = (int)strlen(VLC[i].code);
        u32 code = 0;
        for (int k = 0; k < len; k++) code = (code << 1) | (VLC[i].code[k] == '1');
        u32 base = code << (16 - len), span = 1u << (16 - len);
        for (u32 j = 0; j < span; j++) g_lut[base + j] = (u32)len | ((u32)VLC[i].run << 8) | ((u32)VLC[i].level << 16);
    }
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++)
            g_cos[x][u] = (float)((u == 0 ? sqrt(0.5) : 1.0) * cos((2 * x + 1) * u * M_PI / 16.0) / 2.0);
    g_init = 1;
}

typedef struct { const u8 *p, *end; u32 buf; int bits; } Bits;

static void bits_fill(Bits *b) {
    while (b->bits <= 16) {
        u32 w = 0;
        if (b->p + 1 < b->end) { w = (u32)b->p[0] | ((u32)b->p[1] << 8); b->p += 2; }
        b->buf |= w << (16 - b->bits);
        b->bits += 16;
    }
}
static inline u32 bits_peek(Bits *b, int n) { bits_fill(b); return b->buf >> (32 - n); }
static inline void bits_skip(Bits *b, int n) { b->buf <<= n; b->bits -= n; }
static inline u32 bits_get(Bits *b, int n) { u32 v = bits_peek(b, n); bits_skip(b, n); return v; }

static void idct8x8(const s32 *in, s32 *out) {
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int u = 0; u < 8; u++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += g_cos[y][v] * in[v * 8 + u];
            tmp[y * 8 + u] = s;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += g_cos[x][u] * tmp[y * 8 + u];
            int v = (int)lrintf(s);
            out[y * 8 + x] = v < -128 ? -128 : v > 127 ? 127 : v;
        }
}

/* Decode one 8x8 block; returns 0 on bitstream error. */
static int decode_block(Bits *b, int qscale, s32 *pix) {
    s32 coef[64] = {0};
    s32 dc = (s32)bits_get(b, 10);
    if (dc & 0x200) dc -= 0x400;
    coef[0] = dc * QMAT[0];               /* MDEC clamps every coefficient to 11 bits */
    int k = 0;
    for (int guard = 0; guard < 64; guard++) {
        u32 pk = bits_peek(b, 16);
        if ((pk >> 14) == 2) { bits_skip(b, 2); break; }          /* EOB "10" */
        int run, level;
        if ((pk >> 10) == 1) {                                     /* escape 000001 */
            bits_skip(b, 6);
            run = (int)bits_get(b, 6);
            level = (int)bits_get(b, 10);
            if (level & 0x200) level -= 0x400;
        } else {
            u32 e = g_lut[pk];
            int len = e & 0xFF;
            if (!len) return 0;
            bits_skip(b, len);
            run = (e >> 8) & 0xFF; level = (int)(e >> 16);
            if (bits_get(b, 1)) level = -level;
        }
        k += run + 1;
        if (k > 63) return 0;
        int z = ZIGZAG[k];
        int cv = (level * QMAT[z] * qscale + 4) / 8;
        coef[z] = cv < -1024 ? -1024 : cv > 1023 ? 1023 : cv;
    }
    idct8x8(coef, pix);
    return 1;
}

static inline u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

int mdec_decode_frame(const u8 *bs, int len, int w, int h, u8 *rgb) {
    if (!g_init) init_tables();
    if (len < 8) return 0;
    int qscale = bs[4] | (bs[5] << 8);
    int ver = bs[6] | (bs[7] << 8);
    if ((bs[2] | (bs[3] << 8)) != 0x3800 || ver != 2) return 0;
    Bits b = { bs + 8, bs + len, 0, 0 };
    int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
    s32 cr[64], cb[64], y[4][64];
    for (int mx = 0; mx < mbw; mx++) {
        for (int my = 0; my < mbh; my++) {             /* PS1 order: columns of macroblocks */
            if (!decode_block(&b, qscale, cr) || !decode_block(&b, qscale, cb)) return 0;
            for (int i = 0; i < 4; i++) if (!decode_block(&b, qscale, y[i])) return 0;
            for (int py = 0; py < 16; py++) {
                int oy = my * 16 + py;
                if (oy >= h) break;
                for (int px = 0; px < 16; px++) {
                    int ox = mx * 16 + px;
                    if (ox >= w) continue;
                    int yy = y[(py >> 3) * 2 + (px >> 3)][(py & 7) * 8 + (px & 7)] + 128;
                    int ci = (py >> 1) * 8 + (px >> 1);
                    float r = yy + 1.402f * cr[ci];
                    float g = yy - 0.3437f * cb[ci] - 0.7143f * cr[ci];
                    float bl = yy + 1.772f * cb[ci];
                    u8 *o = rgb + (oy * w + ox) * 3;
                    o[0] = clamp8((int)lrintf(r)); o[1] = clamp8((int)lrintf(g)); o[2] = clamp8((int)lrintf(bl));
                }
            }
        }
    }
    return 1;
}
