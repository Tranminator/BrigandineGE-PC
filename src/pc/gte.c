/* GTE (COP2) implementation, written from the public psx-spx description. */
#include "rt.h"

#define D(c, i) ((c)->gte_d[i])
#define C(c, i) ((c)->gte_c[i])

static inline s32 lo16(s32 v) { return (s16)(v & 0xFFFF); }
static inline s32 hi16(s32 v) { return (s16)((u32)v >> 16); }
static inline s32 clamp(s32 v, s32 lo, s32 hi) { return v < lo ? lo : v > hi ? hi : v; }

/* matrix element (row, col) for matrix base control reg b (0=RT, 8=LLM, 16=LCM) */
static s32 mat(Ctx *c, int b, int r, int col) {
    int idx = r * 3 + col;
    s32 w = C(c, b + idx / 2);
    return (idx & 1) ? hi16(w) : lo16(w);
}

static void flag(Ctx *c, int bit) { C(c, 31) |= (1u << bit); }

static s64 mac_chk(Ctx *c, int i, s64 v) {
    if (v > 0x7FFFFFFFFFFLL) flag(c, 31 - i);         /* MACi overflow + */
    else if (v < -0x80000000000LL) flag(c, 28 - i);   /* MACi overflow - */
    return v;
}

static void set_ir(Ctx *c, int i, s64 v, int lm) {
    s32 lo = lm ? 0 : -0x8000;
    if (v < lo) { v = lo; flag(c, 25 - i); }
    else if (v > 0x7FFF) { v = 0x7FFF; flag(c, 25 - i); }
    D(c, 8 + i) = (s32)v;
}

static void set_mac_ir(Ctx *c, int i, s64 v, int sh, int lm) {
    mac_chk(c, i, v);
    s32 m = (s32)(v >> sh);
    D(c, 24 + i) = m;
    set_ir(c, i, m, lm);
}

static void push_sz(Ctx *c, s64 z) {
    if (z < 0) { z = 0; flag(c, 18); } else if (z > 0xFFFF) { z = 0xFFFF; flag(c, 18); }
    D(c, 16) = D(c, 17); D(c, 17) = D(c, 18); D(c, 18) = D(c, 19); D(c, 19) = (s32)z;
}

static void push_sxy(Ctx *c, s32 x, s32 y) {
    if (x < -0x400) { x = -0x400; flag(c, 14); } else if (x > 0x3FF) { x = 0x3FF; flag(c, 14); }
    if (y < -0x400) { y = -0x400; flag(c, 13); } else if (y > 0x3FF) { y = 0x3FF; flag(c, 13); }
    D(c, 12) = D(c, 13); D(c, 13) = D(c, 14);
    D(c, 14) = (s32)((u32)(x & 0xFFFF) | ((u32)y << 16));
}

static void push_color(Ctx *c) {
    s32 r = D(c, 25) >> 4, g = D(c, 26) >> 4, b = D(c, 27) >> 4;
    if (r < 0 || r > 255) { r = clamp(r, 0, 255); flag(c, 21); }
    if (g < 0 || g > 255) { g = clamp(g, 0, 255); flag(c, 20); }
    if (b < 0 || b > 255) { b = clamp(b, 0, 255); flag(c, 19); }
    D(c, 20) = D(c, 21); D(c, 21) = D(c, 22);
    D(c, 22) = (s32)((u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)D(c, 6) & 0xFF000000u));
}

static void vec(Ctx *c, int v, s32 out[3]) {
    if (v == 3) { out[0] = D(c, 9); out[1] = D(c, 10); out[2] = D(c, 11); return; }
    out[0] = lo16(D(c, v * 2)); out[1] = hi16(D(c, v * 2)); out[2] = lo16(D(c, v * 2 + 1));
}

static u32 divide(Ctx *c, u32 h, u32 sz) {
    if (h < sz * 2) {
        u64 q = (((u64)h << 17) / sz + 1) >> 1;
        if (q > 0x1FFFF) { flag(c, 17); q = 0x1FFFF; }
        return (u32)q;
    }
    flag(c, 17);
    return 0x1FFFF;
}

static void rtp(Ctx *c, int v, int sf, int last) {
    int sh = sf ? 12 : 0;
    s32 V[3]; vec(c, v, V);
    for (int i = 0; i < 3; i++) {
        s64 m = ((s64)C(c, 5 + i) << 12) + (s64)mat(c, 0, i, 0) * V[0] + (s64)mat(c, 0, i, 1) * V[1] + (s64)mat(c, 0, i, 2) * V[2];
        set_mac_ir(c, i + 1, m, sh, 0);
    }
    push_sz(c, (s64)D(c, 27) >> (sf ? 0 : 12));
    u32 q = divide(c, (u16)C(c, 26), (u32)D(c, 19));
    s64 sx = (s64)q * D(c, 9) + C(c, 24);
    s64 sy = (s64)q * D(c, 10) + C(c, 25);
    D(c, 24) = (s32)sy;
    push_sxy(c, (s32)(sx >> 16), (s32)(sy >> 16));
    if (last) {
        s64 m0 = (s64)q * (s16)C(c, 27) + C(c, 28);
        D(c, 24) = (s32)m0;
        s32 ir0 = (s32)(m0 >> 12);
        if (ir0 < 0 || ir0 > 0x1000) { ir0 = clamp(ir0, 0, 0x1000); flag(c, 12); }
        D(c, 8) = ir0;
    }
}

/* [MAC]=([T]<<12 + M*V) >> sh ; T = control base (or 0) */
static void mvmva(Ctx *c, int mbase, const s32 V[3], int tbase, int sh, int lm) {
    for (int i = 0; i < 3; i++) {
        s64 t = tbase >= 0 ? ((s64)C(c, tbase + i) << 12) : 0;
        s64 m = t + (s64)mat(c, mbase, i, 0) * V[0] + (s64)mat(c, mbase, i, 1) * V[1] + (s64)mat(c, mbase, i, 2) * V[2];
        set_mac_ir(c, i + 1, m, sh, lm);
    }
}

static void color_mul(Ctx *c, int sh, int lm) { /* [MAC] = [RGB]*[IR] << 4 */
    u32 rgb = (u32)D(c, 6);
    for (int i = 0; i < 3; i++) {
        s64 m = ((s64)((rgb >> (i * 8)) & 0xFF) * D(c, 9 + i)) << 4;
        set_mac_ir(c, i + 1, m, sh, lm);
    }
}

static void depth_cue_from_mac(Ctx *c, int sh, int lm) {
    /* [MAC] (unshifted values held in MAC<<sh) interpolated toward FC by IR0 */
    for (int i = 0; i < 3; i++) {
        s64 m = (s64)D(c, 25 + i) << sh;
        s64 d = ((s64)C(c, 21 + i) << 12) - m;
        mac_chk(c, i + 1, d);
        s32 di = (s32)(d >> sh);
        di = clamp(di, -0x8000, 0x7FFF);
        m = m + (s64)D(c, 8) * di;
        set_mac_ir(c, i + 1, m, sh, lm);
    }
}

static void light_ncs(Ctx *c, int v, int sh, int lm, int mode) {
    s32 V[3]; vec(c, v, V);
    mvmva(c, 8, V, -1, sh, lm);                       /* LLM * V */
    s32 I[3] = { D(c, 9), D(c, 10), D(c, 11) };
    mvmva(c, 16, I, 13, sh, lm);                      /* BK + LCM * IR */
    if (mode == 0) { /* NCS: color FIFO takes MAC directly */
    } else if (mode == 1) { color_mul(c, sh, lm); }   /* NCC */
    else { color_mul(c, sh, lm); depth_cue_from_mac(c, sh, lm); } /* NCD */
    push_color(c);
}

void gte_op(Ctx *c, u32 cmd) {
    int sf = (cmd >> 19) & 1, sh = sf ? 12 : 0, lm = (cmd >> 10) & 1;
    C(c, 31) = 0;
    switch (cmd & 0x3F) {
    case 0x01: rtp(c, 0, sf, 1); break;                                   /* RTPS */
    case 0x30: rtp(c, 0, sf, 0); rtp(c, 1, sf, 0); rtp(c, 2, sf, 1); break; /* RTPT */
    case 0x06: {                                                          /* NCLIP */
        s32 x0 = lo16(D(c, 12)), y0 = hi16(D(c, 12)), x1 = lo16(D(c, 13)), y1 = hi16(D(c, 13));
        s32 x2 = lo16(D(c, 14)), y2 = hi16(D(c, 14));
        s64 m = (s64)x0 * y1 + (s64)x1 * y2 + (s64)x2 * y0 - (s64)x0 * y2 - (s64)x1 * y0 - (s64)x2 * y1;
        D(c, 24) = (s32)m; break;
    }
    case 0x2D: case 0x2E: {                                               /* AVSZ3 / AVSZ4 */
        s64 m = (cmd & 0x3F) == 0x2D
            ? (s64)(s16)C(c, 29) * (D(c, 17) + D(c, 18) + D(c, 19))
            : (s64)(s16)C(c, 30) * (D(c, 16) + D(c, 17) + D(c, 18) + D(c, 19));
        D(c, 24) = (s32)m;
        s32 otz = (s32)(m >> 12);
        if (otz < 0 || otz > 0xFFFF) { otz = clamp(otz, 0, 0xFFFF); flag(c, 18); }
        D(c, 7) = otz; break;
    }
    case 0x12: {                                                          /* MVMVA */
        int mx = (cmd >> 17) & 3, v = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
        s32 V[3]; vec(c, v, V);
        int tb = cv == 0 ? 5 : cv == 1 ? 13 : cv == 2 ? 21 : -1;
        mvmva(c, mx == 0 ? 0 : mx == 1 ? 8 : 16, V, tb, sh, lm); break;
    }
    case 0x28:                                                            /* SQR */
        for (int i = 1; i <= 3; i++) set_mac_ir(c, i, (s64)D(c, 8 + i) * D(c, 8 + i), sh, lm);
        break;
    case 0x0C: {                                                          /* OP */
        s32 d1 = mat(c, 0, 0, 0), d2 = mat(c, 0, 1, 1), d3 = mat(c, 0, 2, 2);
        s32 i1 = D(c, 9), i2 = D(c, 10), i3 = D(c, 11);
        set_mac_ir(c, 1, (s64)i3 * d2 - (s64)i2 * d3, sh, lm);
        set_mac_ir(c, 2, (s64)i1 * d3 - (s64)i3 * d1, sh, lm);
        set_mac_ir(c, 3, (s64)i2 * d1 - (s64)i1 * d2, sh, lm);
        break;
    }
    case 0x3D:                                                            /* GPF */
        for (int i = 1; i <= 3; i++) set_mac_ir(c, i, (s64)D(c, 8) * D(c, 8 + i), sh, lm);
        push_color(c); break;
    case 0x3E:                                                            /* GPL */
        for (int i = 1; i <= 3; i++) set_mac_ir(c, i, ((s64)D(c, 24 + i) << sh) + (s64)D(c, 8) * D(c, 8 + i), sh, lm);
        push_color(c); break;
    case 0x1E: light_ncs(c, 0, sh, lm, 0); break;                         /* NCS */
    case 0x20: for (int v = 0; v < 3; v++) light_ncs(c, v, sh, lm, 0); break; /* NCT */
    case 0x1B: light_ncs(c, 0, sh, lm, 1); break;                         /* NCCS */
    case 0x3F: for (int v = 0; v < 3; v++) light_ncs(c, v, sh, lm, 1); break; /* NCCT */
    case 0x13: light_ncs(c, 0, sh, lm, 2); break;                         /* NCDS */
    case 0x16: for (int v = 0; v < 3; v++) light_ncs(c, v, sh, lm, 2); break; /* NCDT */
    case 0x1C: case 0x14: {                                               /* CC / CDP */
        s32 I[3] = { D(c, 9), D(c, 10), D(c, 11) };
        mvmva(c, 16, I, 13, sh, lm);
        color_mul(c, sh, lm);
        if ((cmd & 0x3F) == 0x14) depth_cue_from_mac(c, sh, lm);
        push_color(c); break;
    }
    case 0x10: case 0x2A: {                                               /* DPCS / DPCT */
        int n = (cmd & 0x3F) == 0x10 ? 1 : 3;
        for (int k = 0; k < n; k++) {
            u32 rgb = (u32)((cmd & 0x3F) == 0x10 ? D(c, 6) : D(c, 20));
            for (int i = 0; i < 3; i++) D(c, 25 + i) = (s32)(((s64)((rgb >> (i * 8)) & 0xFF) << 16) >> sh);
            depth_cue_from_mac(c, sh, lm);
            push_color(c);
        }
        break;
    }
    case 0x29:                                                            /* DCPL */
        color_mul(c, sh, lm); depth_cue_from_mac(c, sh, lm); push_color(c); break;
    case 0x11:                                                            /* INTPL */
        for (int i = 0; i < 3; i++) D(c, 25 + i) = (s32)(((s64)D(c, 9 + i) << 12) >> sh);
        depth_cue_from_mac(c, sh, lm); push_color(c); break;
    default:
        rt_log("GTE op %02X unimplemented", cmd & 0x3F);
    }
    if (C(c, 31) & 0x7F87E000u) C(c, 31) |= 0x80000000u;
}

/* ------------------------------------------------------------------ register moves */
static s32 lzcr(s32 v) {
    u32 x = (u32)(v < 0 ? ~v : v);
    int n = 0;
    while (n < 32 && !(x & 0x80000000u)) { x <<= 1; n++; }
    return n;
}

u32 gte_mfc2(Ctx *c, int r) {
    switch (r) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return (u32)lo16(D(c, r));
    case 7: case 16: case 17: case 18: case 19: return (u32)(D(c, r) & 0xFFFF);
    case 15: return (u32)D(c, 14);
    case 28: case 29: {
        u32 r_ = (u32)clamp(D(c, 9) >> 7, 0, 31), g = (u32)clamp(D(c, 10) >> 7, 0, 31), b = (u32)clamp(D(c, 11) >> 7, 0, 31);
        return r_ | (g << 5) | (b << 10);
    }
    case 31: return (u32)lzcr(D(c, 30));
    }
    return (u32)D(c, r);
}

void gte_mtc2(Ctx *c, int r, u32 v) {
    switch (r) {
    case 15: D(c, 12) = D(c, 13); D(c, 13) = D(c, 14); D(c, 14) = (s32)v; return;
    case 28:
        D(c, 9) = (s32)((v & 31) << 7); D(c, 10) = (s32)(((v >> 5) & 31) << 7); D(c, 11) = (s32)(((v >> 10) & 31) << 7);
        return;
    case 29: case 31: return;
    }
    D(c, r) = (s32)v;
}

u32 gte_cfc2(Ctx *c, int r) {
    switch (r) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return (u32)lo16(C(c, r));
    }
    return (u32)C(c, r);
}

void gte_ctc2(Ctx *c, int r, u32 v) {
    if (r == 31) { v &= 0x7FFFF000u; if (v & 0x7F87E000u) v |= 0x80000000u; }
    C(c, r) = (s32)v;
}
