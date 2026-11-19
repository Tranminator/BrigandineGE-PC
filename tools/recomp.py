#!/usr/bin/env python3
"""Static recompiler: PS1 MIPS-I machine code -> C.

Reads splat asm only for function boundaries/names, decodes instructions from the
original binaries itself, and writes C that runs against the runtime in src/pc
(Ctx registers + emulated 2 MB RAM).

usage: recomp.py <project_root>
"""
import json
import os
import re
import struct
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else "."
OUT = os.path.join(ROOT, "build", "gen")
OVL_BASE = 0x800B6838
PATCH_LO, PATCH_HI = 0x8006C620, 0x8006FC00  # code inserted by the English patch

RE_GLABEL = re.compile(r"^glabel (\S+)")
RE_INSN = re.compile(r"/\* [0-9A-F]+ ([0-9A-F]{8}) ")


def read_funcs(asm_files):
    funcs = []
    for p in asm_files:
        pending = None
        for line in open(p):
            m = RE_GLABEL.match(line)
            if m:
                pending = m.group(1)
                continue
            if pending:
                m = RE_INSN.search(line)
                if m:
                    funcs.append((int(m.group(1), 16), pending))
                    pending = None
    return funcs


class Segment:
    def __init__(self, sid, data, vram, code_start, code_end, asm_files):
        self.sid = sid                  # "main", "ov1".."ov5"
        self.data = data
        self.vram = vram
        self.code_start = code_start
        self.code_end = code_end
        self.words = struct.unpack_from("<%dI" % (len(data) // 4), data)
        self.names = {}
        for a, n in read_funcs(asm_files):
            if code_start <= a < code_end:
                self.names[a] = n

    def word(self, addr):
        return self.words[(addr - self.vram) // 4]

    def in_code(self, a):
        return self.code_start <= a < self.code_end

    def fname(self, a):
        return f"f_{self.sid}_{a:08X}"


def load_segments():
    exe = open(os.path.join(ROOT, "extracted/disc1/SLPS_026.61"), "rb").read()
    main = Segment("main", exe[0x800:], 0x80010000, 0x8001A4E0, 0x8006FC00,
                   [os.path.join(ROOT, "decomp/asm/game.s"), os.path.join(ROOT, "decomp/asm/psyq.s")])
    segs = [main]
    for n in range(1, 6):
        d = open(os.path.join(ROOT, f"extracted/disc1/OVER{n}.BIN"), "rb").read()
        d += b"\0" * (-len(d) % 4)
        segs.append(Segment(f"ov{n}", d, OVL_BASE, OVL_BASE + 8, OVL_BASE + len(d),
                            [os.path.join(ROOT, f"decomp/asm/over{n}/code.s")]))
    return segs


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def jal_targets(seg):
    out = set()
    for a in range(seg.code_start, seg.code_end, 4):
        w = seg.word(a)
        if w >> 26 == 3:
            t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if seg.in_code(t):
                out.add(t)
    return out


def reach_end(seg, start, limit, data_ptrs):
    """Highest reachable address (exclusive) of a function starting at start, bounded by limit."""
    seen, work = set(), [start]
    cases = [t for t in data_ptrs if start <= t < limit]
    while work:
        a = work.pop()
        while start <= a < limit and a not in seen:
            seen.add(a)
            w = seg.word(a)
            op, fn, rs, rt = w >> 26, w & 63, (w >> 21) & 31, (w >> 16) & 31
            if op in (1, 4, 5, 6, 7):
                seen.add(a + 4)
                t = a + 4 + (sext16(w & 0xFFFF) << 2)
                work.append(t)
                if op == 4 and rs == 0 and rt == 0:
                    break
                a += 8
                continue
            if op == 2:
                seen.add(a + 4)
                work.append((a & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
                break
            if op == 3 or (op == 0 and fn == 9):
                seen.add(a + 4)
                a += 8
                continue
            if op == 0 and fn == 8:
                seen.add(a + 4)
                if rs != 31:
                    work.extend(cases)
                break
            a += 4
    seen = [x for x in seen if start <= x < limit]
    return (max(seen) + 4) if seen else start + 4


def const_code_ptrs(seg, target_seg):
    """Addresses in target_seg's code built in seg's code with lui + addiu/ori."""
    out = set()
    hi = {}
    for a in range(seg.code_start, seg.code_end, 4):
        w = seg.word(a)
        op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if op == 0x0F:
            hi[rt] = (w & 0xFFFF) << 16
            continue
        if op in (0x09, 0x0D) and rs in hi:
            v = (hi[rs] + sext16(w & 0xFFFF)) & 0xFFFFFFFF if op == 0x09 else hi[rs] | (w & 0xFFFF)
            if target_seg.in_code(v) and v % 4 == 0:
                out.add(v)
        if op == 2 or (op == 0 and (w & 63) == 8):
            hi = {}
    return out


def looks_like_start(seg, t):
    if is_prologue(seg.word(t)) or t == seg.code_start:
        return True
    if t - 8 >= seg.code_start:
        p = seg.word(t - 8)
        if p == 0x03E00008 or (p >> 26) == 2:
            return True
    return False


def is_prologue(w):
    return (w & 0xFFFF8000) == 0x27BD8000


def branch_targets(seg, b, e):
    for a in range(b, e, 4):
        w = seg.word(a)
        op = w >> 26
        if op in (1, 4, 5, 6, 7):
            yield a + 4 + (sext16(w & 0xFFFF) << 2)


def func_bounds(seg, starts, data_ptrs, weak=frozenset()):
    starts = set(a for a in starts if seg.in_code(a))
    while True:
        bounds = _func_bounds(seg, starts, data_ptrs)
        st = sorted(starts)
        import bisect
        bad = set()
        for b, e in bounds:
            for t in branch_targets(seg, b, e):
                if b <= t < e or t in starts or not seg.in_code(t):
                    continue
                i = bisect.bisect_right(st, t) - 1
                if i >= 0 and st[i] in weak and st[i] > b:
                    bad.add(st[i])
                # branch backwards into a previous function: the current start is weak
                if t < b and b in weak:
                    bad.add(b)
        if not bad:
            return bounds
        starts -= bad


def _func_bounds(seg, starts, data_ptrs):
    starts = set(starts)
    while True:
        st = sorted(starts)
        bounds, added = [], False
        for b, nxt in zip(st, st[1:] + [seg.code_end]):
            e = reach_end(seg, b, nxt, data_ptrs)
            bounds.append((b, e))
            g = e
            while g < nxt and seg.word(g) == 0:
                g += 4
            if g < nxt and is_prologue(seg.word(g)):
                starts.add(g)
                added = True
        if not added:
            return bounds


# ---------------------------------------------------------------- codegen
R = "c->r"


def reg(n):
    return "0u" if n == 0 else f"{R}[{n}]"


def setreg(n, expr):
    return "" if n == 0 else f"{R}[{n}] = {expr};"


class FuncGen:
    def __init__(self, seg, start, end, ctx, entries=()):
        self.seg, self.start, self.end, self.ctx = seg, start, end, ctx
        self.entries = sorted(entries)
        self.labels = set(self.entries)
        self.unsupported = 0

    def in_func(self, a):
        return self.start <= a < self.end

    # call to an absolute address from this segment
    def call_expr(self, t):
        name = self.ctx["hle_by_addr"].get(t)
        if name:
            return f"hle_{name}(c);"
        main = self.ctx["main"]
        for sg in ((main,) if self.seg.sid == "main" else (main, self.seg)):
            if sg.in_code(t):
                if t in self.ctx["starts"][sg.sid]:
                    return f"{sg.fname(t)}(c);"
                if t in self.ctx["entry_set"][sg.sid]:
                    return f"{sg.fname(t)}_e(c);"
        return f"rt_call(c, 0x{t:08X}u);"

    def simple(self, a, w):
        """Non-control-flow instruction -> C statement."""
        op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
        imm = w & 0xFFFF
        simm = sext16(imm)
        ea = f"({reg(rs)} + {simm}u)" if simm >= 0 else f"({reg(rs)} - {-simm}u)"
        if op == 0:
            S = {
                0x00: lambda: setreg(rd, f"{reg(rt)} << {sa}"),
                0x02: lambda: setreg(rd, f"{reg(rt)} >> {sa}"),
                0x03: lambda: setreg(rd, f"(u32)((s32){reg(rt)} >> {sa})"),
                0x04: lambda: setreg(rd, f"{reg(rt)} << ({reg(rs)} & 31)"),
                0x06: lambda: setreg(rd, f"{reg(rt)} >> ({reg(rs)} & 31)"),
                0x07: lambda: setreg(rd, f"(u32)((s32){reg(rt)} >> ({reg(rs)} & 31))"),
                0x0C: lambda: f"rt_syscall(c, 0x{a:08X}u);",
                0x0D: lambda: f"rt_break(c, 0x{a:08X}u);",
                0x10: lambda: setreg(rd, "c->hi"),
                0x11: lambda: f"c->hi = {reg(rs)};",
                0x12: lambda: setreg(rd, "c->lo"),
                0x13: lambda: f"c->lo = {reg(rs)};",
                0x18: lambda: f"{{ s64 p = (s64)(s32){reg(rs)} * (s32){reg(rt)}; c->lo = (u32)p; c->hi = (u32)(p >> 32); }}",
                0x19: lambda: f"{{ u64 p = (u64){reg(rs)} * {reg(rt)}; c->lo = (u32)p; c->hi = (u32)(p >> 32); }}",
                0x1A: lambda: f"rt_div(c, {reg(rs)}, {reg(rt)});",
                0x1B: lambda: f"rt_divu(c, {reg(rs)}, {reg(rt)});",
                0x20: lambda: setreg(rd, f"{reg(rs)} + {reg(rt)}"),
                0x21: lambda: setreg(rd, f"{reg(rs)} + {reg(rt)}"),
                0x22: lambda: setreg(rd, f"{reg(rs)} - {reg(rt)}"),
                0x23: lambda: setreg(rd, f"{reg(rs)} - {reg(rt)}"),
                0x24: lambda: setreg(rd, f"{reg(rs)} & {reg(rt)}"),
                0x25: lambda: setreg(rd, f"{reg(rs)} | {reg(rt)}"),
                0x26: lambda: setreg(rd, f"{reg(rs)} ^ {reg(rt)}"),
                0x27: lambda: setreg(rd, f"~({reg(rs)} | {reg(rt)})"),
                0x2A: lambda: setreg(rd, f"(s32){reg(rs)} < (s32){reg(rt)}"),
                0x2B: lambda: setreg(rd, f"{reg(rs)} < {reg(rt)}"),
            }
            if fn in S:
                return S[fn]()
        elif op in (0x08, 0x09):
            return setreg(rt, f"{reg(rs)} + {simm}u" if simm >= 0 else f"{reg(rs)} - {-simm}u")
        elif op == 0x0A:
            return setreg(rt, f"(s32){reg(rs)} < {simm}")
        elif op == 0x0B:
            return setreg(rt, f"{reg(rs)} < 0x{simm & 0xFFFFFFFF:X}u")
        elif op == 0x0C:
            return setreg(rt, f"{reg(rs)} & 0x{imm:X}u")
        elif op == 0x0D:
            return setreg(rt, f"{reg(rs)} | 0x{imm:X}u")
        elif op == 0x0E:
            return setreg(rt, f"{reg(rs)} ^ 0x{imm:X}u")
        elif op == 0x0F:
            return setreg(rt, f"0x{imm << 16:08X}u")
        elif op == 0x10:  # COP0
            if rs == 0x00:
                return setreg(rt, f"rt_mfc0(c, {rd})")
            if rs == 0x04:
                return f"rt_mtc0(c, {rd}, {reg(rt)});"
            if rs == 0x10 and fn == 0x10:
                return "rt_rfe(c);"
        elif op == 0x12:  # COP2 / GTE
            if rs == 0x00:
                return setreg(rt, f"gte_mfc2(c, {rd})")
            if rs == 0x02:
                return setreg(rt, f"gte_cfc2(c, {rd})")
            if rs == 0x04:
                return f"gte_mtc2(c, {rd}, {reg(rt)});"
            if rs == 0x06:
                return f"gte_ctc2(c, {rd}, {reg(rt)});"
            if rs & 0x10:
                return f"gte_op(c, 0x{w & 0x1FFFFFF:07X}u);"
        elif op == 0x20:
            return setreg(rt, f"(u32)(s8)rd8({ea})")
        elif op == 0x21:
            return setreg(rt, f"(u32)(s16)rd16({ea})")
        elif op == 0x22:
            return setreg(rt, f"rt_lwl({reg(rt)}, {ea})")
        elif op == 0x23:
            return setreg(rt, f"rd32({ea})") or f"(void)rd32({ea});"
        elif op == 0x24:
            return setreg(rt, f"rd8({ea})")
        elif op == 0x25:
            return setreg(rt, f"rd16({ea})")
        elif op == 0x26:
            return setreg(rt, f"rt_lwr({reg(rt)}, {ea})")
        elif op == 0x28:
            return f"wr8({ea}, (u8){reg(rt)});"
        elif op == 0x29:
            return f"wr16({ea}, (u16){reg(rt)});"
        elif op == 0x2A:
            return f"rt_swl({ea}, {reg(rt)});"
        elif op == 0x2B:
            return f"wr32({ea}, {reg(rt)});"
        elif op == 0x2E:
            return f"rt_swr({ea}, {reg(rt)});"
        elif op == 0x32:
            return f"gte_mtc2(c, {rt}, rd32({ea}));"
        elif op == 0x3A:
            return f"wr32({ea}, gte_mfc2(c, {rt}));"
        self.unsupported += 1
        return f"rt_bad_insn(c, 0x{a:08X}u, 0x{w:08X}u);"

    def is_branch(self, w):
        op = w >> 26
        if op in (2, 3, 4, 5, 6, 7):
            return True
        if op == 1:
            return True
        if op == 0 and (w & 63) in (8, 9):
            return True
        return False

    def collect_labels(self):
        for a in range(self.start, self.end, 4):
            w = self.seg.word(a)
            op = w >> 26
            if op in (4, 5, 6, 7, 1):
                t = a + 4 + (sext16(w & 0xFFFF) << 2)
                if self.in_func(t):
                    self.labels.add(t)
            elif op == 2:
                t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if self.in_func(t):
                    self.labels.add(t)
        self.jt_cases = sorted(t for t in self.ctx["data_ptrs"][self.seg.sid] if self.in_func(t))
        self.labels.update(self.jt_cases)

    def goto_or_tail(self, t):
        if self.in_func(t):
            return f"goto L_{t:08X};"
        return f"{{ {self.call_expr(t)} return; }}"

    def gen(self):
        self.collect_labels()
        seg = self.seg
        name = seg.names.get(self.start, "")
        fn = seg.fname(self.start)
        if self.entries:
            out = [f"/* {name} — has secondary entry points (patch trampolines) */",
                   f"static void {fn}_body(Ctx *c, u32 e_) {{", "    switch (e_) {"]
            out += [f"    case 0x{e:08X}u: goto L_{e:08X};" for e in self.entries]
            out += ["    }"]
        else:
            out = [f"/* {name} */", f"void {fn}(Ctx *c) {{"]
        a = self.start
        while a < self.end:
            w = seg.word(a)
            if a in self.labels:
                out.append(f"L_{a:08X}:;")
            if not self.is_branch(w):
                s = self.simple(a, w)
                if s:
                    out.append("    " + s)
                a += 4
                continue
            # control flow with delay slot
            ds_a = a + 4
            ds = seg.word(ds_a) if ds_a < self.end else 0
            if self.is_branch(ds):
                ds = 0  # branch in delay slot: not emitted by compilers; ignore
            ds_c = self.simple(ds_a, ds) if ds else ""
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            tgt = a + 4 + (sext16(w & 0xFFFF) << 2)
            ret = a + 8
            cond = None
            if op == 4:
                cond = f"{reg(rs)} == {reg(rt)}"
            elif op == 5:
                cond = f"{reg(rs)} != {reg(rt)}"
            elif op == 6:
                cond = f"(s32){reg(rs)} <= 0"
            elif op == 7:
                cond = f"(s32){reg(rs)} > 0"
            elif op == 1:
                kind = rt & 0x11
                cond = f"(s32){reg(rs)} {'<' if (rt & 1) == 0 else '>='} 0"
                if rt & 0x10:  # bltzal / bgezal
                    out.append(f"    {{ int t_ = {cond}; {R}[31] = 0x{ret:08X}u; {ds_c} if (t_) {{ {self.call_expr(tgt)} }} }}")
                    a += 8
                    if ds_a in self.labels:
                        out.append(f"    /* delay slot 0x{ds_a:08X} is also a label */")
                    continue
            if cond is not None:
                if cond == "0u == 0u":
                    out.append(f"    {{ {ds_c} {self.goto_or_tail(tgt)} }}")
                else:
                    out.append(f"    {{ int t_ = {cond}; {ds_c} if (t_) {self.goto_or_tail(tgt)} }}")
            elif op == 2:  # j
                t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                out.append(f"    {{ {ds_c} {self.goto_or_tail(t)} }}")
            elif op == 3:  # jal
                t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if self.ctx["hle_by_addr"].get(t) == "setjmp":
                    out.append(f"    {ds_c}")
                    out.append(f"    if (setjmp(*rt_jmpbuf(c, {R}[4]))) {{ rt_longjmp_restore(c); }} else {{ hle_setjmp(c); }}")
                else:
                    out.append(f"    {ds_c} {R}[31] = 0x{ret:08X}u; {self.call_expr(t)}")
            elif op == 0 and (w & 63) == 8:  # jr
                if rs == 31:
                    out.append(f"    {{ {ds_c} return; }}")
                else:
                    sw = [f"    {{ u32 t_ = {reg(rs)}; {ds_c}"]
                    if self.jt_cases:
                        sw.append("      switch (t_) {")
                        for t in self.jt_cases:
                            sw.append(f"        case 0x{t:08X}u: goto L_{t:08X};")
                        sw.append("      }")
                    sw.append("      rt_call(c, t_); return; }")
                    out.extend(sw)
            elif op == 0 and (w & 63) == 9:  # jalr
                rd = (w >> 11) & 31
                out.append(f"    {{ u32 t_ = {reg(rs)}; {ds_c} {setreg(rd, f'0x{ret:08X}u')} rt_call(c, t_); }}")
            if ds_a in self.labels:
                # something branches into the delay slot: re-emit it as fallthrough target
                out.append(f"    goto L_{ret:08X};")
                out.append(f"L_{ds_a:08X}:; {ds_c}")
                self.labels.add(ret)
            a += 8
        # falling off the end (function was split): continue into the next function
        last = seg.word(self.end - 8) if self.end - 8 >= self.start else 0
        lw = seg.word(self.end - 4)
        ends_flow = (last >> 26 == 2) or (last >> 26 == 0 and (last & 63) == 8) or \
                    (last >> 26 == 4 and ((last >> 16) & 0x3FF) == 0)
        if not ends_flow and not (lw >> 26 == 0 and (lw & 63) == 8):
            out.append(f"    {self.call_expr(self.end)} return;")
        out.append("}")
        # make sure every referenced label exists
        body = "\n".join(out)
        for m in set(re.findall(r"goto L_([0-9A-F]{8});", body)):
            if f"L_{m}:;" not in body:
                body = body[:-1] + f"L_{m}:; rt_bad_label(c, 0x{m}u); return;\n}}"
        if self.entries:
            body += f"\nvoid {fn}(Ctx *c) {{ {fn}_body(c, 0); }}"
            for e in self.entries:
                body += f"\nvoid {seg.fname(e)}_e(Ctx *c) {{ {fn}_body(c, 0x{e:08X}u); }}"
        return body


def data_pointers(segs):
    """Words anywhere in each segment's binary that point into that segment's code."""
    main = segs[0]
    res = {}
    for s in segs:
        ptrs = set()
        for w in s.words:
            if s.in_code(w) and w % 4 == 0:
                ptrs.add(w)
        res[s.sid] = ptrs
    # main-code pointers can also live in overlays (and vice versa for overlay data)
    return res


def main():
    segs = load_segments()
    hle_names = set()
    hle_path = os.path.join(ROOT, "src/pc/hle_list.txt")
    if os.path.exists(hle_path):
        hle_names = {l.split("#")[0].strip() for l in open(hle_path)} - {""}
    main_seg = segs[0]
    hle_by_addr = {a: n for a, n in main_seg.names.items() if n in hle_names}
    starts = {}
    for s in segs:
        st = set(s.names) | jal_targets(s)
        if s.sid == "main":
            for o in segs[1:]:
                st |= {t for t in jal_targets_from(o) if main_seg.in_code(t)}
        else:
            # calls from the EXE into the overlay slot: accept where it looks like a function start
            for t in jal_targets_from(main_seg):
                if s.in_code(t) and (is_prologue(s.word(t)) or t == s.code_start or
                                     (t - 8 >= s.code_start and s.word(t - 8) == 0x03E00008)):
                    st.add(t)
        starts[s.sid] = st
    ctx = {"main": main_seg, "hle_by_addr": hle_by_addr, "starts": starts,
           "data_ptrs": data_pointers(segs)}
    os.makedirs(OUT, exist_ok=True)
    protos, tables = [], []
    stats = {}
    all_bounds = {}
    for s in segs:
        extra = {p for p in ctx["data_ptrs"][s.sid] if is_prologue(s.word(p))}
        cands = const_code_ptrs(s, s) | (const_code_ptrs(main_seg, s) if s.sid != "main" else set())
        if s.sid == "main":
            for o in segs[1:]:
                cands |= const_code_ptrs(o, s)
        for o in segs:
            for a in range(o.code_start, o.code_end, 4):
                w = o.word(a)
                if w >> 26 == 2:
                    cands.add((a & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
        base_bounds = func_bounds(s, starts[s.sid] | extra, ctx["data_ptrs"][s.sid])
        eff_end = max(e for _, e in base_bounds)
        weak = {t for t in cands if s.in_code(t) and t < eff_end and looks_like_start(s, t)}
        if s.sid == "main":
            # English-patch area mixes code with tables: trust any constant pointer into it
            ptrs = const_code_ptrs(s, s)
            for o in segs[1:]:
                ptrs |= const_code_ptrs(o, s)
            weak |= {t for t in ptrs | cands | ctx["data_ptrs"]["main"] if PATCH_LO <= t < PATCH_HI}
        weak -= starts[s.sid] | extra
        bounds = func_bounds(s, starts[s.sid] | extra | weak, ctx["data_ptrs"][s.sid], frozenset(weak))
        starts[s.sid] = {b for b, _ in bounds}
        all_bounds[s.sid] = bounds
    # secondary entries: jumps/branches from other functions into the middle of a function
    import bisect
    entries = {s.sid: {} for s in segs}
    for s in segs:
        for b, e in all_bounds[s.sid]:
            tg = list(branch_targets(s, b, e))
            for a in range(b, e, 4):
                w = s.word(a)
                if w >> 26 == 2:
                    tg.append((a & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
            for t in tg:
                if b <= t < e:
                    continue
                ts = s if s.in_code(t) else (main_seg if main_seg.in_code(t) else None)
                if ts is None or t in starts[ts.sid]:
                    continue
                bl = all_bounds[ts.sid]
                i = bisect.bisect_right([x for x, _ in bl], t) - 1
                if i >= 0 and bl[i][0] <= t < bl[i][1]:
                    entries[ts.sid].setdefault(bl[i][0], set()).add(t)
    ctx["entry_set"] = {sid: set().union(*d.values()) if d else set() for sid, d in entries.items()}
    for s in segs:
        bounds = all_bounds[s.sid]
        chunk, idx, bad = [], 0, 0
        tab = []
        for (b, e) in bounds:
            ents = entries[s.sid].get(b, ())
            fg = FuncGen(s, b, e, ctx, ents)
            chunk.append(fg.gen())
            bad += fg.unsupported
            protos.append(f"void {s.fname(b)}(Ctx *c);")
            tab.append(f"  {{0x{b:08X}u, {s.fname(b)}}},")
            for en in sorted(ents):
                protos.append(f"void {s.fname(en)}_e(Ctx *c);")
                tab.append(f"  {{0x{en:08X}u, {s.fname(en)}_e}},")
            if len(chunk) >= 150:
                write_chunk(s.sid, idx, chunk)
                chunk, idx = [], idx + 1
        if chunk:
            write_chunk(s.sid, idx, chunk)
            idx += 1
        tables.append((s.sid, tab))
        tab.sort(key=lambda l: int(l.split("0x")[1][:8], 16))
        stats[s.sid] = {"funcs": len(bounds), "files": idx, "unsupported_insns": bad,
                        "secondary_entries": len(ctx["entry_set"][s.sid])}
    with open(os.path.join(OUT, "recomp_funcs.h"), "w") as f:
        f.write('#pragma once\n#include "rt.h"\n' + "\n".join(protos) + "\n")
        for n in sorted(hle_names):
            f.write(f"void hle_{n}(Ctx *c);\n")
    with open(os.path.join(OUT, "recomp_tables.c"), "w") as f:
        f.write('#include "recomp_funcs.h"\n')
        for sid, tab in tables:
            f.write(f"const FuncEntry g_funcs_{sid}[] = {{\n" + "\n".join(tab) + "\n  {0, 0}\n};\n")
            f.write(f"const int g_funcs_{sid}_count = {len(tab)};\n")
        f.write("const FuncEntry g_hle_funcs[] = {\n")
        for a, n in sorted(hle_by_addr.items()):
            f.write(f"  {{0x{a:08X}u, hle_{n}}},\n")
        f.write("  {0, 0}\n};\n")
    json.dump(stats, open(os.path.join(OUT, "stats.json"), "w"), indent=1)
    print(json.dumps(stats))


def jal_targets_from(seg):
    out = set()
    for a in range(seg.code_start, seg.code_end, 4):
        w = seg.word(a)
        if w >> 26 == 3:
            out.add((a & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
    return out


def write_chunk(sid, idx, chunk):
    with open(os.path.join(OUT, f"recomp_{sid}_{idx:02d}.c"), "w") as f:
        f.write('#include "recomp_funcs.h"\n#include <setjmp.h>\n\n')
        f.write("\n\n".join(chunk) + "\n")


if __name__ == "__main__":
    main()
