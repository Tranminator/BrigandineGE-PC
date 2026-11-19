/* Core runtime: memory map, call dispatch, COP0, syscalls, BIOS HLE, setjmp. */
#include "rt.h"
#include "recomp_funcs.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

u8 g_ram[RAM_SIZE];
u8 g_scratch[0x400];

#define OVL_BASE 0x800B6838u

extern const FuncEntry g_funcs_main[], g_funcs_ov1[], g_funcs_ov2[], g_funcs_ov3[], g_funcs_ov4[], g_funcs_ov5[];
extern const int g_funcs_main_count, g_funcs_ov1_count, g_funcs_ov2_count, g_funcs_ov3_count,
    g_funcs_ov4_count, g_funcs_ov5_count;
extern const FuncEntry g_hle_funcs[];

void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ------------------------------------------------------------------ memory */
u32 hw_read(u32 phys, int size);
void hw_write(u32 phys, u32 v, int size);

u32 mem_rd_slow(u32 a, int size) {
    u32 p = a & 0x1FFFFFFFu;
    if (p >= 0x1F800000u && p < 0x1F800400u) {
        u32 v = 0; memcpy(&v, g_scratch + (p & 0x3FF), size); return v;
    }
    if (p >= 0x1F801000u && p < 0x1F803000u) return hw_read(p, size);
    if (p >= 0x1FC00000u) return 0; /* BIOS ROM */
    rt_log("read%d from unmapped 0x%08X", size * 8, a);
    return 0;
}

void mem_wr_slow(u32 a, u32 v, int size) {
    u32 p = a & 0x1FFFFFFFu;
    if (p >= 0x1F800000u && p < 0x1F800400u) { memcpy(g_scratch + (p & 0x3FF), &v, size); return; }
    if (p >= 0x1F801000u && p < 0x1F803000u) { hw_write(p, v, size); return; }
    if (a == 0xFFFE0130u) return; /* cache control */
    rt_log("write%d 0x%08X to unmapped 0x%08X", size * 8, v, a);
}

/* ------------------------------------------------------------------ dispatch */
static RecompFn find_fn(const FuncEntry *t, int n, u32 addr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (t[m].addr == addr) return t[m].fn;
        if (t[m].addr < addr) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

int rt_current_overlay(void) {
    u32 id = rd32(OVL_BASE);
    return (id >= 5 && id <= 9) ? (int)(id - 4) : 0;
}

void bios_call(Ctx *c, u32 table);
void interp_run(Ctx *c, u32 pc);

RecompFn rt_lookup(u32 addr) {
    addr |= 0x80000000u;
    for (const FuncEntry *h = g_hle_funcs; h->fn; h++)
        if (h->addr == addr) return h->fn;
    RecompFn f = find_fn(g_funcs_main, g_funcs_main_count, addr);
    if (!f && addr >= OVL_BASE) {
        switch (rt_current_overlay()) {
        case 1: f = find_fn(g_funcs_ov1, g_funcs_ov1_count, addr); break;
        case 2: f = find_fn(g_funcs_ov2, g_funcs_ov2_count, addr); break;
        case 3: f = find_fn(g_funcs_ov3, g_funcs_ov3_count, addr); break;
        case 4: f = find_fn(g_funcs_ov4, g_funcs_ov4_count, addr); break;
        case 5: f = find_fn(g_funcs_ov5, g_funcs_ov5_count, addr); break;
        }
    }
    return f;
}

void rt_call(Ctx *c, u32 addr) {
    addr |= 0x80000000u;
    if (addr == 0x800000A0u || addr == 0x800000B0u || addr == 0x800000C0u) { bios_call(c, addr & 0xFF); return; }
    RecompFn f = rt_lookup(addr);
    if (f) { f(c); return; }
    if (addr >= 0x80010000u && addr < 0x80200000u) {
        static u32 seen[256]; static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == addr) { known = 1; break; }
        if (!known && nseen < 256) { seen[nseen++] = addr; rt_log("interp: entering 0x%08X (ra=0x%08X, overlay=%d)", addr, c->r[31], rt_current_overlay()); }
        interp_run(c, addr);
        return;
    }
    rt_fatal("call to unknown address 0x%08X (ra=0x%08X, overlay=%d)", addr, c->r[31], rt_current_overlay());
}

/* ------------------------------------------------------------------ COP0 / exceptions */
u32 rt_mfc0(Ctx *c, int r) { return c->cop0[r]; }
void rt_mtc0(Ctx *c, int r, u32 v) { c->cop0[r] = v; }
void rt_rfe(Ctx *c) { (void)c; }
void rt_break(Ctx *c, u32 pc) { rt_log("break at 0x%08X (ra=0x%08X)", pc, c->r[31]); }
void rt_bad_insn(Ctx *c, u32 pc, u32 w) { rt_fatal("bad insn %08X at 0x%08X (ra=0x%08X)", w, pc, c->r[31]); }
void rt_bad_label(Ctx *c, u32 pc) { rt_fatal("jump to missing label 0x%08X (ra=0x%08X)", pc, c->r[31]); }

void rt_syscall(Ctx *c, u32 pc) {
    (void)pc;
    switch (c->r[4]) {
    case 1: c->r[2] = 1; break; /* EnterCriticalSection */
    case 2: break;              /* ExitCriticalSection */
    default: rt_log("syscall a0=%u", c->r[4]); break;
    }
}

/* ------------------------------------------------------------------ setjmp/longjmp
 * Host setjmp lives in the recompiled caller; PSX jmp_buf address keys the host buffer. */
#define MAX_JMP 8
static struct { u32 psx; jmp_buf jb; u32 regs[32]; u32 ret; } g_jmp[MAX_JMP];
static int g_jmp_last = -1;

static int jmp_slot(u32 psx) {
    for (int i = 0; i < MAX_JMP; i++) if (g_jmp[i].psx == psx) return i;
    for (int i = 0; i < MAX_JMP; i++) if (!g_jmp[i].psx) { g_jmp[i].psx = psx; return i; }
    rt_fatal("too many jmp_bufs");
    return 0;
}

jmp_buf *rt_jmpbuf(Ctx *c, u32 psx) { (void)c; return &g_jmp[jmp_slot(psx)].jb; }

void hle_setjmp(Ctx *c) {
    int s = jmp_slot(c->r[4]);
    memcpy(g_jmp[s].regs, c->r, sizeof c->r);
    c->r[2] = 0;
}

void rt_longjmp_restore(Ctx *c) {
    int s = g_jmp_last;
    memcpy(c->r, g_jmp[s].regs, sizeof c->r);
    c->r[2] = g_jmp[s].ret;
}

void hle_longjmp(Ctx *c) {
    int s = jmp_slot(c->r[4]);
    g_jmp[s].ret = c->r[5] ? c->r[5] : 1;
    g_jmp_last = s;
    longjmp(g_jmp[s].jb, 1);
}

/* ------------------------------------------------------------------ BIOS */
static u32 g_heap_base, g_heap_size, g_heap_ptr;

static u32 heap_alloc(u32 n) {
    n = (n + 7) & ~7u;
    if (g_heap_ptr + n > g_heap_base + g_heap_size) { rt_log("BIOS malloc(%u) failed", n); return 0; }
    u32 p = g_heap_ptr; g_heap_ptr += n; return p;
}

void bios_call(Ctx *c, u32 table) {
    u32 fn = c->r[9];
    u32 a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    if (table == 0xA0) {
        switch (fn) {
        case 0x39: g_heap_base = g_heap_ptr = a0; g_heap_size = a1; return;           /* InitHeap */
        case 0x33: c->r[2] = heap_alloc(a0); return;                                 /* malloc */
        case 0x34: return;                                                           /* free */
        case 0x3F: rt_log("[printf] fmt@%08X", a0); return;                          /* printf */
        case 0x44: return;                                                           /* FlushCache */
        case 0x2A: memcpy(ram_ptr(a0), ram_ptr(a1), a2); c->r[2] = a0; return;       /* memcpy */
        case 0x28: memset(ram_ptr(a0), 0, a1); return;                               /* bzero */
        case 0x2B: memset(ram_ptr(a0), a1 & 0xFF, a2); c->r[2] = a0; return;         /* memset */
        case 0x72: case 0x96: case 0x49: case 0x97: return;                          /* CdRemove, AddCDROMDevice, GPU_cw.. */
        case 0xA3: return;                                                           /* DequeueCdIntr */
        }
    } else if (table == 0xB0) {
        switch (fn) {
        case 0x08: c->r[2] = 0xF1000001u; return; /* OpenEvent */
        case 0x09: c->r[2] = 1; return;           /* CloseEvent */
        case 0x0A: c->r[2] = 0; return;           /* WaitEvent */
        case 0x0B: c->r[2] = 0; return;           /* TestEvent */
        case 0x0C: c->r[2] = 1; return;           /* EnableEvent */
        case 0x0D: c->r[2] = 1; return;           /* DisableEvent */
        case 0x07: c->r[2] = 1; return;           /* DeliverEvent */
        case 0x17: return;                        /* ReturnFromException */
        case 0x18: return;                        /* ResetEntryInt */
        case 0x19: return;                        /* HookEntryInt */
        case 0x12: case 0x13: case 0x14: return;  /* InitPad, StartPad, StopPad */
        case 0x4A: case 0x4B: case 0x4C: return;  /* InitCard, StartCard, StopCard */
        case 0x5B: return;                        /* ChangeClearPad */
        case 0x56: c->r[2] = 0x80000E00u; return; /* GetC0Table (dummy) */
        case 0x57: c->r[2] = 0x80000E80u; return; /* GetB0Table (dummy) */
        case 0x3D: putc(a0 & 0xFF, stderr); return;/* putchar */
        }
    } else if (table == 0xC0) {
        switch (fn) {
        case 0x00: case 0x01: case 0x02: case 0x03: return; /* EnqueueTimerAndVblankIrqs etc. */
        case 0x0A: return;                                  /* ChangeClearRCnt */
        }
    }
    rt_log("BIOS %02X:%02X unimplemented (ra=0x%08X)", table, fn, c->r[31]);
    c->r[2] = 0;
}
