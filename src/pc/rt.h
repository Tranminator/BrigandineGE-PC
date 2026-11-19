/* Runtime for recompiled PS1 code: CPU context, memory map, helpers. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <setjmp.h>

typedef uint8_t u8;   typedef int8_t s8;
typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32;
typedef uint64_t u64; typedef int64_t s64;

typedef struct Ctx {
    u32 r[32];
    u32 hi, lo;
    u32 cop0[32];
    /* GTE */
    s32 gte_d[32];
    s32 gte_c[32];
} Ctx;

typedef void (*RecompFn)(Ctx *c);
typedef struct { u32 addr; RecompFn fn; } FuncEntry;

#define RAM_SIZE 0x200000u
extern u8 g_ram[RAM_SIZE];
extern u8 g_scratch[0x400];

u32  mem_rd_slow(u32 a, int size);
void mem_wr_slow(u32 a, u32 v, int size);

/* KUSEG/KSEG0/KSEG1 all map to physical a & 0x1FFFFFFF; RAM mirrors in first 8MB. */
static inline u32 rd32(u32 a) {
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) { u32 v; memcpy(&v, g_ram + (p & (RAM_SIZE - 1)), 4); return v; }
    return mem_rd_slow(a, 4);
}
static inline u32 rd16(u32 a) {
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) { u16 v; memcpy(&v, g_ram + (p & (RAM_SIZE - 1)), 2); return v; }
    return mem_rd_slow(a, 2);
}
static inline u32 rd8(u32 a) {
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) return g_ram[p & (RAM_SIZE - 1)];
    return mem_rd_slow(a, 1);
}
static inline void wr32(u32 a, u32 v) {
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) { memcpy(g_ram + (p & (RAM_SIZE - 1)), &v, 4); return; }
    mem_wr_slow(a, v, 4);
}
static inline void wr16(u32 a, u16 v) {
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) { memcpy(g_ram + (p & (RAM_SIZE - 1)), &v, 2); return; }
    mem_wr_slow(a, v, 2);
}
static inline void wr8(u32 a, u8 v) {
    u32 p = a & 0x1FFFFFFFu;
    if (p < 0x00800000u) { g_ram[p & (RAM_SIZE - 1)] = v; return; }
    mem_wr_slow(a, v, 1);
}
/* Host pointer to PS1 RAM (for HLE code). */
static inline void *ram_ptr(u32 a) { return g_ram + (a & (RAM_SIZE - 1)); }

/* Unaligned loads/stores (little endian). */
static inline u32 rt_lwl(u32 rt, u32 a) {
    u32 sh = a & 3, w = rd32(a & ~3u);
    return (rt & (0x00FFFFFFu >> (sh * 8))) | (w << (24 - sh * 8));
}
static inline u32 rt_lwr(u32 rt, u32 a) {
    u32 sh = a & 3, w = rd32(a & ~3u);
    return (rt & (0xFFFFFF00u << ((3 - sh) * 8))) | (w >> (sh * 8));
}
static inline void rt_swl(u32 a, u32 rt) {
    u32 sh = a & 3, m = rd32(a & ~3u);
    wr32(a & ~3u, (m & (0xFFFFFF00u << (sh * 8))) | (rt >> (24 - sh * 8)));
}
static inline void rt_swr(u32 a, u32 rt) {
    u32 sh = a & 3, m = rd32(a & ~3u);
    wr32(a & ~3u, (m & (0x00FFFFFFu >> ((3 - sh) * 8))) | (rt << (sh * 8)));
}

static inline void rt_div(Ctx *c, u32 a, u32 b) {
    s32 n = (s32)a, d = (s32)b;
    if (d == 0) { c->hi = a; c->lo = n >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (a == 0x80000000u && d == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (u32)(n / d); c->hi = (u32)(n % d); }
}
static inline void rt_divu(Ctx *c, u32 a, u32 b) {
    if (b == 0) { c->hi = a; c->lo = 0xFFFFFFFFu; }
    else { c->lo = a / b; c->hi = a % b; }
}

/* Implemented in rt.c */
void rt_call(Ctx *c, u32 addr);
void rt_syscall(Ctx *c, u32 pc);
void rt_break(Ctx *c, u32 pc);
u32  rt_mfc0(Ctx *c, int r);
void rt_mtc0(Ctx *c, int r, u32 v);
void rt_rfe(Ctx *c);
void rt_bad_insn(Ctx *c, u32 pc, u32 w);
void rt_bad_label(Ctx *c, u32 pc);
jmp_buf *rt_jmpbuf(Ctx *c, u32 psx_buf);
void rt_longjmp_restore(Ctx *c);

/* GTE (gte.c) */
u32  gte_mfc2(Ctx *c, int r);
u32  gte_cfc2(Ctx *c, int r);
void gte_mtc2(Ctx *c, int r, u32 v);
void gte_ctc2(Ctx *c, int r, u32 v);
void gte_op(Ctx *c, u32 cmd);

void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);
