/* Hardware register window 0x1F801000-0x1F802FFF.
 * Most hardware is HLE'd at library level; this only answers stray accesses. */
#include "rt.h"

static u32 g_regs[0x2000 / 4];
static u8  g_logged[0x2000 / 4];

u32 hw_read(u32 p, int size) {
    u32 off = p - 0x1F801000u;
    switch (p & ~3u) {
    case 0x1F801814: return 0x1C000000u; /* GPUSTAT: ready for everything */
    case 0x1F801810: return 0;
    }
    if (!g_logged[off / 4]) { g_logged[off / 4] = 1; rt_log("hw read%d 0x%08X", size * 8, p); }
    return g_regs[off / 4];
}

void hw_write(u32 p, u32 v, int size) {
    u32 off = p - 0x1F801000u;
    if (!g_logged[off / 4]) { g_logged[off / 4] = 1; rt_log("hw write%d 0x%08X = 0x%08X", size * 8, p, v); }
    g_regs[off / 4] = v;
}
