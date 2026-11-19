/* Fallback MIPS-I interpreter.
 * Used when control reaches an address the recompiler did not identify as a function
 * entry (mostly code inserted by the English patch). Calls out to recompiled/HLE code
 * via rt_call and returns on `jr $ra`, so it composes with the recompiled call model. */
#include "rt.h"

RecompFn rt_lookup(u32 addr);
void bios_call(Ctx *c, u32 table);

static inline s32 sx16(u32 v) { return (s16)(v & 0xFFFF); }

/* Execute one non-control-flow instruction. Returns 0 if it is not one. */
static int exec_simple(Ctx *c, u32 pc, u32 w) {
    u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31, sa = (w >> 6) & 31, fn = w & 63;
    u32 *R = c->r;
    u32 ea = R[rs] + (u32)sx16(w);
    u32 v = 0; int wr = -1;
    switch (op) {
    case 0x00:
        switch (fn) {
        case 0x00: v = R[rt] << sa; wr = rd; break;
        case 0x02: v = R[rt] >> sa; wr = rd; break;
        case 0x03: v = (u32)((s32)R[rt] >> sa); wr = rd; break;
        case 0x04: v = R[rt] << (R[rs] & 31); wr = rd; break;
        case 0x06: v = R[rt] >> (R[rs] & 31); wr = rd; break;
        case 0x07: v = (u32)((s32)R[rt] >> (R[rs] & 31)); wr = rd; break;
        case 0x0C: rt_syscall(c, pc); return 1;
        case 0x0D: rt_break(c, pc); return 1;
        case 0x10: v = c->hi; wr = rd; break;
        case 0x11: c->hi = R[rs]; return 1;
        case 0x12: v = c->lo; wr = rd; break;
        case 0x13: c->lo = R[rs]; return 1;
        case 0x18: { s64 p = (s64)(s32)R[rs] * (s32)R[rt]; c->lo = (u32)p; c->hi = (u32)(p >> 32); return 1; }
        case 0x19: { u64 p = (u64)R[rs] * R[rt]; c->lo = (u32)p; c->hi = (u32)(p >> 32); return 1; }
        case 0x1A: rt_div(c, R[rs], R[rt]); return 1;
        case 0x1B: rt_divu(c, R[rs], R[rt]); return 1;
        case 0x20: case 0x21: v = R[rs] + R[rt]; wr = rd; break;
        case 0x22: case 0x23: v = R[rs] - R[rt]; wr = rd; break;
        case 0x24: v = R[rs] & R[rt]; wr = rd; break;
        case 0x25: v = R[rs] | R[rt]; wr = rd; break;
        case 0x26: v = R[rs] ^ R[rt]; wr = rd; break;
        case 0x27: v = ~(R[rs] | R[rt]); wr = rd; break;
        case 0x2A: v = (s32)R[rs] < (s32)R[rt]; wr = rd; break;
        case 0x2B: v = R[rs] < R[rt]; wr = rd; break;
        default: return 0;
        }
        break;
    case 0x08: case 0x09: v = R[rs] + (u32)sx16(w); wr = rt; break;
    case 0x0A: v = (s32)R[rs] < sx16(w); wr = rt; break;
    case 0x0B: v = R[rs] < (u32)sx16(w); wr = rt; break;
    case 0x0C: v = R[rs] & (w & 0xFFFF); wr = rt; break;
    case 0x0D: v = R[rs] | (w & 0xFFFF); wr = rt; break;
    case 0x0E: v = R[rs] ^ (w & 0xFFFF); wr = rt; break;
    case 0x0F: v = (w & 0xFFFF) << 16; wr = rt; break;
    case 0x10:
        if (rs == 0) { v = rt_mfc0(c, rd); wr = rt; break; }
        if (rs == 4) { rt_mtc0(c, rd, R[rt]); return 1; }
        if (rs == 0x10 && fn == 0x10) { rt_rfe(c); return 1; }
        return 0;
    case 0x12:
        if (rs == 0) { v = gte_mfc2(c, rd); wr = rt; break; }
        if (rs == 2) { v = gte_cfc2(c, rd); wr = rt; break; }
        if (rs == 4) { gte_mtc2(c, rd, R[rt]); return 1; }
        if (rs == 6) { gte_ctc2(c, rd, R[rt]); return 1; }
        if (rs & 0x10) { gte_op(c, w & 0x1FFFFFF); return 1; }
        return 0;
    case 0x20: v = (u32)(s8)rd8(ea); wr = rt; break;
    case 0x21: v = (u32)(s16)rd16(ea); wr = rt; break;
    case 0x22: v = rt_lwl(R[rt], ea); wr = rt; break;
    case 0x23: v = rd32(ea); wr = rt; break;
    case 0x24: v = rd8(ea); wr = rt; break;
    case 0x25: v = rd16(ea); wr = rt; break;
    case 0x26: v = rt_lwr(R[rt], ea); wr = rt; break;
    case 0x28: wr8(ea, (u8)R[rt]); return 1;
    case 0x29: wr16(ea, (u16)R[rt]); return 1;
    case 0x2A: rt_swl(ea, R[rt]); return 1;
    case 0x2B: wr32(ea, R[rt]); return 1;
    case 0x2E: rt_swr(ea, R[rt]); return 1;
    case 0x32: gte_mtc2(c, rt, rd32(ea)); return 1;
    case 0x3A: wr32(ea, gte_mfc2(c, rt)); return 1;
    default: return 0;
    }
    if (wr > 0) R[wr] = v;
    return 1;
}

static int is_cf(u32 w) {
    u32 op = w >> 26;
    return (op >= 1 && op <= 7) || (op == 0 && ((w & 63) == 8 || (w & 63) == 9));
}

/* Jump to target: known function -> call it and finish; otherwise keep interpreting. */
static int jump_to(Ctx *c, u32 t, u32 *pc) {
    if (rt_lookup(t)) { rt_call(c, t); return 1; }
    *pc = t;
    return 0;
}

void interp_run(Ctx *c, u32 pc) {
    for (;;) {
        u32 w = rd32(pc);
        if (!is_cf(w)) {
            if (!exec_simple(c, pc, w)) rt_fatal("interp: bad insn %08X at 0x%08X", w, pc);
            pc += 4;
            continue;
        }
        u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
        u32 *R = c->r;
        u32 btgt = pc + 4 + ((u32)sx16(w) << 2);
        u32 jtgt = (pc & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
        u32 rsv = R[rs];
        int taken = 0, link = 0;
        u32 target = 0;
        switch (op) {
        case 0x00: target = rsv; taken = 1; link = (w & 63) == 9; break;
        case 0x01: taken = (rt & 1) ? (s32)rsv >= 0 : (s32)rsv < 0; target = btgt; link = (rt & 0x10) != 0; break;
        case 0x02: target = jtgt; taken = 1; break;
        case 0x03: target = jtgt; taken = 1; link = 1; break;
        case 0x04: taken = rsv == R[rt]; target = btgt; break;
        case 0x05: taken = rsv != R[rt]; target = btgt; break;
        case 0x06: taken = (s32)rsv <= 0; target = btgt; break;
        case 0x07: taken = (s32)rsv > 0; target = btgt; break;
        }
        u32 ds = rd32(pc + 4);
        if (!exec_simple(c, pc + 4, ds) && ds) rt_fatal("interp: bad delay slot %08X at 0x%08X", ds, pc + 4);
        if (link) {
            u32 linkreg = (op == 0 && (w & 63) == 9) ? ((w >> 11) & 31) : 31;
            if (linkreg) R[linkreg] = pc + 8;
            if (taken) rt_call(c, target);
            pc += 8;
            continue;
        }
        if (op == 0x00) {
            if (rs == 31) return;                 /* jr $ra */
            if (jump_to(c, target, &pc)) return;   /* jr reg */
            continue;
        }
        if (op == 0x02) {
            if (jump_to(c, target, &pc)) return;
            continue;
        }
        pc = taken ? target : pc + 8;
    }
}
